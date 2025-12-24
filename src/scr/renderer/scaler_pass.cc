#include "scaler_pass.h"
#include "logger.h"
#include <string>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>

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

ScalerPass::ScalerPass() {}
ScalerPass::~ScalerPass() {}

bool ScalerPass::Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;

    mShader = std::make_unique<Shader>("data/shaders/scaler.glsl");
    if (!mShader->IsValid()) {
        Logger::Log(LogLevel::Error, "ScalerPass: Failed to compile shader");
        return false;
    }
    return true;
}

void ScalerPass::Shutdown(GpuContext& context) {
    mShader.reset();
}

void ScalerPass::Execute(GpuContext& context, void* input, void* output) {
    if (!mShader || !mShader->IsValid()) return;

    // Calculate scale and offset to preserve aspect ratio
    float scaleX = (float)mOutputWidth / mInputWidth;
    float scaleY = (float)mOutputHeight / mInputHeight;
    float scale = std::min(scaleX, scaleY);

    float offsetX = (mOutputWidth - mInputWidth * scale) * 0.5f;
    float offsetY = (mOutputHeight - mInputHeight * scale) * 0.5f;

    ScalerConstants constants;
    constants.inputWidth = mInputWidth;
    constants.inputHeight = mInputHeight;
    constants.outputWidth = mOutputWidth;
    constants.outputHeight = mOutputHeight;
    constants.offsetX = offsetX;
    constants.offsetY = offsetY;
    constants.scaleX = scale;
    constants.scaleY = scale;

    context.SetConstants(0, &constants, sizeof(constants));
    context.BindTexture(0, input);
    context.BindUnorderedAccessView(0, output);

    int groupX = (mOutputWidth + 7) / 8;
    int groupY = (mOutputHeight + 7) / 8;
    mShader->Dispatch(groupX, groupY, 1);
}

} // namespace renderer
} // namespace fallout
