#include "mpv_shader_parser.h"
#include "logger.h"
#include <sstream>
#include <regex>

namespace fallout {
namespace renderer {

std::vector<MPVPass> MPVShaderParser::Parse(const std::string& source, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    std::vector<MPVPass> passes;
    std::stringstream ss(source);
    std::string line;
    
    MPVPass currentPass;
    std::string currentSource;
    bool inPass = false;
    
    std::map<std::string, std::pair<int, int>> textureSizes;
    textureSizes["MAIN"] = {inputWidth, inputHeight};
    textureSizes["INPUT"] = {inputWidth, inputHeight};
    textureSizes["HOOKED"] = {inputWidth, inputHeight};
    textureSizes["OUTPUT"] = {outputWidth, outputHeight};

    auto FinishPass = [&]() {
        if (!inPass) return;
        
        std::stringstream shaderSrc;
        shaderSrc << "#version 430\n";
        shaderSrc << "layout(local_size_x = 8, local_size_y = 8) in;\n";
        shaderSrc << "layout(rgba16f, binding = 0) writeonly uniform image2D outputImage;\n";
        
        int binding = 1;
        for (const auto& texName : currentPass.inputTextures) {
            shaderSrc << "layout(binding = " << binding << ") uniform sampler2D " << texName << ";\n";
            shaderSrc << "layout(std140, binding = " << binding << ") uniform " << texName << "_Constants {\n";
            shaderSrc << "    vec2 " << texName << "_size;\n";
            shaderSrc << "    vec2 " << texName << "_pt;\n";
            shaderSrc << "};\n";
            
            shaderSrc << "vec4 " << texName << "_tex(vec2 pos) { return texture(" << texName << ", pos); }\n";
            shaderSrc << "vec4 " << texName << "_texOff(vec2 off) { return texture(" << texName << ", (vec2(gl_GlobalInvocationID.xy) + off + 0.5) * " << texName << "_pt); }\n";
            shaderSrc << "vec2 " << texName << "_pos;\n";
            binding++;
        }

        shaderSrc << currentSource << "\n";

        shaderSrc << "void main() {\n";
        shaderSrc << "    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);\n";
        shaderSrc << "    vec2 size = vec2(imageSize(outputImage));\n"; 
        shaderSrc << "    if (pos.x >= size.x || pos.y >= size.y) return;\n";
        for (const auto& texName : currentPass.inputTextures) {
            shaderSrc << "    " << texName << "_pos = (vec2(pos) + 0.5) / vec2(imageSize(outputImage));\n";
        }
        shaderSrc << "    vec4 color = hook();\n";
        shaderSrc << "    imageStore(outputImage, pos, color);\n";
        shaderSrc << "}\n";

        auto shader = Shader::CreateFromSource(shaderSrc.str());
        if (shader && shader->IsValid()) {
            currentPass.shader = std::move(shader);
            passes.push_back(currentPass);
            
            if (!currentPass.outputTexture.empty()) {
                textureSizes[currentPass.outputTexture] = {currentPass.width, currentPass.height};
                if (currentPass.outputTexture == "MAIN") {
                    textureSizes["HOOKED"] = {currentPass.width, currentPass.height};
                    textureSizes["MAIN"] = {currentPass.width, currentPass.height};
                }
            }
        } else {
            Logger::Log(LogLevel::Error, "MPVShaderParser: Failed to compile MPV pass");
        }

        currentPass = MPVPass();
        currentSource = "";
        inPass = false;
    };

    while (std::getline(ss, line)) {
        if (line.find("//!HOOK MAIN") != std::string::npos) {
            FinishPass();
            inPass = true;
            currentPass.width = textureSizes["HOOKED"].first;
            currentPass.height = textureSizes["HOOKED"].second;
            continue;
        }

        if (!inPass) continue;

        if (line.find("//!BIND") != std::string::npos) {
            std::regex re("//!BIND\\s+(\\w+)");
            std::smatch match;
            if (std::regex_search(line, match, re)) {
                std::string name = match[1];
                currentPass.inputTextures.push_back(name);
            }
        } else if (line.find("//!SAVE") != std::string::npos) {
            std::regex re("//!SAVE\\s+(\\w+)");
            std::smatch match;
            if (std::regex_search(line, match, re)) {
                currentPass.outputTexture = match[1];
            }
        } else if (line.find("//!WIDTH") != std::string::npos) {
             currentPass.width = ParseDimension(line, inputWidth, true, textureSizes);
        } else if (line.find("//!HEIGHT") != std::string::npos) {
             currentPass.height = ParseDimension(line, inputHeight, false, textureSizes);
        } else if (line.find("//!") == 0) {
            // Ignore
        } else {
            currentSource += line + "\n";
        }
    }
    FinishPass();
    
    return passes;
}

int MPVShaderParser::ParseDimension(const std::string& line, int defaultVal, bool isWidth, const std::map<std::string, std::pair<int, int>>& textureSizes) {
    std::regex reName("([\\w]+)\\.[wh]");
    std::smatch match;
    int baseVal = defaultVal;
    
    if (std::regex_search(line, match, reName)) {
        std::string refName = match[1];
        if (textureSizes.count(refName)) {
            baseVal = isWidth ? textureSizes.at(refName).first : textureSizes.at(refName).second;
        }
    }
    
    float mult = 1.0f;
    std::regex reMult("([0-9.]+)\\s*\\*");
    if (std::regex_search(line, match, reMult)) {
        mult = std::stof(match[1]);
    } else {
        std::regex reMult2("\\*\\s*([0-9.]+)");
        if (std::regex_search(line, match, reMult2)) {
            mult = std::stof(match[1]);
        }
    }
    
    return (int)(baseVal * mult);
}

} // namespace renderer
} // namespace fallout
