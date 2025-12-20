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
 * 
 * NOTE: These filters are designed to complement the upscaling process
 * by addressing specific artifacts common in pixel art upscaling.
 */
enum class FilterType {
    NONE = 0,           // No filtering
    DEBANDING = 1,      // Reduce color banding from 8-bit palette (temporal dithering)
    SMOOTH_EDGES = 2,   // Anti-alias jagged pixel art edges (Sobel + Gaussian)
    MINIMAL = 3         // Debanding + edge smoothing (recommended for pixel art)
};

/**
 * @struct FilterConfig
 * @brief Configuration for post-processing filters
 * 
 * Configuration for post-processing filters applied after upscaling.
 */
struct FilterConfig {
    FilterType type = FilterType::MINIMAL;
    
    // Debanding parameters (reduce color banding from 8-bit palette)
    bool enableDebanding = true;       // Apply temporal dithering for smooth gradients
    float debandingStrength = 0.5f;    // 0.0-1.0, dithering intensity
    int frameIndex = 0;                // Frame counter for temporal dithering animation
    
    // Edge smoothing parameters (reduce jagged pixel art edges)
    bool enableEdgeSmoothing = true;   // Apply selective anti-aliasing
    float smoothingStrength = 0.6f;    // 0.0-1.0, smoothing intensity
    float edgeDetectThreshold = 0.15f; // 0.0-1.0, edge detection sensitivity (optimized for pixel art)
    
    // Soft HDR parameters (expand color range and enhance contrast)
    bool enableSoftHDR = false;        // Apply HDR-like tone mapping
    float hdrStrength = 0.5f;          // 0.0-1.0, HDR effect intensity
    float hdrSaturation = 1.2f;        // 0.0-2.0, mid-tone saturation boost
    float hdrContrast = 1.1f;          // 0.0-2.0, contrast enhancement
    
    // General
    bool enableLogging = false;        // Log filter application (verbose mode only)
};

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
 * @brief Apply soft HDR tone mapping to expand color range
 * 
 * Creates an HDR-like effect by:
 * - S-curve tone mapping to expand dynamic range
 * - Selective saturation boost in mid-tones
 * - Contrast enhancement without clipping
 * - Subtle highlight/shadow lift
 * 
 * @param buffer Input/output ARGB8888 buffer
 * @param width  Buffer width in pixels
 * @param height Buffer height in pixels
 * @param pitch  Buffer pitch in bytes
 * @param strength HDR effect strength (0.0-1.0)
 * @param saturation Mid-tone saturation boost (0.0-2.0, 1.0 = neutral)
 * @param contrast Contrast multiplier (0.0-2.0, 1.0 = neutral)
 * @return true if successful
 */
bool filterApplySoftHDR(
    uint32_t* buffer,
    int width,
    int height,
    int pitch,
    float strength,
    float saturation,
    float contrast
);

/**
 * @brief Apply Kuwahara filter for edge-preserving color smoothing
 * 
 * Pre-scaling smoothing filter that smooths colors while preserving edges.
 * Perfect for making color gradients smoother before integer scaling.
 * 
 * Algorithm: For each pixel, examines 4 quadrants of a window, computes
 * mean color of each quadrant, and selects the mean closest to original
 * pixel. This preserves edges while smoothing flat color regions.
 * 
 * @param input Input ARGB8888 buffer (640x480)
 * @param output Output ARGB8888 buffer (same dimensions)
 * @param width Buffer width in pixels
 * @param height Buffer height in pixels
 * @param radius Window radius (typically 1-3, higher = more smoothing)
 * @return true if successful
 */
bool filterApplyKuwahara(
    const uint32_t* input,
    uint32_t* output,
    int width,
    int height,
    int radius
);

/**
 * @brief Apply post-processing filters for pixel art upscaling
 * 
 * Lightweight filter pipeline for pixel art enhancement:
 * - Debanding: Reduces 8-bit palette quantization artifacts
 * - Edge smoothing: Optional anti-aliasing for softer pixel art look
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
