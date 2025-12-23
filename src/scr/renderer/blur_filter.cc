#include "blur_filter.h"

namespace fallout {
namespace renderer {

BlurFilter::BlurFilter() {}
BlurFilter::~BlurFilter() {}

bool BlurFilter::Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;
    // TODO: Implement shader loading
    return true;
}

void BlurFilter::Shutdown(GpuContext& context) {
    // TODO
}

void BlurFilter::Execute(GpuContext& context, void* input, void* output) {
    // TODO
}

void BlurFilter::SetStrength(float strength) {
    mParams.strength = strength;
}

} // namespace renderer
} // namespace fallout
