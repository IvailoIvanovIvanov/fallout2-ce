#include "generic_shader_pass.h"
#include "mpv_shader_parser.h"
#include "logger.h"
#include "render_types.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace fallout {
namespace renderer {

//-----------------------------------------------------------------------------
// Construction
//-----------------------------------------------------------------------------

GenericShaderPass::GenericShaderPass(const std::string& shaderPath)
    : mShaderPath(shaderPath) {}

//-----------------------------------------------------------------------------
// Lifecycle
//-----------------------------------------------------------------------------

bool GenericShaderPass::Init(GpuContext& context, const RenderSurface& input, const RenderSurface& output) {
    std::string source;
    if (!LoadShaderSource(source)) {
        Logger::Log(LogLevel::Error, "GenericShaderPass: Failed to open shader file: %s", mShaderPath.c_str());
        return false;
    }

    // Detect shader format and initialize appropriately
    if (source.find("//!HOOK") != std::string::npos) {
        return InitMPVShader(context, source, input, output);
    }
    return InitStandardShader(source);
}

bool GenericShaderPass::LoadShaderSource(std::string& source) {
    // Try multiple search paths
    const std::string paths[] = {
        mShaderPath,
        "../" + mShaderPath,
        "../../" + mShaderPath
    };

    for (const auto& path : paths) {
        std::ifstream file(path);
        if (file.is_open()) {
            mShaderPath = path;
            std::stringstream buffer;
            buffer << file.rdbuf();
            source = buffer.str();
            return true;
        }
    }
    return false;
}

bool GenericShaderPass::InitMPVShader(GpuContext& context, const std::string& source,
                                       const RenderSurface& input, const RenderSurface& output) {
    mIsMPV = true;
    mPasses = MPVShaderParser::Parse(source, input.width, input.height, output.width, output.height);

    // Pre-create textures for named outputs
    for (const auto& pass : mPasses) {
        if (!pass.outputTexture.empty() && pass.outputTexture != "MAIN") {
            CreateTexture(context, pass.outputTexture, pass.width, pass.height);
        }
    }

    // Load blit shader for final resize if needed
    mBlitShader = std::make_unique<Shader>("data/shaders/scaler.glsl");
    if (!mBlitShader->IsValid()) {
        Logger::Log(LogLevel::Warning, "GenericShaderPass: Failed to load scaler.glsl for MPV resize support");
    }

    Logger::Log(LogLevel::Info, "GenericShaderPass: Successfully initialized MPV shader: %s", mShaderPath.c_str());
    return !mPasses.empty();
}

bool GenericShaderPass::InitStandardShader(const std::string& source) {
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
    size_t lastSlash = mShaderPath.find_last_of("/\\");
    return (lastSlash != std::string::npos) ? mShaderPath.substr(lastSlash + 1) : mShaderPath;
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
    }
}

//-----------------------------------------------------------------------------
// Execution
//-----------------------------------------------------------------------------

void GenericShaderPass::Execute(GpuContext& context, const RenderSurface& input, const RenderSurface& output) {
    if (mIsMPV) {
        ExecuteMPVChain(context, input, output);
    } else {
        ExecuteStandardShader(context, input, output);
    }
}

void GenericShaderPass::ExecuteStandardShader(GpuContext& context, const RenderSurface& input,
                                               const RenderSurface& output) {
    if (!mShader || !mShader->IsValid()) return;

    context.BindTexture(0, input.handle);
    context.BindUnorderedAccessView(1, output.handle, output.format);
    mShader->Dispatch((output.width + 15) / 16, (output.height + 15) / 16, 1);
}

void GenericShaderPass::ExecuteMPVChain(GpuContext& context, const RenderSurface& input,
                                         const RenderSurface& output) {
    // Initialize texture references
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
        ExecuteMPVPass(context, pass, input, output, currentHooked, sizes);
    }
}

void GenericShaderPass::ExecuteMPVPass(GpuContext& context, const MPVPass& pass,
                                        const RenderSurface& input, const RenderSurface& output,
                                        void*& currentHooked,
                                        std::map<std::string, std::pair<int, int>>& sizes) {
    TextureFormat passFormat = TextureFormat::RGBA16F;
    bool needsBlit = false;

    void* passOutput = ResolvePassOutput(context, pass, output, sizes, passFormat, needsBlit);
    
    context.BindUnorderedAccessView(0, passOutput, passFormat);
    pass.shader->Use();

    BindPassInputs(context, pass, input, currentHooked, sizes);

    int groupX = (pass.width + 7) / 8;
    int groupY = (pass.height + 7) / 8;
    pass.shader->Dispatch(groupX, groupY, 1);

    // Update size tracking
    if (!pass.outputTexture.empty()) {
        sizes[pass.outputTexture] = {pass.width, pass.height};
    }

    // Handle MAIN output updates
    if (pass.outputTexture == "MAIN") {
        currentHooked = passOutput;
        sizes["HOOKED"] = {pass.width, pass.height};
        sizes["MAIN"] = {pass.width, pass.height};

        if (needsBlit) {
            BlitToOutput(context, passOutput, pass.width, pass.height, output);
        }
    }
}

void* GenericShaderPass::ResolvePassOutput(GpuContext& context, const MPVPass& pass,
                                            const RenderSurface& output,
                                            std::map<std::string, std::pair<int, int>>& sizes,
                                            TextureFormat& outFormat, bool& needsBlit) {
    needsBlit = false;

    if (pass.outputTexture == "MAIN" || pass.outputTexture.empty()) {
        // Check if sizes match
        if (pass.width != output.width || pass.height != output.height) {
            // Need intermediate buffer
            std::string internalName = "MAIN_INTERNAL";
            void* tex = GetTexture(internalName);

            // Verify existing buffer size matches
            if (tex) {
                auto it = sizes.find(internalName);
                if (it != sizes.end() && 
                    (it->second.first != pass.width || it->second.second != pass.height)) {
                    context.DestroyTexture(tex);
                    mTextures.erase(internalName);
                    tex = nullptr;
                }
            }

            if (!tex) {
                CreateTexture(context, internalName, pass.width, pass.height);
                tex = GetTexture(internalName);
                sizes[internalName] = {pass.width, pass.height};
            }

            outFormat = TextureFormat::RGBA16F;
            needsBlit = true;
            return tex;
        }

        outFormat = output.format;
        return output.handle;
    }

    // Named output texture
    void* tex = GetTexture(pass.outputTexture);
    if (!tex) {
        CreateTexture(context, pass.outputTexture, pass.width, pass.height);
        tex = GetTexture(pass.outputTexture);
    }
    return tex;
}

void GenericShaderPass::BindPassInputs(GpuContext& context, const MPVPass& pass,
                                        const RenderSurface& input, void* currentHooked,
                                        const std::map<std::string, std::pair<int, int>>& sizes) {
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

        // Determine texture size for uniforms
        float w = static_cast<float>(input.width);
        float h = static_cast<float>(input.height);

        auto it = sizes.find(texName);
        if (it != sizes.end()) {
            w = static_cast<float>(it->second.first);
            h = static_cast<float>(it->second.second);
        }

        float pt[2] = {1.0f / w, 1.0f / h};
        pass.shader->SetVec2(texName + "_size", w, h);
        pass.shader->SetVec2(texName + "_pt", pt[0], pt[1]);

        slot++;
    }
}

void GenericShaderPass::BlitToOutput(GpuContext& context, void* source,
                                      int srcWidth, int srcHeight,
                                      const RenderSurface& output) {
    if (!mBlitShader || !mBlitShader->IsValid()) return;

    mBlitShader->Use();

    ScalerConstants constants;
    constants.inputWidth = srcWidth;
    constants.inputHeight = srcHeight;
    constants.outputWidth = output.width;
    constants.outputHeight = output.height;
    constants.offsetX = 0.0f;
    constants.offsetY = 0.0f;
    constants.scaleX = static_cast<float>(output.width) / srcWidth;
    constants.scaleY = static_cast<float>(output.height) / srcHeight;

    context.SetConstants(0, &constants, sizeof(constants));
    context.BindTexture(0, source);
    context.BindUnorderedAccessView(1, output.handle, output.format);

    int groupX = (output.width + 7) / 8;
    int groupY = (output.height + 7) / 8;
    mBlitShader->Dispatch(groupX, groupY, 1);
}

//-----------------------------------------------------------------------------
// Texture Management
//-----------------------------------------------------------------------------

void* GenericShaderPass::GetTexture(const std::string& name) {
    auto it = mTextures.find(name);
    return (it != mTextures.end()) ? it->second : nullptr;
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
