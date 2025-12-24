#include "generic_shader_pass.h"
#include "mpv_shader_parser.h"
#include "logger.h"
#include <fstream>
#include <sstream>

namespace fallout {
namespace renderer {

GenericShaderPass::GenericShaderPass(const std::string& shaderPath)
    : mShaderPath(shaderPath) {}

bool GenericShaderPass::Init(GpuContext& context, const RenderSurface& input, const RenderSurface& output) {
    std::ifstream file(mShaderPath);
    if (!file.is_open()) {
        Logger::Log(LogLevel::Error, "GenericShaderPass: Failed to open shader file: %s", mShaderPath.c_str());
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    // Check if it's an MPV shader
    if (source.find("//!HOOK") != std::string::npos) {
        mIsMPV = true;
        mPasses = MPVShaderParser::Parse(source, input.width, input.height, output.width, output.height);
        
        for (const auto& pass : mPasses) {
            if (!pass.outputTexture.empty() && pass.outputTexture != "MAIN") {
                CreateTexture(context, pass.outputTexture, pass.width, pass.height);
            }
        }
        
        Logger::Log(LogLevel::Info, "GenericShaderPass: Successfully initialized MPV shader: %s", mShaderPath.c_str());
        return !mPasses.empty();
    }

    // Standard Compute Shader
    mIsMPV = false;
    mShader = Shader::CreateFromSource(source);
    if (!mShader || !mShader->IsValid()) {
        Logger::Log(LogLevel::Error, "GenericShaderPass: Failed to compile shader: %s", mShaderPath.c_str());
        return false;
    }

    Logger::Log(LogLevel::Info, "GenericShaderPass: Successfully initialized shader: %s", mShaderPath.c_str());
    return true;
}

std::string GenericShaderPass::GetName() const {
    // Extract filename from path
    size_t lastSlash = mShaderPath.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        return mShaderPath.substr(lastSlash + 1);
    }
    return mShaderPath;
}

void GenericShaderPass::Execute(GpuContext& context, const RenderSurface& input, const RenderSurface& output) {
    if (!mIsMPV) {
        if (!mShader || !mShader->IsValid()) return;
        context.BindTexture(0, input.handle);
        context.BindUnorderedAccessView(1, output.handle);
        mShader->Dispatch((output.width + 15) / 16, (output.height + 15) / 16, 1);
        return;
    }

    // MPV Execution Logic
    mTextures["INPUT"] = input.handle;
    mTextures["HOOKED"] = input.handle; 
    mTextures["MAIN"] = input.handle; 

    void* currentHooked = input.handle;
    
    std::map<std::string, std::pair<int, int>> sizes;
    sizes["INPUT"] = {input.width, input.height};
    sizes["HOOKED"] = {input.width, input.height};
    sizes["MAIN"] = {input.width, input.height};

    for (const auto& pass : mPasses) {
        if (!pass.shader) continue;

        void* passOutput = nullptr;
        if (pass.outputTexture == "MAIN" || pass.outputTexture.empty()) {
            passOutput = output.handle;
        } else {
            passOutput = GetTexture(pass.outputTexture);
            if (!passOutput) {
                CreateTexture(context, pass.outputTexture, pass.width, pass.height);
                passOutput = GetTexture(pass.outputTexture);
            }
        }

        context.BindUnorderedAccessView(0, passOutput);

        if (pass.shader) pass.shader->Use();

        int slot = 1;
        for (const auto& texName : pass.inputTextures) {
            void* tex = nullptr;
            
            if (texName == "HOOKED") {
                tex = currentHooked;
            } else if (texName == "INPUT") {
                tex = input.handle;
            } else {
                tex = GetTexture(texName);
            }
            
            if (!tex) tex = input.handle; 
            context.BindTexture(slot, tex);
            
            float w = (float)input.width;
            float h = (float)input.height;
            
            if (sizes.count(texName)) {
                w = (float)sizes[texName].first;
                h = (float)sizes[texName].second;
            } else if (tex == output.handle) {
                 w = (float)output.width;
                 h = (float)output.height;
            }
            
            float pt[2] = { 1.0f / w, 1.0f / h };
            
            if (pass.shader) {
                pass.shader->SetVec2(texName + "_size", w, h);
                pass.shader->SetVec2(texName + "_pt", pt[0], pt[1]);
            }
            
            slot++;
        }

        int groupX = (pass.width + 7) / 8;
        int groupY = (pass.height + 7) / 8;
        if (pass.shader) pass.shader->Dispatch(groupX, groupY, 1);
        
        if (!pass.outputTexture.empty()) {
            sizes[pass.outputTexture] = {pass.width, pass.height};
        }
        
        if (pass.outputTexture == "MAIN") {
            currentHooked = output.handle;
            sizes["HOOKED"] = {pass.width, pass.height};
            sizes["MAIN"] = {pass.width, pass.height};
        }
    }
}

void GenericShaderPass::Shutdown(GpuContext& context) {
    if (mIsMPV) {
        for (auto& pair : mTextures) {
            if (pair.first != "MAIN" && pair.first != "INPUT" && pair.first != "HOOKED") {
                 context.DestroyTexture(pair.second);
            }
        }
        mTextures.clear();
        mPasses.clear();
    } else {
        // Standard shader cleanup if needed
    }
}

void* GenericShaderPass::GetTexture(const std::string& name) {
    auto it = mTextures.find(name);
    if (it != mTextures.end()) {
        return it->second;
    }
    return nullptr;
}

void GenericShaderPass::CreateTexture(GpuContext& context, const std::string& name, int width, int height) {
    if (mTextures.find(name) != mTextures.end()) return;

    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = TextureFormat::RGBA16F; 
    
    void* tex = context.CreateTexture(desc);
    mTextures[name] = tex;
}



} // namespace renderer
} // namespace fallout
