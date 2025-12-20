#ifndef FALLOUT2_UPSCALER_POSTPROCESS_SHADER_H
#define FALLOUT2_UPSCALER_POSTPROCESS_SHADER_H

namespace fallout {

// GPU Post-Processing Shader (HLSL Compute Shader)
// Applies HDR, color enhancement, and black crush on GPU for performance
const char* UPSCALER_POSTPROCESS_SHADER = R"(
cbuffer PostProcessParams : register(b0) {
    uint2 resolution;           // Output resolution
    float hdrSaturation;        // Saturation boost (0.0-2.0)
    float hdrContrast;          // Contrast multiplier (0.0-2.0)
    float blackCrushThreshold;  // Luminance threshold for black crush
    float blackCrushStrength;   // Black crush intensity
    uint frameIndex;            // For temporal dithering
    float padding;
};

RWTexture2D<float4> OutputTexture : register(u0);

// Convert RGB to HSV
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

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= resolution.x || DTid.y >= resolution.y) return;
    
    float4 pixel = OutputTexture[DTid.xy];
    float3 rgb = pixel.rgb;
    
    // Calculate luminance
    float luminance = dot(rgb, float3(0.299, 0.587, 0.114));
    
    // 1. Aggressive Black Crush for OLED
    if (luminance < blackCrushThreshold) {
        OutputTexture[DTid.xy] = float4(0, 0, 0, pixel.a);
        return;
    }
    
    // 2. Black Point Adjustment - darken near-blacks
    if (luminance < blackCrushThreshold * 3.0 && blackCrushStrength > 0.0) {
        float blackProximity = (luminance - blackCrushThreshold) / (blackCrushThreshold * 2.0);
        float darkeningFactor = 1.0 - pow(1.0 - blackProximity, 2.0 / blackCrushStrength);
        rgb *= darkeningFactor;
        luminance = dot(rgb, float3(0.299, 0.587, 0.114));
    }
    
    // 3. Saturation Boost (Vividness)
    if (hdrSaturation != 1.0) {
        float3 hsv = rgb_to_hsv(rgb);
        hsv.y *= hdrSaturation;
        hsv.y = saturate(hsv.y);
        rgb = hsv_to_rgb(hsv);
    }
    
    // 4. Contrast Enhancement (dual-mode)
    float contrastPivot = 0.5;
    if (luminance > blackCrushThreshold * 3.0) {
        // Full contrast for bright colors
        rgb = (rgb - contrastPivot) * hdrContrast + contrastPivot;
    } else {
        // Reduced contrast for near-blacks
        float reducedContrast = 1.0 + (hdrContrast - 1.0) * 0.5;
        rgb = (rgb - contrastPivot) * reducedContrast + contrastPivot;
    }
    
    // 5. Clamp to valid range
    rgb = saturate(rgb);
    
    OutputTexture[DTid.xy] = float4(rgb, pixel.a);
}
)";

} // namespace fallout

#endif // FALLOUT2_UPSCALER_POSTPROCESS_SHADER_H
