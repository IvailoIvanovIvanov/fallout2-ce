#include "lighting_abstraction.h"

#include <algorithm>
#include <cmath>

namespace fallout {

LightingProcessor& LightingProcessor::getInstance()
{
    static LightingProcessor instance;
    return instance;
}

LightingProcessor::LightingProcessor()
{
    buildLookupTables();
}

void LightingProcessor::buildLookupTables()
{
    // Build gamma correction tables (sRGB gamma ~2.2)
    for (int i = 0; i < 256; i++) {
        // Linear to sRGB
        float linear = static_cast<float>(i) / 255.0f;
        float srgb;
        if (linear <= 0.0031308f) {
            srgb = linear * 12.92f;
        } else {
            srgb = 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        }
        mLinearToSrgb[i] = static_cast<uint8_t>(std::clamp(srgb * 255.0f + 0.5f, 0.0f, 255.0f));

        // sRGB to Linear
        float srgbIn = static_cast<float>(i) / 255.0f;
        float linearOut;
        if (srgbIn <= 0.04045f) {
            linearOut = srgbIn / 12.92f;
        } else {
            linearOut = std::pow((srgbIn + 0.055f) / 1.055f, 2.4f);
        }
        mSrgbToLinear[i] = static_cast<uint8_t>(std::clamp(linearOut * 255.0f + 0.5f, 0.0f, 255.0f));
    }

    // Build enhanced lighting curve (smoother S-curve for shadows)
    for (int i = 0; i < 256; i++) {
        float t = static_cast<float>(i) / 255.0f;
        // Use a smoothstep-like curve for more natural lighting
        // This reduces harsh transitions in shadow areas
        float curved;
        if (t < 0.5f) {
            // Shadow region: use gentler curve
            curved = 2.0f * t * t;
        } else {
            // Highlight region: use inverse curve
            float t2 = t - 0.5f;
            curved = 0.5f + 2.0f * t2 * (1.0f - t2);
        }
        // Blend with linear for a subtler effect
        float blended = 0.7f * t + 0.3f * curved;
        mEnhancedCurve[i] = static_cast<uint8_t>(std::clamp(blended * 255.0f + 0.5f, 0.0f, 255.0f));
    }
}

void LightingProcessor::setQuality(LightingQuality quality)
{
    mQuality = quality;
}

LightingQuality LightingProcessor::getQuality() const
{
    return mQuality;
}

uint8_t LightingProcessor::gammaCorrect(uint8_t linear) const
{
    return mLinearToSrgb[linear];
}

uint8_t LightingProcessor::gammaUncorrect(uint8_t srgb) const
{
    return mSrgbToLinear[srgb];
}

uint8_t LightingProcessor::applyLightingToChannel(uint8_t value, int intensityIndex) const
{
    intensityIndex = std::clamp(intensityIndex, 0, 255);

    if (mQuality == LightingQuality::Original) {
        // Original Fallout 2 lighting formula
        if (intensityIndex <= 0) {
            return 0;
        }
        if (intensityIndex >= 255) {
            return 255;
        }
        if (intensityIndex < 128) {
            return static_cast<uint8_t>((value * intensityIndex) / 128);
        }
        int lighten = intensityIndex - 128;
        return static_cast<uint8_t>(value + ((255 - value) * lighten) / 128);
    }

    // Enhanced lighting: work in linear space for correct blending
    float linear = static_cast<float>(mSrgbToLinear[value]) / 255.0f;
    float intensity = static_cast<float>(intensityIndex) / 128.0f; // 0.0 to 2.0

    float result;
    if (mQuality == LightingQuality::Enhanced) {
        // Simple gamma-correct multiply
        result = linear * intensity;
    } else { // LightingQuality::Smooth
        // Apply enhanced curve for smoother shadows
        float curvedIntensity = static_cast<float>(mEnhancedCurve[intensityIndex]) / 128.0f;
        result = linear * curvedIntensity;
    }

    // Clamp and convert back to sRGB
    result = std::clamp(result, 0.0f, 1.0f);
    uint8_t linearResult = static_cast<uint8_t>(result * 255.0f + 0.5f);
    return mLinearToSrgb[linearResult];
}

uint32_t LightingProcessor::applyLighting(uint32_t color, int intensityIndex) const
{
    uint8_t a = static_cast<uint8_t>(color >> 24);
    uint8_t r = static_cast<uint8_t>((color >> 16) & 0xFF);
    uint8_t g = static_cast<uint8_t>((color >> 8) & 0xFF);
    uint8_t b = static_cast<uint8_t>(color & 0xFF);

    r = applyLightingToChannel(r, intensityIndex);
    g = applyLightingToChannel(g, intensityIndex);
    b = applyLightingToChannel(b, intensityIndex);

    return (static_cast<uint32_t>(a) << 24)
        | (static_cast<uint32_t>(r) << 16)
        | (static_cast<uint32_t>(g) << 8)
        | static_cast<uint32_t>(b);
}

int LightingProcessor::interpolateIntensity(int intensity1, int intensity2, float t) const
{
    t = std::clamp(t, 0.0f, 1.0f);
    
    if (mQuality == LightingQuality::Original) {
        // Simple linear interpolation
        return static_cast<int>(intensity1 + (intensity2 - intensity1) * t);
    }

    // Smooth interpolation using smoothstep for enhanced modes
    float smoothT = t * t * (3.0f - 2.0f * t);
    return static_cast<int>(intensity1 + (intensity2 - intensity1) * smoothT);
}

int LightingProcessor::smoothIntensity(int rawIntensity, int neighborIntensities[4]) const
{
    if (mQuality == LightingQuality::Original) {
        return rawIntensity;
    }

    // Average with neighbors for smoother transitions
    // This reduces the visible triangle boundaries in per-pixel lighting
    int sum = rawIntensity * 4; // Weight center more heavily
    int count = 4;
    for (int i = 0; i < 4; i++) {
        if (neighborIntensities[i] >= 0) {
            sum += neighborIntensities[i];
            count++;
        }
    }
    return sum / count;
}

// Convenience functions
uint32_t lightingApplyToArgb(uint32_t color, int intensityIndex)
{
    return LightingProcessor::getInstance().applyLighting(color, intensityIndex);
}

uint8_t lightingApplyToChannel(uint8_t value, int intensityIndex)
{
    return LightingProcessor::getInstance().applyLightingToChannel(value, intensityIndex);
}

void lightingSetQuality(LightingQuality quality)
{
    LightingProcessor::getInstance().setQuality(quality);
}

LightingQuality lightingGetQuality()
{
    return LightingProcessor::getInstance().getQuality();
}

} // namespace fallout
