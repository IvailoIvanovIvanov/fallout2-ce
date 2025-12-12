#ifndef FALLOUT_UPSCALER_H
#define FALLOUT_UPSCALER_H

#include <cstdint>

namespace fallout {

// Upscaler mode selection
enum class UpscalerMode {
    NONE,      // No upscaling - use integer/bilinear scaling only
    FSR2,      // AMD FidelityFX Super Resolution 2 (Redstone/SDK 2.1)
};

// Upscaler quality/performance tiers
enum class UpscalerQuality {
    QUALITY,      // Highest quality, lower performance
    BALANCED,     // Balanced mode (default)
    PERFORMANCE,  // Performance mode, lower quality
};

// Upscaler state enumeration
enum class UpscalerState {
    UNINITIALIZED,
    INITIALIZING,
    READY,
    ERROR,
};

// Forward declaration for opaque implementation
class UpscalerImpl;

/**
 * GPU-based upscaling using AMD FSR Redstone (FSR SDK 2.1)
 * 
 * Upscales a 640x480 source buffer to the physical display resolution using AI-based
 * supersampling. This is a full-screen approach that composites all game elements
 * before upscaling, eliminating seams and lighting issues present in per-asset upscaling.
 * 
 * Usage:
 *   1. Initialize once: upscalerInit(1920, 1080)
 *   2. Each frame:
 *      - Copy/convert source to RGBA: copyScreenBufferToUpscaler()
 *      - Dispatch upscaling: upscalerDispatch()
 *      - Copy result to display: upscalerGetOutputBuffer()
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
