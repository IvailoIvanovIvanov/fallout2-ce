#include "generic_shader_pass.h"
#include "../../diagnostics.h"
#include <fstream>
#include <sstream>

namespace fallout {
namespace renderer {

GenericShaderPass::GenericShaderPass(const std::string& shaderPath)
    : mShaderPath(shaderPath) {}

bool GenericShaderPass::Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    // We assume this pass runs at the output resolution (post-process)
    // or input resolution (pre-process).
    // The caller (RenderPipeline) sets inputWidth/Height and outputWidth/Height.
    // If they are the same, it's a pre-pass. If different, it's a scaler pass (but this is Generic).
    // We'll use outputWidth/Height as the target dispatch size.
    mWidth = outputWidth;
    mHeight = outputHeight;

    std::ifstream file(mShaderPath);
    if (!file.is_open()) {
        diagnosticsLog(DiagnosticsLevel::Error, "GenericShaderPass", "Failed to open shader file: %s", mShaderPath.c_str());
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    if (!context.CreateComputeShader(source, &mComputeShader)) {
        diagnosticsLog(DiagnosticsLevel::Error, "GenericShaderPass", "Failed to compile shader: %s", mShaderPath.c_str());
        return false;
    }

    return true;
}

void GenericShaderPass::Execute(GpuContext& context, void* input, void* output) {
    if (!mComputeShader) return;

    context.BindTexture(0, input);
    context.BindUnorderedAccessView(1, output);
    context.Dispatch(mComputeShader, (mWidth + 15) / 16, (mHeight + 15) / 16, 1);
}

void GenericShaderPass::Shutdown(GpuContext& context) {
    // Shader destruction is not explicitly exposed in GpuContext yet, 
    // but usually handled by context shutdown or we should add DestroyShader.
    // For now, we assume context cleanup handles it or it's a leak we need to fix in GpuContext.
}

} // namespace renderer
} // namespace fallout
