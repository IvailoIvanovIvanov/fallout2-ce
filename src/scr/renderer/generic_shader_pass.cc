#include "generic_shader_pass.h"
#include "mpv_shader_parser.h"
#include "logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace fallout {
namespace renderer {

struct ScalerConstants {
    int inputWidth;
    int inputHeight;
    int outputWidth;
    int outputHeight;
    float offsetX;
    float offsetY;
    float scaleX;
    float scaleY;
};

GenericShaderPass::GenericShaderPass(const std::string& shaderPath)
    : mShaderPath(shaderPath) {}

bool GenericShaderPass::Init(GpuContext& context, const RenderSurface& input, const RenderSurface& output) {
    std::ifstream file(mShaderPath);
    if (!file.is_open()) {
        // Try looking in ../data/shaders/ (useful for dev builds)
        std::string altPath = "../" + mShaderPath;
        file.open(altPath);
        if (file.is_open()) {
            mShaderPath = altPath;
        } else {
            // Try looking in ../../data/shaders/
            altPath = "../../" + mShaderPath;
            file.open(altPath);
            if (file.is_open()) {
                mShaderPath = altPath;
            } else {
                Logger::Log(LogLevel::Error, "GenericShaderPass: Failed to open shader file: %s", mShaderPath.c_str());
                return false;
            }
        }
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

        // Load Blit Shader for resizing if needed
        mBlitShader = std::make_unique<Shader>("data/shaders/scaler.glsl");
        if (!mBlitShader->IsValid()) {
             Logger::Log(LogLevel::Warning, "GenericShaderPass: Failed to load scaler.glsl for MPV resize support");
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
        context.BindUnorderedAccessView(1, output.handle, output.format);
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
        TextureFormat passFormat = TextureFormat::RGBA16F;
        bool needsBlit = false;

        if (pass.outputTexture == "MAIN" || pass.outputTexture.empty()) {
            if (pass.width != output.width || pass.height != output.height) {
                // Use intermediate buffer
                std::string internalName = "MAIN_INTERNAL";
                passOutput = GetTexture(internalName);
                
                // Check if existing buffer matches size
                if (passOutput) {
                    auto it = sizes.find(internalName);
                    if (it != sizes.end()) {
                        if (it->second.first != pass.width || it->second.second != pass.height) {
                            context.DestroyTexture(passOutput);
                            mTextures.erase(internalName);
                            passOutput = nullptr;
                        }
                    }
                }

                if (!passOutput) {
                    CreateTexture(context, internalName, pass.width, pass.height);
                    passOutput = GetTexture(internalName);
                    sizes[internalName] = {pass.width, pass.height};
                }
                
                passFormat = TextureFormat::RGBA16F;
                needsBlit = true;
            } else {
                passOutput = output.handle;
                passFormat = output.format;
            }
        } else {
            passOutput = GetTexture(pass.outputTexture);
            if (!passOutput) {
                CreateTexture(context, pass.outputTexture, pass.width, pass.height);
                passOutput = GetTexture(pass.outputTexture);
            }
        }

        context.BindUnorderedAccessView(0, passOutput, passFormat);

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
            currentHooked = passOutput;
            sizes["HOOKED"] = {pass.width, pass.height};
            sizes["MAIN"] = {pass.width, pass.height};
            
            if (needsBlit && mBlitShader && mBlitShader->IsValid()) {
                // Blit from passOutput to output.handle
                mBlitShader->Use();
                
                ScalerConstants constants;
                constants.inputWidth = pass.width;
                constants.inputHeight = pass.height;
                constants.outputWidth = output.width;
                constants.outputHeight = output.height;
                constants.offsetX = 0.0f;
                constants.offsetY = 0.0f;
                constants.scaleX = (float)output.width / pass.width;
                constants.scaleY = (float)output.height / pass.height;
                
                context.SetConstants(0, &constants, sizeof(constants));
                context.BindTexture(0, passOutput);
                context.BindUnorderedAccessView(1, output.handle, output.format);
                
                int bx = (output.width + 7) / 8;
                int by = (output.height + 7) / 8;
                mBlitShader->Dispatch(bx, by, 1);
            }
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
