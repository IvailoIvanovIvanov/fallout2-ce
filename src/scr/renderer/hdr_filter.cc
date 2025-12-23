#include "hdr_filter.h"
#include "logger.h"

namespace fallout {
namespace renderer {

static const char* HDR_SHADER_SOURCE = R"(
#version 430
layout(local_size_x = 16, local_size_y = 16) in;
layout(rgba8, binding = 0) uniform readonly image2D inputTex;
layout(rgba8, binding = 1) uniform writeonly image2D outputTex;

layout(std140, binding = 3) uniform HDRParams {
    float u_Saturation;
    float u_Contrast;
    float u_BlackCrushThreshold;
    float u_BlackCrushStrength;
};

vec3 adjustSaturation(vec3 color, float saturation) {
    float grey = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(grey), color, saturation);
}

vec3 adjustContrast(vec3 color, float contrast) {
    return (color - 0.5) * contrast + 0.5;
}

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outputTex);
    if (pos.x >= size.x || pos.y >= size.y) return;

    vec4 color = imageLoad(inputTex, pos);
    
    vec3 rgb = color.rgb;
    
    // Black Crush
    float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    if (lum < u_BlackCrushThreshold) {
        rgb *= pow(lum / u_BlackCrushThreshold, u_BlackCrushStrength);
    }

    rgb = adjustContrast(rgb, u_Contrast);
    rgb = adjustSaturation(rgb, u_Saturation);
    
    imageStore(outputTex, pos, vec4(rgb, color.a));
}
)";

struct HDRParamsStruct {
    float saturation;
    float contrast;
    float blackCrushThreshold;
    float blackCrushStrength;
};

HDRFilter::HDRFilter() {}
HDRFilter::~HDRFilter() {}

bool HDRFilter::Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    
    if (!context.CreateComputeShader(HDR_SHADER_SOURCE, &mShader)) {
        Logger::Log(LogLevel::Error, "HDRFilter: Failed to compile shader");
        return false;
    }
    return true;
}

void HDRFilter::Shutdown(GpuContext& context) {}

void HDRFilter::SetParams(float saturation, float contrast, float blackCrushThreshold, float blackCrushStrength) {
    mSaturation = saturation;
    mContrast = contrast;
    mBlackCrushThreshold = blackCrushThreshold;
    mBlackCrushStrength = blackCrushStrength;
}

void HDRFilter::Execute(GpuContext& context, void* input, void* output) {
    if (!mShader) return;

    context.BindUnorderedAccessView(0, input);
    context.BindUnorderedAccessView(1, output);
    
    HDRParamsStruct params;
    params.saturation = mSaturation;
    params.contrast = mContrast;
    params.blackCrushThreshold = mBlackCrushThreshold;
    params.blackCrushStrength = mBlackCrushStrength;

    context.SetConstants(3, &params, sizeof(params));

    int groupsX = (mInputWidth + 15) / 16;
    int groupsY = (mInputHeight + 15) / 16;
    context.Dispatch(mShader, groupsX, groupsY, 1);
}

} // namespace renderer
} // namespace fallout
