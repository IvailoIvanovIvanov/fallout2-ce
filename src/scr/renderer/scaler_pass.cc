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

bool ScalerPass::Init(GpuContext& context, const RenderSurface& input, const RenderSurface& output) {
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

void ScalerPass::Execute(GpuContext& context, const RenderSurface& input, const RenderSurface& output) {
    if (!mShader || !mShader->IsValid()) return;

    // Calculate scale and offset to preserve aspect ratio
    float scaleX = (float)output.width / input.width;
    float scaleY = (float)output.height / input.height;
    float scale = std::min(scaleX, scaleY);

    float offsetX = (output.width - input.width * scale) * 0.5f;
    float offsetY = (output.height - input.height * scale) * 0.5f;

    ScalerConstants constants;
    constants.inputWidth = input.width;
    constants.inputHeight = input.height;
    constants.outputWidth = output.width;
    constants.outputHeight = output.height;
    constants.offsetX = offsetX;
    constants.offsetY = offsetY;
    constants.scaleX = scale;
    constants.scaleY = scale;

    context.SetConstants(0, &constants, sizeof(constants));
    context.BindTexture(0, input.handle);
    context.BindUnorderedAccessView(1, output.handle, output.format);

    int groupX = (output.width + 7) / 8;
    int groupY = (output.height + 7) / 8;
    mShader->Dispatch(groupX, groupY, 1);
}

} // namespace renderer
} // namespace fallout
