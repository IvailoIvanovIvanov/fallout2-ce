#include "upscaler_filters.h"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include "../diagnostics.h"

namespace fallout {

// ============================================================================
// Color Space Utilities
// ============================================================================

/**
 * Extract ARGB components from packed 32-bit color
 */
inline void unpackARGB(uint32_t color, uint8_t& a, uint8_t& r, uint8_t& g, uint8_t& b) {
    a = (color >> 24) & 0xFF;
    r = (color >> 16) & 0xFF;
    g = (color >> 8) & 0xFF;
    b = color & 0xFF;
}

/**
 * Pack ARGB components into 32-bit color
 */
inline uint32_t packARGB(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

/**
 * Convert RGB to HSV (for saturation adjustment)
 */
inline void rgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    float maxc = std::max({r, g, b});
    float minc = std::min({r, g, b});
    v = maxc;
    
    if (minc == maxc) {
        h = s = 0.0f;
        return;
    }
    
    s = (maxc - minc) / maxc;
    float delta = maxc - minc;
    
    if (maxc == r) h = fmod((g - b) / delta, 6.0f);
    else if (maxc == g) h = (b - r) / delta + 2.0f;
    else h = (r - g) / delta + 4.0f;
    
    h /= 6.0f;
    if (h < 0.0f) h += 1.0f;
}

/**
 * Convert HSV back to RGB
 */
inline void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    if (s == 0.0f) {
        r = g = b = v;
        return;
    }
    
    h = fmod(h * 6.0f, 6.0f);
    float c = v * s;
    float x = c * (1.0f - fabsf(fmod(h, 2.0f) - 1.0f));
    float m = v - c;
    
    if (h < 1.0f) { r = c; g = x; b = 0.0f; }
    else if (h < 2.0f) { r = x; g = c; b = 0.0f; }
    else if (h < 3.0f) { r = 0.0f; g = c; b = x; }
    else if (h < 4.0f) { r = 0.0f; g = x; b = c; }
    else if (h < 5.0f) { r = x; g = 0.0f; b = c; }
    else { r = c; g = 0.0f; b = x; }
    
    r += m;
    g += m;
    b += m;
}

// ============================================================================
// Edge Enhancement (Unsharp Masking)
// ============================================================================

bool filterApplyEdgeEnhancement(
    uint32_t* buffer,
    int width,
    int height,
    int pitch,
    float strength)
{
    if (buffer == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    
    if (strength <= 0.0f) {
        return true; // No-op
    }
    
    strength = std::min(1.0f, std::max(0.0f, strength));
    
    // Create a temporary buffer for edge detection
    uint32_t* tempBuffer = new uint32_t[width * height];
    if (tempBuffer == nullptr) {
        return false;
    }
    
    // Copy original
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            tempBuffer[y * width + x] = buffer[y * width + x];
        }
    }
    
    // Apply Unsharp Masking
    // For each pixel, calculate difference from blurred version
    int bytePitch = pitch;
    int pixelPitch = pitch / 4;
    
    for (int y = 1; y < height - 1; ++y) {
        for (int x = 1; x < width - 1; ++x) {
            uint8_t a, r, g, b;
            unpackARGB(buffer[y * width + x], a, r, g, b);
            
            // Calculate average of neighbors (simplified blur)
            uint32_t avgR = 0, avgG = 0, avgB = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    
                    uint8_t nr, ng, nb, na;
                    unpackARGB(buffer[(y + dy) * width + (x + dx)], na, nr, ng, nb);
                    avgR += nr;
                    avgG += ng;
                    avgB += nb;
                }
            }
            avgR /= 8;
            avgG /= 8;
            avgB /= 8;
            
            // Calculate edge (difference from blur)
            int edgeR = static_cast<int>(r) - static_cast<int>(avgR);
            int edgeG = static_cast<int>(g) - static_cast<int>(avgG);
            int edgeB = static_cast<int>(b) - static_cast<int>(avgB);
            
            // Apply edge enhancement
            int newR = static_cast<int>(r) + static_cast<int>(edgeR * strength);
            int newG = static_cast<int>(g) + static_cast<int>(edgeG * strength);
            int newB = static_cast<int>(b) + static_cast<int>(edgeB * strength);
            
            // Clamp to valid range
            newR = std::min(255, std::max(0, newR));
            newG = std::min(255, std::max(0, newG));
            newB = std::min(255, std::max(0, newB));
            
            tempBuffer[y * width + x] = packARGB(a, (uint8_t)newR, (uint8_t)newG, (uint8_t)newB);
        }
    }
    
    // Copy result back
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            buffer[y * width + x] = tempBuffer[y * width + x];
        }
    }
    
    delete[] tempBuffer;
    return true;
}

// ============================================================================
// Color Correction
// ============================================================================

bool filterApplyColorCorrection(
    uint32_t* buffer,
    int width,
    int height,
    int pitch,
    float strength,
    float saturation)
{
    if (buffer == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    
    strength = std::min(1.0f, std::max(0.0f, strength));
    saturation = std::min(2.0f, std::max(0.5f, saturation));
    
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t a, r, g, b;
            unpackARGB(buffer[y * width + x], a, r, g, b);
            
            // Normalize to 0-1 range
            float fr = r / 255.0f;
            float fg = g / 255.0f;
            float fb = b / 255.0f;
            
            // Convert to HSV for saturation adjustment
            float h, s, v;
            rgbToHsv(fr, fg, fb, h, s, v);
            
            // Apply saturation boost
            s *= saturation;
            s = std::min(1.0f, s);
            
            // Convert back to RGB
            hsvToRgb(h, s, v, fr, fg, fb);
            
            // Apply color correction (slightly boost mid-tones)
            fr = fr * (1.0f + strength * 0.2f);
            fg = fg * (1.0f + strength * 0.2f);
            fb = fb * (1.0f + strength * 0.2f);
            
            // Clamp to valid range
            fr = std::min(1.0f, fr);
            fg = std::min(1.0f, fg);
            fb = std::min(1.0f, fb);
            
            uint8_t newR = (uint8_t)(fr * 255.0f);
            uint8_t newG = (uint8_t)(fg * 255.0f);
            uint8_t newB = (uint8_t)(fb * 255.0f);
            
            buffer[y * width + x] = packARGB(a, newR, newG, newB);
        }
    }
    
    return true;
}

// ============================================================================
// Contrast Boost
// ============================================================================

bool filterApplyContrastBoost(
    uint32_t* buffer,
    int width,
    int height,
    int pitch,
    float contrastBoost,
    float brightnessShift)
{
    if (buffer == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    
    contrastBoost = std::min(2.0f, std::max(0.5f, contrastBoost));
    brightnessShift = std::min(0.2f, std::max(-0.2f, brightnessShift));
    
    // Mid-tone level (0.5)
    float midpoint = 0.5f;
    
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t a, r, g, b;
            unpackARGB(buffer[y * width + x], a, r, g, b);
            
            // Normalize to 0-1
            float fr = r / 255.0f;
            float fg = g / 255.0f;
            float fb = b / 255.0f;
            
            // Apply contrast (relative to midpoint)
            fr = midpoint + (fr - midpoint) * contrastBoost;
            fg = midpoint + (fg - midpoint) * contrastBoost;
            fb = midpoint + (fb - midpoint) * contrastBoost;
            
            // Apply brightness shift
            fr += brightnessShift;
            fg += brightnessShift;
            fb += brightnessShift;
            
            // Clamp
            fr = std::min(1.0f, std::max(0.0f, fr));
            fg = std::min(1.0f, std::max(0.0f, fg));
            fb = std::min(1.0f, std::max(0.0f, fb));
            
            uint8_t newR = (uint8_t)(fr * 255.0f);
            uint8_t newG = (uint8_t)(fg * 255.0f);
            uint8_t newB = (uint8_t)(fb * 255.0f);
            
            buffer[y * width + x] = packARGB(a, newR, newG, newB);
        }
    }
    
    return true;
}

// ============================================================================
// Debanding (Temporal Dithering)
// ============================================================================

bool filterApplyDebanding(
    uint32_t* buffer,
    int width,
    int height,
    int pitch,
    float strength,
    int frameIndex)
{
    if (buffer == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    
    strength = std::min(1.0f, std::max(0.0f, strength));
    
    // Blue noise pattern for dithering (8x8 Bayer matrix scaled)
    static const float bayerMatrix[8][8] = {
        { 0.0f/64, 32.0f/64, 8.0f/64, 40.0f/64, 2.0f/64, 34.0f/64, 10.0f/64, 42.0f/64 },
        { 48.0f/64, 16.0f/64, 56.0f/64, 24.0f/64, 50.0f/64, 18.0f/64, 58.0f/64, 26.0f/64 },
        { 12.0f/64, 44.0f/64, 4.0f/64, 36.0f/64, 14.0f/64, 46.0f/64, 6.0f/64, 38.0f/64 },
        { 60.0f/64, 28.0f/64, 52.0f/64, 20.0f/64, 62.0f/64, 30.0f/64, 54.0f/64, 22.0f/64 },
        { 3.0f/64, 35.0f/64, 11.0f/64, 43.0f/64, 1.0f/64, 33.0f/64, 9.0f/64, 41.0f/64 },
        { 51.0f/64, 19.0f/64, 59.0f/64, 27.0f/64, 49.0f/64, 17.0f/64, 57.0f/64, 25.0f/64 },
        { 15.0f/64, 47.0f/64, 7.0f/64, 39.0f/64, 13.0f/64, 45.0f/64, 5.0f/64, 37.0f/64 },
        { 63.0f/64, 31.0f/64, 55.0f/64, 23.0f/64, 61.0f/64, 29.0f/64, 53.0f/64, 21.0f/64 }
    };
    
    // Temporal offset based on frame index (creates moving dither pattern)
    int temporalOffset = (frameIndex % 4) * 2;
    
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t a, r, g, b;
            unpackARGB(buffer[y * width + x], a, r, g, b);
            
            // Get dither value from Bayer matrix with temporal offset
            int bx = (x + temporalOffset) % 8;
            int by = (y + (frameIndex % 2)) % 8;
            float ditherValue = bayerMatrix[by][bx];
            
            // Skip dithering for very dark pixels to preserve OLED blacks
            if (r < 8 && g < 8 && b < 8) {
                continue;  // Keep near-black pixels untouched
            }
            
            // Apply dithering to reduce banding
            // Scale dither based on strength and apply to each channel
            float ditherAmount = (ditherValue - 0.5f) * strength * 8.0f;
            
            float fr = r + ditherAmount;
            float fg = g + ditherAmount;
            float fb = b + ditherAmount;
            
            // Clamp to valid range
            fr = std::min(255.0f, std::max(0.0f, fr));
            fg = std::min(255.0f, std::max(0.0f, fg));
            fb = std::min(255.0f, std::max(0.0f, fb));
            
            buffer[y * width + x] = packARGB(a, (uint8_t)fr, (uint8_t)fg, (uint8_t)fb);
        }
    }
    
    return true;
}

// ============================================================================
// Edge Smoothing (Selective Anti-Aliasing)
// ============================================================================

bool filterApplyEdgeSmoothing(
    uint32_t* buffer,
    int width,
    int height,
    int pitch,
    float strength,
    float edgeThreshold)
{
    if (buffer == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    
    strength = std::min(1.0f, std::max(0.0f, strength));
    edgeThreshold = std::min(1.0f, std::max(0.0f, edgeThreshold));
    
    // Create temporary buffer for edge detection
    uint32_t* tempBuffer = new uint32_t[width * height];
    if (tempBuffer == nullptr) {
        return false;
    }
    
    // Copy original to temp buffer
    for (int i = 0; i < width * height; ++i) {
        tempBuffer[i] = buffer[i];
    }
    
    // Sobel edge detection + selective smoothing
    for (int y = 1; y < height - 1; ++y) {
        for (int x = 1; x < width - 1; ++x) {
            // Sample 3x3 neighborhood
            uint8_t a[9], r[9], g[9], b[9];
            
            int idx = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    unpackARGB(tempBuffer[(y + dy) * width + (x + dx)], 
                              a[idx], r[idx], g[idx], b[idx]);
                    idx++;
                }
            }
            
            // Sobel operator for edge detection (on luminance)
            float lum[9];
            for (int i = 0; i < 9; ++i) {
                lum[i] = (0.299f * r[i] + 0.587f * g[i] + 0.114f * b[i]) / 255.0f;
            }
            
            // Sobel X: [-1 0 1; -2 0 2; -1 0 1]
            float gx = -lum[0] + lum[2] - 2*lum[3] + 2*lum[5] - lum[6] + lum[8];
            // Sobel Y: [-1 -2 -1; 0 0 0; 1 2 1]
            float gy = -lum[0] - 2*lum[1] - lum[2] + lum[6] + 2*lum[7] + lum[8];
            
            float edgeMagnitude = std::sqrt(gx*gx + gy*gy);
            
            // If edge is strong enough, apply smoothing
            if (edgeMagnitude > edgeThreshold) {
                // Gaussian-weighted average (approximate)
                float totalWeight = 0.0f;
                float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;
                
                // Gaussian weights (3x3)
                static const float gaussianWeights[9] = {
                    0.077f, 0.123f, 0.077f,
                    0.123f, 0.195f, 0.123f,
                    0.077f, 0.123f, 0.077f
                };
                
                for (int i = 0; i < 9; ++i) {
                    float weight = gaussianWeights[i];
                    sumR += r[i] * weight;
                    sumG += g[i] * weight;
                    sumB += b[i] * weight;
                    totalWeight += weight;
                }
                
                // Blend between original and smoothed based on strength
                float smoothedR = sumR / totalWeight;
                float smoothedG = sumG / totalWeight;
                float smoothedB = sumB / totalWeight;
                
                float finalR = r[4] * (1.0f - strength) + smoothedR * strength;
                float finalG = g[4] * (1.0f - strength) + smoothedG * strength;
                float finalB = b[4] * (1.0f - strength) + smoothedB * strength;
                
                buffer[y * width + x] = packARGB(a[4], 
                    (uint8_t)finalR, (uint8_t)finalG, (uint8_t)finalB);
            }
        }
    }
    
    delete[] tempBuffer;
    return true;
}

// ============================================================================
// Combined Post-Processing
// ============================================================================

// ============================================================================
// Kuwahara Filter (Edge-Preserving Color Smoothing)
// ============================================================================

/**
 * Helper: Extract RGB values from packed color
 */
inline void getRGB(uint32_t color, float& r, float& g, float& b) {
    r = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
    g = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
    b = static_cast<float>(color & 0xFF) / 255.0f;
}

/**
 * Helper: Pack RGB back to color (preserves alpha)
 */
inline uint32_t packRGB(uint32_t original, float r, float g, float b) {
    uint8_t alpha = (original >> 24) & 0xFF;
    uint8_t rb = static_cast<uint8_t>(std::clamp(r * 255.0f, 0.0f, 255.0f));
    uint8_t gb = static_cast<uint8_t>(std::clamp(g * 255.0f, 0.0f, 255.0f));
    uint8_t bb = static_cast<uint8_t>(std::clamp(b * 255.0f, 0.0f, 255.0f));
    return (static_cast<uint32_t>(alpha) << 24) | (static_cast<uint32_t>(rb) << 16) |
           (static_cast<uint32_t>(gb) << 8) | static_cast<uint32_t>(bb);
}

/**
 * Kuwahara filter: Edge-preserving smoothing
 * 
 * For each pixel, divides surrounding window into 4 quadrants,
 * computes mean color of each quadrant, then selects the mean
 * closest to the original pixel. This preserves edges while
 * smoothing flat color regions.
 */
bool filterApplyKuwahara(
    const uint32_t* input,
    uint32_t* output,
    int width,
    int height,
    int radius)
{
    if (input == nullptr || output == nullptr || width <= 0 || height <= 0 || radius <= 0) {
        return false;
    }
    
    if (radius > 10) {
        radius = 10;  // Clamp to reasonable range
    }
    
    // Process each pixel
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t originalColor = input[y * width + x];
            float origR, origG, origB;
            getRGB(originalColor, origR, origG, origB);
            
            // Divide window into 4 quadrants and compute mean color of each
            float meanR[4] = {0}, meanG[4] = {0}, meanB[4] = {0};
            int counts[4] = {0};
            
            // Quadrant offsets: TL, TR, BL, BR
            int qx[4] = {-1, 0, -1, 0};
            int qy[4] = {-1, -1, 0, 0};
            
            // Sample each quadrant
            for (int q = 0; q < 4; q++) {
                int startX = x + qx[q] * radius;
                int startY = y + qy[q] * radius;
                int endX = startX + radius;
                int endY = startY + radius;
                
                for (int py = startY; py < endY; py++) {
                    if (py < 0 || py >= height) continue;
                    for (int px = startX; px < endX; px++) {
                        if (px < 0 || px >= width) continue;
                        
                        float r, g, b;
                        getRGB(input[py * width + px], r, g, b);
                        meanR[q] += r;
                        meanG[q] += g;
                        meanB[q] += b;
                        counts[q]++;
                    }
                }
                
                if (counts[q] > 0) {
                    meanR[q] /= counts[q];
                    meanG[q] /= counts[q];
                    meanB[q] /= counts[q];
                }
            }
            
            // Find quadrant with mean closest to original pixel
            float minDist = FLT_MAX;
            int bestQuad = 0;
            
            for (int q = 0; q < 4; q++) {
                float dr = meanR[q] - origR;
                float dg = meanG[q] - origG;
                float db = meanB[q] - origB;
                float dist = dr * dr + dg * dg + db * db;
                
                if (dist < minDist) {
                    minDist = dist;
                    bestQuad = q;
                }
            }
            
            // Output the mean color of the closest quadrant
            output[y * width + x] = packRGB(originalColor, meanR[bestQuad], meanG[bestQuad], meanB[bestQuad]);
        }
    }
    
    return true;
}

// ============================================================================
// Soft HDR Tone Mapping
// ============================================================================

/**
 * S-curve for smooth tone mapping (ACES-inspired)
 */
inline float sCurve(float x, float strength) {
    // Sigmoid-based S-curve
    float a = 2.51f * strength;
    float b = 0.03f;
    float c = 2.43f * strength;
    float d = 0.59f;
    float e = 0.14f;
    
    float numerator = x * (a * x + b);
    float denominator = x * (c * x + d) + e;
    return std::max(0.0f, std::min(1.0f, numerator / denominator));
}

/**
 * Apply soft HDR tone mapping
 */
bool filterApplySoftHDR(
    uint32_t* buffer,
    int width,
    int height,
    int pitch,
    float strength,
    float saturation,
    float contrast,
    float blackCrushThreshold,
    float blackCrushStrength)
{
    if (buffer == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    
    if (strength <= 0.0f) {
        return true; // No-op
    }
    
    strength = std::clamp(strength, 0.0f, 1.0f);
    saturation = std::clamp(saturation, 0.0f, 2.0f);
    contrast = std::clamp(contrast, 0.0f, 2.0f);
    blackCrushThreshold = std::clamp(blackCrushThreshold, 0.0f, 0.15f);
    blackCrushStrength = std::clamp(blackCrushStrength, 0.0f, 2.0f);
    
    const int rowStride = pitch / 4;
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int idx = y * rowStride + x;
            uint32_t pixel = buffer[idx];
            
            uint8_t a, r, g, b;
            unpackARGB(pixel, a, r, g, b);
            
            // Convert to float [0, 1]
            float rf = r / 255.0f;
            float gf = g / 255.0f;
            float bf = b / 255.0f;
            
            // 1. Saturation Boost (Vividness)
            // Applied first to work on the original color balance
            if (saturation != 1.0f) {
                float h, s, v;
                rgbToHsv(rf, gf, bf, h, s, v);
                
                // Global saturation boost with slight mid-tone emphasis
                // This makes colors pop more
                s *= saturation;
                s = std::clamp(s, 0.0f, 1.0f);
                
                hsvToRgb(h, s, v, rf, gf, bf);
            }

            // 2. Calculate luminance for black level processing
            float luminance = 0.299f * rf + 0.587f * gf + 0.114f * bf;
            
            // 3. Aggressive Black Crush for OLED Deep Blacks
            // Convert near-blacks to true black for better contrast on OLED/HDR displays
            if (luminance < blackCrushThreshold) {
                r = g = b = 0;
                buffer[idx] = packARGB(a, 0, 0, 0);
                continue;
            }
            
            // 4. Black Point Adjustment - darken colors just above black threshold
            // This creates a smoother transition and ensures lifted blacks get pushed down
            if (luminance < blackCrushThreshold * 3.0f && blackCrushStrength > 0.0f) {
                // Calculate how close we are to the black threshold (0.0 = at threshold, 1.0 = at 3x threshold)
                float blackProximity = (luminance - blackCrushThreshold) / (blackCrushThreshold * 2.0f);
                
                // Apply power curve to aggressively darken near-blacks
                // Higher blackCrushStrength = more aggressive darkening
                float darkeningFactor = 1.0f - powf(1.0f - blackProximity, 2.0f / blackCrushStrength);
                
                rf *= darkeningFactor;
                gf *= darkeningFactor;
                bf *= darkeningFactor;
                
                // Recalculate luminance after darkening
                luminance = 0.299f * rf + 0.587f * gf + 0.114f * bf;
            }

            // 5. Contrast Enhancement (pivot-based)
            // Use a modified pivot point that preserves blacks better
            // Instead of pivoting at 0.5, pivot at a slightly higher point
            float contrastPivot = 0.5f;
            
            // Only apply contrast to colors above the near-black range
            if (luminance > blackCrushThreshold * 3.0f) {
                // Standard contrast adjustment: (color - pivot) * contrast + pivot
                rf = (rf - contrastPivot) * contrast + contrastPivot;
                gf = (gf - contrastPivot) * contrast + contrastPivot;
                bf = (bf - contrastPivot) * contrast + contrastPivot;
            } else {
                // For near-blacks, apply reduced contrast to avoid lifting them
                float reducedContrast = 1.0f + (contrast - 1.0f) * 0.5f;
                rf = (rf - contrastPivot) * reducedContrast + contrastPivot;
                gf = (gf - contrastPivot) * reducedContrast + contrastPivot;
                bf = (bf - contrastPivot) * reducedContrast + contrastPivot;
            }
            
            // 3. Highlight Compression (Soft Clip)
            // Instead of hard clamping, soft clip the highs to preserve detail in bright lights
            // But ensure we can reach 1.0 (pure white)
            // Simple Reinhard-ish operator mixed with original to keep brightness
            
            // Clamp and convert back to uint8
            r = static_cast<uint8_t>(std::clamp(rf * 255.0f, 0.0f, 255.0f));
            g = static_cast<uint8_t>(std::clamp(gf * 255.0f, 0.0f, 255.0f));
            b = static_cast<uint8_t>(std::clamp(bf * 255.0f, 0.0f, 255.0f));
            
            buffer[idx] = packARGB(a, r, g, b);
        }
    }
    
    return true;
}

// ============================================================================
// Post-Processing Pipeline
// ============================================================================

bool filterApplyPostProcessing(
    uint32_t* buffer,
    int width,
    int height,
    int pitch,
    const FilterConfig& config)
{
    if (buffer == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    
    bool success = true;
    
    // Apply configured filters
    switch (config.type) {
        case FilterType::NONE:
            // No additional filtering
            break;
            
        case FilterType::DEBANDING:
            // Only debanding (8-bit palette artifact reduction)
            if (config.enableLogging) {
                diagnosticsLog(DiagnosticsLevel::Info, "FILTERS",
                    "Applying debanding (strength=%.2f, frame=%d)",
                    config.debandingStrength, config.frameIndex);
            }
            success = filterApplyDebanding(buffer, width, height, pitch,
                config.debandingStrength, config.frameIndex);
            break;
            
        case FilterType::SMOOTH_EDGES:
            // Only edge smoothing (pixel art anti-aliasing)
            if (config.enableLogging) {
                diagnosticsLog(DiagnosticsLevel::Info, "FILTERS",
                    "Applying edge smoothing (strength=%.2f, threshold=%.2f)",
                    config.smoothingStrength, config.edgeDetectThreshold);
            }
            success = filterApplyEdgeSmoothing(buffer, width, height, pitch,
                config.smoothingStrength, config.edgeDetectThreshold);
            break;
            
        case FilterType::MINIMAL:
            // Recommended: Debanding + edge smoothing for pixel art
            if (config.enableLogging) {
                diagnosticsLog(DiagnosticsLevel::Info, "FILTERS", 
                    "Applying minimal filter pipeline (debanding + edge smoothing)");
            }
            
            // 1. Debanding first (smooth color gradients from 8-bit palette)
            if (config.enableDebanding) {
                success = filterApplyDebanding(buffer, width, height, pitch,
                    config.debandingStrength, config.frameIndex);
                if (!success && config.enableLogging) {
                    diagnosticsLog(DiagnosticsLevel::Info, "FILTERS", "WARNING: Debanding failed");
                }
            }
            
            // 2. Edge smoothing (anti-alias jagged pixel art edges)
            if (success && config.enableEdgeSmoothing) {
                success = filterApplyEdgeSmoothing(buffer, width, height, pitch,
                    config.smoothingStrength, config.edgeDetectThreshold);
                if (!success && config.enableLogging) {
                    diagnosticsLog(DiagnosticsLevel::Info, "FILTERS", "WARNING: Edge smoothing failed");
                }
            }
            
            // 3. Soft HDR (optional color enhancement)
            if (success && config.enableSoftHDR) {
                success = filterApplySoftHDR(buffer, width, height, pitch,
                    config.hdrStrength, config.hdrSaturation, config.hdrContrast,
                    config.blackCrushThreshold, config.blackCrushStrength);
                if (!success && config.enableLogging) {
                    diagnosticsLog(DiagnosticsLevel::Info, "FILTERS", "WARNING: Soft HDR failed");
                }
            }
            break;
    }
    
    if (config.enableLogging && success) {
        diagnosticsLog(DiagnosticsLevel::Info, "FILTERS", "Post-processing complete");
    }
    
    return success;
}

} // namespace fallout
