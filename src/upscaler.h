#ifndef FALLOUT_UPSCALER_H
#define FALLOUT_UPSCALER_H

#include <cstdint>

namespace fallout {

// Upscaler mode selection
enum class UpscalerMode {
    NONE,       // No upscaling - passthrough
    FSR2,       // AMD FidelityFX Super Resolution 2 (temporal upscaling)
    INTEGER_2X, // Integer 2x scaling (640x480 -> 1280x960) - Perfect for pixel art
    INTEGER_3X, // Integer 3x scaling (640x480 -> 1920x1440) - Perfect for pixel art
    INTEGER_4X, // Integer 4x scaling (640x480 -> 2560x1920) - Perfect for pixel art
};

// Upscaler quality/performance tiers
enum class UpscalerQuality {
    QUALITY,      // Highest quality, lower performance
    BALANCED,     // Balanced mode (default)
    PERFORMANCE,  // Performance mode, lower quality
};

// Upscaler state enumeration
enum class UpscalerState {
    STATE_UNINITIALIZED,
    STATE_INITIALIZING,
    STATE_READY,
    STATE_ERROR,
};

// Forward declaration for opaque implementation
class UpscalerImpl;

/**
 * Full-screen upscaling for Fallout 2 CE
 * 
 * Supports multiple upscaling modes:
 * - FSR2: AMD FidelityFX temporal upscaling (GPU-accelerated)
 * - INTEGER_2X/3X/4X: Lossless pixel replication (perfect for pixel art)
 * 
 * Integer scaling is RECOMMENDED for pixel art:
 *   - Zero artifacts, perfect sharpness
 *   - Preserves original aesthetic
 *   - Ultra-fast (simple pixel replication)
 *   - Best for monitors that match output resolution
 * 
 * Configuration (fallout2.cfg):
 *   upscaler_mode=1  # FSR2 (default, any resolution)
 *   upscaler_mode=2  # Integer 2x (640x480 -> 1280x960)
 *   upscaler_mode=3  # Integer 3x (640x480 -> 1920x1440)
 *   upscaler_mode=4  # Integer 4x (640x480 -> 2560x1920)
 * 
 * Usage:
 *   1. Initialize: upscalerInit(inputW, inputH, outputW, outputH, mode)
 *   2. Each frame:
 *      - Set input: upscalerSetIndexedInput() or upscalerSetRgbaInput()
 *      - Dispatch: upscalerDispatch()
 *      - Get result: upscalerGetOutputBuffer()
 *   3. Cleanup: upscalerShutdown()
 */

// Get singleton instance
UpscalerImpl* upscalerGetImpl();

/**
 * Initialize upscaler with input and output dimensions
 * 
 * @param inputWidth Source resolution width (typically 640)
 * @param inputHeight Source resolution height (typically 480)
 * @param outputWidth Physical display width
 * @param outputHeight Physical display height
 * @param mode Upscaler backend (FSR2, NONE)
 * @return 0 on success, non-zero on failure
 */
int upscalerInit(int inputWidth, int inputHeight, int outputWidth, int outputHeight, UpscalerMode mode);

/**
 * Reconfigure output dimensions (called on window resize)
 * 
 * @param outputWidth New physical display width
 * @param outputHeight New physical display height
 * @return 0 on success, non-zero on failure
 */
int upscalerReconfigureOutput(int outputWidth, int outputHeight);

/**
 * Shutdown upscaler and free all resources
 */
void upscalerShutdown();

/**
 * Get current upscaler state
 */
UpscalerState upscalerGetState();

/**
 * Copy indexed 640x480 buffer to upscaler input (with palette conversion)
 * 
 * @param indexedBuffer Pointer to 640x480 indexed pixel buffer
 * @param palette Pointer to 256-entry RGB palette
 * @return 0 on success, non-zero on failure
 */
int upscalerSetIndexedInput(const unsigned char* indexedBuffer, const uint32_t* palette);

/**
 * Copy RGBA 640x480 buffer to upscaler input (pre-converted)
 * 
 * @param rgbaBuffer Pointer to 640x480 ARGB8888 buffer
 * @return 0 on success, non-zero on failure
 */
int upscalerSetRgbaInput(const uint32_t* rgbaBuffer);

/**
 * Dispatch upscaling operation on GPU
 * Processes input buffer and generates output at target resolution
 * 
 * @return 0 on success, non-zero on failure
 */
int upscalerDispatch();

/**
 * Get pointer to upscaled output buffer
 * 
 * @return Pointer to ARGB8888 buffer at output resolution, or nullptr on error
 */
const uint32_t* upscalerGetOutputBuffer();

/**
 * Get pitch (stride in bytes) of output buffer
 */
int upscalerGetOutputPitch();

/**
 * Get actual output dimensions (may differ from requested if constrained)
 * 
 * @param width [out] Actual output width
 * @param height [out] Actual output height
 */
void upscalerGetOutputDimensions(int& width, int& height);

/**
 * Set quality/performance tier
 * 
 * @param quality Upscaler quality mode
 * @return 0 on success, non-zero on failure
 */
int upscalerSetQuality(UpscalerQuality quality);

/**
 * Set output sharpening level (0.0 = none, 1.0 = maximum)
 * 
 * @param sharpness Sharpening intensity [0.0, 1.0]
 * @return 0 on success, non-zero on failure
 */
int upscalerSetSharpness(float sharpness);

/**
 * Enable/disable motion vectors for temporal stability
 * If disabled, uses zero vectors + jitter for temporal coherence
 * 
 * @param enabled true to enable motion vectors
 */
void upscalerSetMotionVectorsEnabled(bool enabled);

/**
 * Set filter parameters for post-processing
 * Values will be saved to config file
 * 
 * @param edgeStrength Edge enhancement [0.0-1.0]
 * @param colorStrength Color correction [0.0-1.0]
 * @param saturation Saturation multiplier [0.5-2.0]
 * @param contrast Contrast multiplier [0.5-2.0]
 * @return 0 on success, non-zero on failure
 */
int upscalerSetFilterParams(float edgeStrength, float colorStrength, float saturation, float contrast);

/**
 * Enable/disable verbose logging for upscaler operations
 * When enabled, logs detailed frame-by-frame information
 * 
 * @param enabled true to enable verbose logging
 */
void upscalerSetVerboseLogging(bool enabled);

/**
 * Reload configuration from fallout2.cfg
 * Useful for runtime configuration changes
 */
void upscalerReloadConfig();

/**
 * Check if upscaler is available (FSR SDK properly initialized)
 * 
 * @return true if upscaler can be used, false if FSR unavailable/disabled
 */
bool upscalerIsAvailable();

/**
 * Get human-readable error message from last operation
 */
const char* upscalerGetLastError();

} // namespace fallout

#endif // FALLOUT_UPSCALER_H
