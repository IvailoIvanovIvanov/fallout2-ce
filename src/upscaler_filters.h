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
    COMBINED = 4,       // Apply edge enhancement + color correction
    DEBANDING = 5,      // Reduce color banding from 8-bit palette
    SMOOTH_EDGES = 6,   // Anti-alias jagged edges
    ADVANCED = 7        // All filters including debanding and smoothing
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
    
    // Debanding parameters (reduce color banding from 8-bit palette)
    bool enableDebanding = true;       // Apply temporal dithering for smooth gradients
    float debandingStrength = 0.5f;    // 0.0-1.0, dithering intensity
    int frameIndex = 0;                // Frame counter for temporal dithering
    
    // Edge smoothing parameters (reduce jagged edges)
    bool enableEdgeSmoothing = true;   // Apply selective anti-aliasing
    float smoothingStrength = 0.6f;    // 0.0-1.0, smoothing intensity
    float edgeDetectThreshold = 0.15f; // 0.0-1.0, edge detection sensitivity
    
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
 * @brief Apply debanding filter to reduce color banding
 * 
 * Uses temporal dithering to break up visible color bands that occur
 * when upscaling 8-bit palette content. Creates smoother gradients.
 * 
 * @param buffer Input/output ARGB8888 buffer
 * @param width  Buffer width in pixels
 * @param height Buffer height in pixels
 * @param pitch  Buffer pitch in bytes
 * @param strength Dithering strength (0.0-1.0)
 * @param frameIndex Current frame number for temporal pattern
 * @return true if successful
 */
bool filterApplyDebanding(
    uint32_t* buffer,
    int width,
    int height,
    int pitch,
    float strength,
    int frameIndex
);

/**
 * @brief Apply selective edge smoothing to reduce jagged edges
 * 
 * Detects high-contrast edges (typical of pixel art) and applies
 * selective anti-aliasing to smooth them without blurring the image.
 * 
 * @param buffer Input/output ARGB8888 buffer
 * @param width  Buffer width in pixels
 * @param height Buffer height in pixels
 * @param pitch  Buffer pitch in bytes
 * @param strength Smoothing strength (0.0-1.0)
 * @param edgeThreshold Edge detection threshold (0.0-1.0)
 * @return true if successful
 */
bool filterApplyEdgeSmoothing(
    uint32_t* buffer,
    int width,
    int height,
    int pitch,
    float strength,
    float edgeThreshold
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
