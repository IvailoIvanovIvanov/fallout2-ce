#ifndef FALLOUT2_UPSCALER_FILTERS_H
#define FALLOUT2_UPSCALER_FILTERS_H

#include <cstdint>

namespace fallout {

/**
 * @brief Post-processing filters for upscaled content
 * 
 * These filters improve the quality of AI-upscaled images by:
 * - Correcting color shift from upscaling
 * - Enhancing edges without introducing artifacts
 * - Improving contrast and clarity
 */

/**
 * @enum FilterType
 * @brief Available post-processing filters
 */
enum class FilterType {
    NONE = 0,           // No filtering
    EDGE_ENHANCE = 1,   // Enhance edges to improve sharpness
    COLOR_CORRECT = 2,  // Correct color shift from upscaling
    CONTRAST_BOOST = 3, // Increase contrast subtly
    COMBINED = 4        // Apply edge enhancement + color correction
};

/**
 * @struct FilterConfig
 * @brief Configuration for post-processing filters
 */
struct FilterConfig {
    FilterType type = FilterType::COMBINED;
    
    // Edge enhancement parameters
    float edgeEnhanceStrength = 0.5f;  // 0.0-1.0, strength of edge enhancement
    float edgeThreshold = 0.1f;        // 0.0-1.0, minimum gradient to enhance
    
    // Color correction parameters
    float colorCorrectionStrength = 0.3f;  // 0.0-1.0, how much to correct color shift
    float saturationBoost = 1.1f;          // 0.5-2.0, saturation multiplier
    
    // Contrast parameters
    float contrastBoost = 1.15f;       // 0.5-2.0, contrast multiplier
    float brightnessShift = 0.0f;      // -0.2 to 0.2, brightness adjustment
    
    // General
    bool enableLogging = true;         // Log filter statistics
};

/**
 * @brief Apply edge enhancement filter to improve sharpness
 * 
 * Uses Unsharp Masking technique to enhance edges and details
 * without creating halos or artifacts.
 * 
 * @param buffer Input/output ARGB8888 buffer
 * @param width  Buffer width in pixels
 * @param height Buffer height in pixels
 * @param pitch  Buffer pitch in bytes (width * 4 for ARGB8888)
 * @param strength Edge enhancement strength (0.0-1.0)
 * @return true if successful
 */
bool filterApplyEdgeEnhancement(
    uint32_t* buffer, 
    int width, 
    int height, 
    int pitch,
    float strength
);

/**
 * @brief Apply color correction to remove upscaling color shift
 * 
 * Corrects color space shift that often occurs during AI upscaling,
 * improving color accuracy and vibrancy.
 * 
 * @param buffer Input/output ARGB8888 buffer
 * @param width  Buffer width in pixels
 * @param height Buffer height in pixels
 * @param pitch  Buffer pitch in bytes
 * @param strength Color correction strength (0.0-1.0)
 * @param saturation Saturation boost factor (0.5-2.0)
 * @return true if successful
 */
bool filterApplyColorCorrection(
    uint32_t* buffer,
    int width,
    int height,
    int pitch,
    float strength,
    float saturation
);

/**
 * @brief Apply contrast and brightness adjustment
 * 
 * Improves visual punch and clarity of the upscaled image.
 * 
 * @param buffer Input/output ARGB8888 buffer
 * @param width  Buffer width in pixels
 * @param height Buffer height in pixels
 * @param pitch  Buffer pitch in bytes
 * @param contrastBoost Contrast multiplier (0.5-2.0)
 * @param brightnessShift Brightness adjustment (-0.2 to 0.2)
 * @return true if successful
 */
bool filterApplyContrastBoost(
    uint32_t* buffer,
    int width,
    int height,
    int pitch,
    float contrastBoost,
    float brightnessShift
);

/**
 * @brief Apply combined post-processing filters
 * 
 * Applies all enabled filters in optimal order for best quality.
 * 
 * @param buffer Input/output ARGB8888 buffer
 * @param width  Buffer width in pixels
 * @param height Buffer height in pixels
 * @param pitch  Buffer pitch in bytes
 * @param config Filter configuration
 * @return true if successful
 */
bool filterApplyPostProcessing(
    uint32_t* buffer,
    int width,
    int height,
    int pitch,
    const FilterConfig& config
);

} // namespace fallout

#endif // FALLOUT2_UPSCALER_FILTERS_H
