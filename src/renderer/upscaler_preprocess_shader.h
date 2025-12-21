#ifndef FALLOUT2_UPSCALER_PREPROCESS_SHADER_H
#define FALLOUT2_UPSCALER_PREPROCESS_SHADER_H

namespace fallout {

// GPU Preprocessing Shader (HLSL Compute Shader)
// Handles: Blur filter and HDR (Saturation/Contrast)
// Applied to the original 640x480 image BEFORE ML upscaling
const char* UPSCALER_PREPROCESS_SHADER = R"(
cbuffer PreprocessParams : register(b0) {
    uint2 resolution;      // Input resolution (640x480)
    float blurStrength;    // Blur intensity (0.0-1.0)
    float hdrSaturation;   // Saturation multiplier (0.0-2.0)
    float hdrContrast;     // Contrast multiplier (0.0-2.0)
    float padding[2];
};

Texture2D<float4> InputTexture : register(t0);
RWStructuredBuffer<float> OutputBuffer : register(u0);

// Convert RGB to HSV for saturation adjustment
float3 rgb_to_hsv(float3 rgb) {
    float maxc = max(max(rgb.r, rgb.g), rgb.b);
    float minc = min(min(rgb.r, rgb.g), rgb.b);
    float v = maxc;
    if (minc == maxc) return float3(0, 0, v);
    float s = (maxc - minc) / maxc;
    float delta = maxc - minc;
    float h;
    if (maxc == rgb.r) h = fmod((rgb.g - rgb.b) / delta, 6.0);
    else if (maxc == rgb.g) h = (rgb.b - rgb.r) / delta + 2.0;
    else h = (rgb.r - rgb.g) / delta + 4.0;
    h /= 6.0;
    if (h < 0.0) h += 1.0;
    return float3(h, s, v);
}

// Convert HSV back to RGB
float3 hsv_to_rgb(float3 hsv) {
    if (hsv.y == 0.0) return float3(hsv.z, hsv.z, hsv.z);
    float h = fmod(hsv.x * 6.0, 6.0);
    float c = hsv.z * hsv.y;
    float x = c * (1.0 - abs(fmod(h, 2.0) - 1.0));
    float m = hsv.z - c;
    float3 rgb;
    if (h < 1.0) rgb = float3(c, x, 0);
    else if (h < 2.0) rgb = float3(x, c, 0);
    else if (h < 3.0) rgb = float3(0, c, x);
    else if (h < 4.0) rgb = float3(0, x, c);
    else if (h < 5.0) rgb = float3(x, 0, c);
    else rgb = float3(c, 0, x);
    return rgb + float3(m, m, m);
}

[numthreads(16, 16, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= resolution.x || dtid.y >= resolution.y) return;

    // 1. Blur Filter (3x3 Box Blur)
    float3 color = 0;
    float totalWeight = 0;
    
    [unroll]
    for (int dy = -1; dy <= 1; dy++) {
        [unroll]
        for (int dx = -1; dx <= 1; dx++) {
            int2 pos = clamp(int2(dtid.xy) + int2(dx, dy), int2(0, 0), int2(resolution.x - 1, resolution.y - 1));
            float3 sample = InputTexture[pos].rgb;
            
            // Simple Gaussian-like weight
            float weight = (dx == 0 && dy == 0) ? 4.0 : ((dx == 0 || dy == 0) ? 2.0 : 1.0);
            color += sample * weight;
            totalWeight += weight;
        }
    }
    color /= totalWeight;
    
    // Interpolate between original and blurred based on blurStrength
    float3 original = InputTexture[dtid.xy].rgb;
    color = lerp(original, color, blurStrength);

    // 2. HDR Filter (Saturation)
    float3 hsv = rgb_to_hsv(color);
    hsv.y *= hdrSaturation;
    color = hsv_to_rgb(hsv);

    // 3. HDR Filter (Contrast)
    color = (color - 0.5) * hdrContrast + 0.5;
    color = saturate(color);

    // 4. Write to Planar RGB Buffer [R, G, B]
    uint pixelCount = resolution.x * resolution.y;
    uint idx = dtid.y * resolution.x + dtid.x;
    
    OutputBuffer[idx] = color.r;
    OutputBuffer[idx + pixelCount] = color.g;
    OutputBuffer[idx + pixelCount * 2] = color.b;
}
)";

} // namespace fallout

#endif
