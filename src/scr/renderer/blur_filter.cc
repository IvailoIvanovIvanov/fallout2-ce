#include "blur_filter.h"
#include "logger.h"
#include <cstdio>

namespace fallout {
namespace renderer {

static const char* BLUR_SHADER_SOURCE = R"(
#version 430
layout(local_size_x = 16, local_size_y = 16) in;
layout(rgba8, binding = 0) uniform readonly image2D inputTex;
layout(rgba8, binding = 1) uniform writeonly image2D outputTex;

layout(std140, binding = 2) uniform BlurParams {
    float u_Strength;
};

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outputTex);
    if (pos.x >= size.x || pos.y >= size.y) return;

    if (u_Strength <= 0.0) {
        imageStore(outputTex, pos, imageLoad(inputTex, pos));
        return;
    }

    vec4 sum = vec4(0.0);
    float weightSum = 0.0;
    
    // Simple 3x3 kernel
    for(int y = -1; y <= 1; y++) {
        for(int x = -1; x <= 1; x++) {
            ivec2 offset = ivec2(x, y);
            vec4 c = imageLoad(inputTex, clamp(pos + offset, ivec2(0), size - 1));
            float w = 1.0; 
            if (x==0 && y==0) w = 2.0; // Center weight
            sum += c * w;
            weightSum += w;
        }
    }
    
    vec4 blurred = sum / weightSum;
    vec4 original = imageLoad(inputTex, pos);
    
    imageStore(outputTex, pos, mix(original, blurred, u_Strength));
}
)";

BlurFilter::BlurFilter() {}
BlurFilter::~BlurFilter() {}

bool BlurFilter::Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;

    if (!context.CreateComputeShader(BLUR_SHADER_SOURCE, &mShader)) {
        Logger::Log(LogLevel::Error, "BlurFilter: Failed to compile shader");
        return false;
    }
    return true;
}

void BlurFilter::Shutdown(GpuContext& context) {
    // Shader deletion is handled by context or we need to add DeleteShader to context?
}

void BlurFilter::Execute(GpuContext& context, void* input, void* output) {
    if (!mShader) return;

    context.BindUnorderedAccessView(0, input);
    context.BindUnorderedAccessView(1, output);
    
    // Update params
    context.SetConstants(2, &mParams.strength, sizeof(float));

    int groupsX = (mInputWidth + 15) / 16;
    int groupsY = (mInputHeight + 15) / 16;
    context.Dispatch(mShader, groupsX, groupsY, 1);
}

void BlurFilter::SetStrength(float strength) {
    mParams.strength = strength;
}

} // namespace renderer
} // namespace fallout
