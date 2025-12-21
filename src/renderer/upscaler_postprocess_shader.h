#ifndef FALLOUT2_UPSCALER_POSTPROCESS_SHADER_H
#define FALLOUT2_UPSCALER_POSTPROCESS_SHADER_H

namespace fallout {

// Comprehensive GPU Post-Processing Shader (HLSL Compute Shader)
// Handles: Palette normalization, debanding, edge smoothing, HDR, color grading
// Processes all filters in a single GPU pass for maximum performance
const char* UPSCALER_POSTPROCESS_SHADER = R"(
cbuffer PostProcessParams : register(b0) {
    uint2 resolution;              // Output resolution
    float hdrSaturation;           // Saturation boost (0.0-2.0)
    float hdrContrast;             // Contrast multiplier (0.0-2.0)
    float blackCrushThreshold;     // Luminance threshold for black crush
    float blackCrushStrength;      // Black crush intensity
    uint frameIndex;               // For temporal dithering/debanding
    float debandingStrength;       // Debanding intensity (0.0-1.0)
    float edgeSmoothingStrength;   // Edge smoothing intensity (0.0-1.0)
    float paletteNormalization;    // 8-bit palette expansion (0.0-1.0)
    float colorGrading;            // Color grading intensity (0.0-1.0)
    float padding;
};

RWTexture2D<float4> OutputTexture : register(u0);

// ============================================================================
// Color Space Utilities
// ============================================================================

// Linear to sRGB gamma correction (improves 8-bit palette expansion)
float3 linear_to_srgb(float3 linearColor) {
    float3 srgb;
    srgb.r = (linearColor.r <= 0.0031308) ? linearColor.r * 12.92 : 1.055 * pow(linearColor.r, 1.0/2.4) - 0.055;
    srgb.g = (linearColor.g <= 0.0031308) ? linearColor.g * 12.92 : 1.055 * pow(linearColor.g, 1.0/2.4) - 0.055;
    srgb.b = (linearColor.b <= 0.0031308) ? linearColor.b * 12.92 : 1.055 * pow(linearColor.b, 1.0/2.4) - 0.055;
    return srgb;
}

// sRGB to Linear (for proper color math)
float3 srgb_to_linear(float3 srgb) {
    float3 linearColor;
    linearColor.r = (srgb.r <= 0.04045) ? srgb.r / 12.92 : pow((srgb.r + 0.055) / 1.055, 2.4);
    linearColor.g = (srgb.g <= 0.04045) ? srgb.g / 12.92 : pow((srgb.g + 0.055) / 1.055, 2.4);
    linearColor.b = (srgb.b <= 0.04045) ? srgb.b / 12.92 : pow((srgb.b + 0.055) / 1.055, 2.4);
    return linearColor;
}

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

// Convert RGB to LAB (perceptual color space - better for 8-bit palette)
float3 rgb_to_lab(float3 rgb) {
    // Convert to XYZ first (D65 illuminant)
    float3 linearColor = srgb_to_linear(rgb);
    float x = linearColor.r * 0.4124564 + linearColor.g * 0.3575761 + linearColor.b * 0.1804375;
    float y = linearColor.r * 0.2126729 + linearColor.g * 0.7151522 + linearColor.b * 0.0721750;
    float z = linearColor.r * 0.0193339 + linearColor.g * 0.1191920 + linearColor.b * 0.9503041;
    
    // XYZ to LAB
    x = (x > 0.008856) ? pow(x, 1.0/3.0) : (7.787 * x + 16.0/116.0);
    y = (y > 0.008856) ? pow(y, 1.0/3.0) : (7.787 * y + 16.0/116.0);
    z = (z > 0.008856) ? pow(z, 1.0/3.0) : (7.787 * z + 16.0/116.0);
    
    return float3(
        (116.0 * y) - 16.0,  // L
        500.0 * (x - y),      // a
        200.0 * (y - z)       // b
    );
}

// ============================================================================
// 8-bit Palette Normalization & Expansion
// ============================================================================

// Quantize to 8-bit palette levels (256 values)
float3 quantize_8bit(float3 color) {
    return floor(color * 255.0 + 0.5) / 255.0;
}

// Detect if color is from limited 8-bit palette (posterization detection)
bool is_posterized(float3 color) {
    // Check if color matches 8-bit quantization
    float3 quantized = quantize_8bit(color);
    float3 diff = abs(color - quantized);
    return all(diff < 0.002); // Very close to quantized value
}

// Bilateral filter for smooth color gradients (preserves edges)
float3 bilateral_smooth(uint2 pos, float3 center_color, float sigma_spatial, float sigma_color) {
    float3 result = 0;
    float weight_sum = 0;
    
    // 5x5 kernel for smoothing
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            uint2 sample_pos = pos + int2(dx, dy);
            if (sample_pos.x >= resolution.x || sample_pos.y >= resolution.y) continue;
            
            float4 sample_pixel = OutputTexture[sample_pos];
            float3 sample_color = sample_pixel.rgb;
            
            // Spatial weight (Gaussian based on distance)
            float spatial_dist = length(float2(dx, dy));
            float spatial_weight = exp(-(spatial_dist * spatial_dist) / (2.0 * sigma_spatial * sigma_spatial));
            
            // Color weight (preserve edges)
            float3 color_diff = sample_color - center_color;
            float color_dist = length(color_diff);
            float color_weight = exp(-(color_dist * color_dist) / (2.0 * sigma_color * sigma_color));
            
            float weight = spatial_weight * color_weight;
            result += sample_color * weight;
            weight_sum += weight;
        }
    }
    
    return result / max(weight_sum, 0.0001);
}

// ============================================================================
// Debanding (Temporal Dithering)
// ============================================================================

// Blue noise pattern (8x8 Bayer matrix) - reduces banding from 8-bit palette
float get_dither_value(uint2 pos, uint frame) {
    const float bayer[64] = {
        0.0/64, 32.0/64,  8.0/64, 40.0/64,  2.0/64, 34.0/64, 10.0/64, 42.0/64,
       48.0/64, 16.0/64, 56.0/64, 24.0/64, 50.0/64, 18.0/64, 58.0/64, 26.0/64,
       12.0/64, 44.0/64,  4.0/64, 36.0/64, 14.0/64, 46.0/64,  6.0/64, 38.0/64,
       60.0/64, 28.0/64, 52.0/64, 20.0/64, 62.0/64, 30.0/64, 54.0/64, 22.0/64,
        3.0/64, 35.0/64, 11.0/64, 43.0/64,  1.0/64, 33.0/64,  9.0/64, 41.0/64,
       51.0/64, 19.0/64, 59.0/64, 27.0/64, 49.0/64, 17.0/64, 57.0/64, 25.0/64,
       15.0/64, 47.0/64,  7.0/64, 39.0/64, 13.0/64, 45.0/64,  5.0/64, 37.0/64,
       63.0/64, 31.0/64, 55.0/64, 23.0/64, 61.0/64, 29.0/64, 53.0/64, 21.0/64
    };
    
    // Temporal offset for animation
    int temporal_offset = (frame % 4) * 2;
    int bx = (pos.x + temporal_offset) % 8;
    int by = (pos.y + (frame % 2)) % 8;
    
    return bayer[by * 8 + bx];
}

// ============================================================================
// Edge Detection & Smoothing
// ============================================================================

// Sobel edge detection (detects sharp palette transitions)
float detect_edge(uint2 pos) {
    float lum[9];
    int idx = 0;
    
    // Sample 3x3 neighborhood
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            uint2 sample_pos = pos + int2(dx, dy);
            sample_pos = clamp(sample_pos, uint2(0, 0), resolution - 1);
            
            float4 pixel = OutputTexture[sample_pos];
            lum[idx++] = dot(pixel.rgb, float3(0.299, 0.587, 0.114));
        }
    }
    
    // Sobel operator
    float gx = -lum[0] + lum[2] - 2.0*lum[3] + 2.0*lum[5] - lum[6] + lum[8];
    float gy = -lum[0] - 2.0*lum[1] - lum[2] + lum[6] + 2.0*lum[7] + lum[8];
    
    return sqrt(gx*gx + gy*gy);
}

// Gaussian blur for edge smoothing
float3 gaussian_blur(uint2 pos) {
    const float weights[9] = {
        0.077, 0.123, 0.077,
        0.123, 0.195, 0.123,
        0.077, 0.123, 0.077
    };
    
    float3 result = 0;
    int idx = 0;
    
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            uint2 sample_pos = pos + int2(dx, dy);
            sample_pos = clamp(sample_pos, uint2(0, 0), resolution - 1);
            
            float4 pixel = OutputTexture[sample_pos];
            result += pixel.rgb * weights[idx++];
        }
    }
    
    return result;
}

// ============================================================================
// Main Compute Shader
// ============================================================================

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= resolution.x || DTid.y >= resolution.y) return;
    
    uint2 pos = DTid.xy;
    float4 pixel = OutputTexture[pos];
    float3 rgb = pixel.rgb;
    
    // ========================================================================
    // STAGE 1: 8-bit Palette Normalization & Expansion
    // ========================================================================
    
    if (paletteNormalization > 0.0) {
        // Detect posterization from 8-bit palette
        bool is_quantized = is_posterized(rgb);
        
        if (is_quantized) {
            // Apply bilateral filtering to smooth palette transitions
            float3 smoothed = bilateral_smooth(pos, rgb, 2.0, 0.1);
            rgb = lerp(rgb, smoothed, paletteNormalization * 0.6);
        }
        
        // Work in linear space for proper color math
        rgb = srgb_to_linear(rgb);
    }
    
    // ========================================================================
    // STAGE 2: Debanding (Temporal Dithering)
    // ========================================================================
    
    if (debandingStrength > 0.0) {
        float lum = dot(rgb, float3(0.299, 0.587, 0.114));
        
        // Skip dithering for very dark pixels (preserve blacks)
        if (lum > 0.03) {
            float dither = get_dither_value(pos, frameIndex);
            float dither_amount = (dither - 0.5) * debandingStrength * 0.03;
            rgb += dither_amount;
        }
    }
    
    // ========================================================================
    // STAGE 3: Edge Detection & Smoothing
    // ========================================================================
    
    if (edgeSmoothingStrength > 0.0) {
        float edge_magnitude = detect_edge(pos);
        
        // Apply smoothing at detected edges (anti-aliasing)
        if (edge_magnitude > 0.1) {
            float3 blurred = gaussian_blur(pos);
            if (paletteNormalization > 0.0) {
                blurred = srgb_to_linear(blurred);
            }
            rgb = lerp(rgb, blurred, edgeSmoothingStrength * saturate(edge_magnitude * 2.0));
        }
    }
    
    // Convert back to sRGB for rest of processing
    if (paletteNormalization > 0.0) {
        rgb = linear_to_srgb(rgb);
    }
    
    // ========================================================================
    // STAGE 4: Calculate Luminance
    // ========================================================================
    
    float luminance = dot(rgb, float3(0.299, 0.587, 0.114));
    
    // ========================================================================
    // STAGE 5: Aggressive Black Crush for OLED
    // ========================================================================
    if (luminance < blackCrushThreshold) {
        OutputTexture[pos] = float4(0, 0, 0, pixel.a);
        return;
    }
    
    // ========================================================================
    // STAGE 6: Black Point Adjustment
    // ========================================================================
    
    if (luminance < blackCrushThreshold * 3.0 && blackCrushStrength > 0.0) {
        float blackProximity = (luminance - blackCrushThreshold) / (blackCrushThreshold * 2.0);
        float darkeningFactor = 1.0 - pow(1.0 - blackProximity, 2.0 / blackCrushStrength);
        rgb *= darkeningFactor;
        luminance = dot(rgb, float3(0.299, 0.587, 0.114));
    }
    
    // ========================================================================
    // STAGE 7: Saturation Boost (Vividness)
    // ========================================================================
    
    if (hdrSaturation != 1.0) {
        float3 hsv = rgb_to_hsv(rgb);
        
        // Boost saturation more in mid-tones (natural look)
        float sat_multiplier = hdrSaturation;
        if (luminance < 0.3 || luminance > 0.7) {
            // Reduce saturation boost in darks and brights
            sat_multiplier = 1.0 + (hdrSaturation - 1.0) * 0.7;
        }
        
        hsv.y *= sat_multiplier;
        hsv.y = saturate(hsv.y);
        rgb = hsv_to_rgb(hsv);
    }
    
    // ========================================================================
    // STAGE 8: Contrast Enhancement (Dual-Mode)
    // ========================================================================
    
    float contrastPivot = 0.5;
    if (luminance > blackCrushThreshold * 3.0) {
        // Full contrast for bright colors (S-curve)
        rgb = (rgb - contrastPivot) * hdrContrast + contrastPivot;
    } else {
        // Reduced contrast for near-blacks (preserve shadow detail)
        float reducedContrast = 1.0 + (hdrContrast - 1.0) * 0.5;
        rgb = (rgb - contrastPivot) * reducedContrast + contrastPivot;
    }
    
    // ========================================================================
    // STAGE 9: Color Grading (Optional Film-like Look)
    // ========================================================================
    
    if (colorGrading > 0.0) {
        // Subtle warm lift in shadows, cool tint in highlights
        float shadow_lift = saturate(1.0 - luminance * 2.0); // Stronger in shadows
        float highlight_tint = saturate((luminance - 0.5) * 2.0); // Stronger in highlights
        
        // Warm shadows (slight orange tint)
        rgb.r += shadow_lift * 0.02 * colorGrading;
        rgb.g += shadow_lift * 0.01 * colorGrading;
        
        // Cool highlights (slight blue tint)
        rgb.b += highlight_tint * 0.015 * colorGrading;
        
        // Subtle saturation boost in mid-tones for more vibrant look
        if (luminance > 0.3 && luminance < 0.7) {
            float3 hsv = rgb_to_hsv(rgb);
            hsv.y *= (1.0 + 0.1 * colorGrading);
            hsv.y = saturate(hsv.y);
            rgb = hsv_to_rgb(hsv);
        }
    }
    
    // ========================================================================
    // STAGE 10: Final Adjustments & Output
    // ========================================================================
    
    // Clamp to valid range
    rgb = saturate(rgb);
    
    // Subtle highlight roll-off (prevents clipping)
    if (any(rgb > 0.95)) {
        float max_component = max(max(rgb.r, rgb.g), rgb.b);
        if (max_component > 0.95) {
            float rolloff = (max_component - 0.95) / 0.05; // 0 to 1
            rgb = lerp(rgb, float3(1, 1, 1), rolloff * rolloff * 0.3);
        }
    }
    
    // Write final result
    OutputTexture[pos] = float4(rgb, pixel.a);
}
)";

} // namespace fallout

#endif // FALLOUT2_UPSCALER_POSTPROCESS_SHADER_H
