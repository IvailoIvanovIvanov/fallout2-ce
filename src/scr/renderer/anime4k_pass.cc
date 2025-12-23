#include "anime4k_pass.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <regex>
#include <map>

namespace fallout {
namespace renderer {

Anime4kPass::Anime4kPass() {}
Anime4kPass::~Anime4kPass() {}

bool Anime4kPass::Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;

    return LoadShader(context);
}

void Anime4kPass::Shutdown(GpuContext& context) {
    for (auto& pair : mTextures) {
        if (pair.first != "MAIN" && pair.first != "INPUT" && pair.first != "HOOKED") {
             context.DestroyTexture(pair.second);
        }
    }
    mTextures.clear();
    mPasses.clear();
}

void Anime4kPass::SetVersion(Version version) {
    if (mVersion != version) {
        mVersion = version;
        mDirty = true;
    }
}

void* Anime4kPass::GetTexture(const std::string& name) {
    auto it = mTextures.find(name);
    if (it != mTextures.end()) {
        return it->second;
    }
    return nullptr;
}

void Anime4kPass::CreateTexture(GpuContext& context, const std::string& name, int width, int height) {
    if (mTextures.find(name) != mTextures.end()) return;

    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = TextureFormat::RGBA16F; 
    
    void* tex = context.CreateTexture(desc);
    mTextures[name] = tex;
}

bool Anime4kPass::LoadShader(GpuContext& context) {
    std::string filename = "data/shaders/Anime4K_Upscale_GAN_x4_UUL.glsl";
    if (mVersion == Version::V3_2) {
        filename = "data/shaders/anime4k.glsl";
    }

    std::ifstream file(filename);
    if (!file.is_open()) {        
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    if (mVersion == Version::GAN_X4_UUL || filename.find("GAN") != std::string::npos) {
        return ParseMPVShader(context, source);
    } else {
        void* shader = nullptr;
        if (!context.CreateComputeShader(source, &shader)) return false;
        
        Pass pass;
        pass.shader = shader;
        pass.width = mOutputWidth;
        pass.height = mOutputHeight;
        mPasses.push_back(pass);
        return true;
    }
}

bool Anime4kPass::ParseMPVShader(GpuContext& context, const std::string& source) {
    std::stringstream ss(source);
    std::string line;
    
    Pass currentPass;
    std::string currentSource;
    bool inPass = false;
    
    std::map<std::string, std::pair<int, int>> textureSizes;
    textureSizes["MAIN"] = {mInputWidth, mInputHeight};
    textureSizes["INPUT"] = {mInputWidth, mInputHeight};
    textureSizes["HOOKED"] = {mInputWidth, mInputHeight};
    textureSizes["OUTPUT"] = {mOutputWidth, mOutputHeight};

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
            shaderSrc << "    " << texName << "_pos = (vec2(pos) + 0.5) * " << texName << "_pt;\n";
        }
        shaderSrc << "    vec4 color = hook();\n";
        shaderSrc << "    imageStore(outputImage, pos, color);\n";
        shaderSrc << "}\n";

        void* shader = nullptr;
        if (context.CreateComputeShader(shaderSrc.str(), &shader)) {
            currentPass.shader = shader;
            mPasses.push_back(currentPass);
            
            // Update texture sizes for the output of this pass
            if (!currentPass.outputTexture.empty()) {
                textureSizes[currentPass.outputTexture] = {currentPass.width, currentPass.height};
                // If output is MAIN, update MAIN/HOOKED for next passes?
                // In MPV, saving to MAIN updates the chain.
                if (currentPass.outputTexture == "MAIN") {
                    textureSizes["HOOKED"] = {currentPass.width, currentPass.height};
                    textureSizes["MAIN"] = {currentPass.width, currentPass.height};
                }
            }
        } else {
            
        }

        currentPass = Pass();
        currentSource = "";
        inPass = false;
    };

    auto ParseDimension = [&](const std::string& line, int defaultVal, bool isWidth) -> int {
        std::regex reName("([\\w]+)\\.[wh]");
        std::smatch match;
        int baseVal = defaultVal;
        
        if (std::regex_search(line, match, reName)) {
            std::string refName = match[1];
            if (textureSizes.count(refName)) {
                baseVal = isWidth ? textureSizes[refName].first : textureSizes[refName].second;
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
    };

    while (std::getline(ss, line)) {
        if (line.find("//!HOOK MAIN") != std::string::npos) {
            FinishPass();
            inPass = true;
            // Default to input size (or current chain size?)
            // Usually defaults to HOOKED size.
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
                // Do not rename MAIN to INPUT. Keep it as MAIN.
                currentPass.inputTextures.push_back(name);
            }
        } else if (line.find("//!SAVE") != std::string::npos) {
            std::regex re("//!SAVE\\s+(\\w+)");
            std::smatch match;
            if (std::regex_search(line, match, re)) {
                currentPass.outputTexture = match[1];
            }
        } else if (line.find("//!WIDTH") != std::string::npos) {
             currentPass.width = ParseDimension(line, mInputWidth, true);
        } else if (line.find("//!HEIGHT") != std::string::npos) {
             currentPass.height = ParseDimension(line, mInputHeight, false);
        } else if (line.find("//!") == 0) {
            // Ignore
        } else {
            currentSource += line + "\n";
        }
    }
    FinishPass();
    
    for (const auto& pass : mPasses) {
        if (!pass.outputTexture.empty() && pass.outputTexture != "MAIN") {
            CreateTexture(context, pass.outputTexture, pass.width, pass.height);
        }
    }

    return !mPasses.empty();
}

void Anime4kPass::Execute(GpuContext& context, void* input, void* output) {
    if (mDirty) {
        Shutdown(context);
        LoadShader(context);
        mDirty = false;
    }

    mTextures["INPUT"] = input;
    mTextures["HOOKED"] = input; 
    mTextures["MAIN"] = input; 

    void* currentHooked = input;
    
    std::map<std::string, std::pair<int, int>> sizes;
    sizes["INPUT"] = {mInputWidth, mInputHeight};
    sizes["HOOKED"] = {mInputWidth, mInputHeight};
    sizes["MAIN"] = {mInputWidth, mInputHeight};

    for (const auto& pass : mPasses) {
        if (!pass.shader) continue;

        void* passOutput = nullptr;
        if (pass.outputTexture == "MAIN" || pass.outputTexture.empty()) {
            passOutput = output;
        } else {
            passOutput = GetTexture(pass.outputTexture);
            if (!passOutput) {
                CreateTexture(context, pass.outputTexture, pass.width, pass.height);
                passOutput = GetTexture(pass.outputTexture);
            }
        }

        context.BindUnorderedAccessView(0, passOutput);

        int slot = 1;
        for (const auto& texName : pass.inputTextures) {
            void* tex = nullptr;
            
            if (texName == "HOOKED") {
                tex = currentHooked;
            } else if (texName == "INPUT") {
                tex = input;
            } else {
                tex = GetTexture(texName);
            }
            
            if (!tex) tex = input; 
            context.BindTexture(slot, tex);
            
            float w = (float)mInputWidth;
            float h = (float)mInputHeight;
            
            if (sizes.count(texName)) {
                w = (float)sizes[texName].first;
                h = (float)sizes[texName].second;
            } else if (tex == output) {
                 w = (float)mOutputWidth;
                 h = (float)mOutputHeight;
            }
            
            float pt[2] = { 1.0f / w, 1.0f / h };
            struct { float w, h; float ptx, pty; } constants = { w, h, pt[0], pt[1] };
            context.SetConstants(slot, &constants, sizeof(constants));
            
            slot++;
        }

        int groupX = (pass.width + 7) / 8;
        int groupY = (pass.height + 7) / 8;
        context.Dispatch(pass.shader, groupX, groupY, 1);
        
        if (!pass.outputTexture.empty()) {
            sizes[pass.outputTexture] = {pass.width, pass.height};
        }
        
        if (pass.outputTexture == "MAIN") {
            currentHooked = output;
            sizes["HOOKED"] = {pass.width, pass.height};
            sizes["MAIN"] = {pass.width, pass.height};
        }
    }
}

} // namespace renderer
} // namespace fallout
