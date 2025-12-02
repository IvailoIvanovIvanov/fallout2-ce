#ifndef FALLOUT_LIGHTING_ABSTRACTION_H_
#define FALLOUT_LIGHTING_ABSTRACTION_H_

#include <cstdint>

namespace fallout {

// Lighting quality modes
enum class LightingQuality {
    Original,    // Original Fallout 2 lighting (indexed color table)
    Enhanced,    // Smoother gamma-correct lighting for HD assets
    Smooth       // Extra smooth with dithering to reduce banding
};

// Lighting abstraction layer
// Provides both original and enhanced lighting calculations
class LightingProcessor {
public:
    static LightingProcessor& getInstance();

    // Set the lighting quality mode
    void setQuality(LightingQuality quality);
    LightingQuality getQuality() const;

    // Apply lighting to a single ARGB pixel
    // intensityIndex: 0-255 where 128 is neutral, <128 is darker, >128 is brighter
    uint32_t applyLighting(uint32_t color, int intensityIndex) const;

    // Apply lighting to a channel (for compatibility)
    uint8_t applyLightingToChannel(uint8_t value, int intensityIndex) const;

    // Enhanced: interpolate intensity between two values for smooth gradients
    // t is 0.0 to 1.0, returns interpolated intensity
    int interpolateIntensity(int intensity1, int intensity2, float t) const;

    // Enhanced: smooth intensity across a tile for per-pixel lighting
    // This reduces the visible "triangle" artifacts in the original lighting
    int smoothIntensity(int rawIntensity, int neighborIntensities[4]) const;

private:
    LightingProcessor();
    ~LightingProcessor() = default;

    LightingProcessor(const LightingProcessor&) = delete;
    LightingProcessor& operator=(const LightingProcessor&) = delete;

    // Precomputed lookup tables for faster lighting
    void buildLookupTables();

    // Apply gamma correction for perceptually uniform lighting
    uint8_t gammaCorrect(uint8_t linear) const;
    uint8_t gammaUncorrect(uint8_t srgb) const;

    LightingQuality mQuality = LightingQuality::Original;
    
    // Lookup tables for gamma correction (2.2 gamma)
    uint8_t mLinearToSrgb[256];
    uint8_t mSrgbToLinear[256];
    
    // Enhanced lighting curve lookup (smoother than linear)
    uint8_t mEnhancedCurve[256];
};

// Convenience functions that use the singleton
uint32_t lightingApplyToArgb(uint32_t color, int intensityIndex);
uint8_t lightingApplyToChannel(uint8_t value, int intensityIndex);

// Configuration
void lightingSetQuality(LightingQuality quality);
LightingQuality lightingGetQuality();

} // namespace fallout

#endif /* FALLOUT_LIGHTING_ABSTRACTION_H_ */
