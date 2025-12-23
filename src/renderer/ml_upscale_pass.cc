#include "ml_upscale_pass.h"

namespace fallout {
namespace renderer {

MLUpscalePass::MLUpscalePass() {}
MLUpscalePass::~MLUpscalePass() {}

bool MLUpscalePass::Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    return true;
}

void MLUpscalePass::Shutdown(GpuContext& context) {}

void MLUpscalePass::SetModelFile(const std::string& modelFile) {
    mModelFile = modelFile;
}

void MLUpscalePass::Execute(GpuContext& context, void* input, void* output) {}

} // namespace renderer
} // namespace fallout
