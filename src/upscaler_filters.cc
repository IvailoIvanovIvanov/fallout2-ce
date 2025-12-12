#include "upscaler_filters.h"

#include <algorithm>
#include <cmath>
#include "diagnostics.h"

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
// Combined Post-Processing
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
    
    // Apply filters in optimal order
    switch (config.type) {
        case FilterType::NONE:
            break;
            
        case FilterType::EDGE_ENHANCE:
            if (config.enableLogging) {
                diagnosticsLog(DiagnosticsLevel::Info, "FILTERS", 
                    "Applying edge enhancement (strength=%.2f)", config.edgeEnhanceStrength);
            }
            success = filterApplyEdgeEnhancement(buffer, width, height, pitch, config.edgeEnhanceStrength);
            break;
            
        case FilterType::COLOR_CORRECT:
            if (config.enableLogging) {
                diagnosticsLog(DiagnosticsLevel::Info, "FILTERS",
                    "Applying color correction (strength=%.2f, saturation=%.2f)",
                    config.colorCorrectionStrength, config.saturationBoost);
            }
            success = filterApplyColorCorrection(buffer, width, height, pitch,
                config.colorCorrectionStrength, config.saturationBoost);
            break;
            
        case FilterType::CONTRAST_BOOST:
            if (config.enableLogging) {
                diagnosticsLog(DiagnosticsLevel::Info, "FILTERS",
                    "Applying contrast boost (contrast=%.2f, brightness=%.2f)",
                    config.contrastBoost, config.brightnessShift);
            }
            success = filterApplyContrastBoost(buffer, width, height, pitch,
                config.contrastBoost, config.brightnessShift);
            break;
            
        case FilterType::COMBINED:
            // Apply in optimal order: contrast -> color correction -> edge enhancement
            if (config.enableLogging) {
                diagnosticsLog(DiagnosticsLevel::Info, "FILTERS", "Applying combined filters");
            }
            
            success = filterApplyContrastBoost(buffer, width, height, pitch,
                config.contrastBoost, config.brightnessShift);
            
            if (success) {
                success = filterApplyColorCorrection(buffer, width, height, pitch,
                    config.colorCorrectionStrength, config.saturationBoost);
            }
            
            if (success) {
                success = filterApplyEdgeEnhancement(buffer, width, height, pitch,
                    config.edgeEnhanceStrength);
            }
            break;
    }
    
    if (config.enableLogging && success) {
        diagnosticsLog(DiagnosticsLevel::Info, "FILTERS", "Post-processing complete");
    }
    
    return success;
}

} // namespace fallout
