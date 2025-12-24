#ifndef FALLOUT_RENDERER_GENERIC_SHADER_PASS_H
#define FALLOUT_RENDERER_GENERIC_SHADER_PASS_H

#include "shader_pass.h"
#include "shader.h"
#include "mpv_shader_parser.h"
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace fallout {
namespace renderer {

class GenericShaderPass : public ShaderPass {
public:
    GenericShaderPass(const std::string& shaderPath);
    ~GenericShaderPass() override = default;

    bool Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) override;
    void Execute(GpuContext& context, void* input, void* output) override;
    void Shutdown(GpuContext& context) override;

private:
    void* GetTexture(const std::string& name);
    void CreateTexture(GpuContext& context, const std::string& name, int width, int height);

    std::string mShaderPath;
    
    // For standard shaders
    std::unique_ptr<Shader> mShader;
    int mWidth = 0;
    int mHeight = 0;

    // For MPV shaders
    bool mIsMPV = false;
    std::vector<MPVPass> mPasses;
    std::map<std::string, void*> mTextures;
    
    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_GENERIC_SHADER_PASS_H
