#include "hdr_filter.h"

namespace fallout {
namespace renderer {

HDRFilter::HDRFilter() {}
HDRFilter::~HDRFilter() {}

bool HDRFilter::Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    return true;
}

void HDRFilter::Shutdown(GpuContext& context) {}

void HDRFilter::SetParams(float saturation, float contrast, float blackCrushThreshold, float blackCrushStrength) {
    mSaturation = saturation;
    mContrast = contrast;
    mBlackCrushThreshold = blackCrushThreshold;
    mBlackCrushStrength = blackCrushStrength;
}

void HDRFilter::Execute(GpuContext& context, void* input, void* output) {}

} // namespace renderer
} // namespace fallout
