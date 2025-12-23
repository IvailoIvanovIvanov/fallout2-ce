#ifndef FALLOUT_UPSCALER_H
#define FALLOUT_UPSCALER_H

#include <cstdint>

namespace fallout {

// Upscaler mode selection
enum class UpscalerMode {
    NONE,       // No upscaling - passthrough
    ANIME4K,    // Anime4K shader upscaling (experimental, fast ML-inspired shader)
    REAL_ESRGAN // Real-ESRGAN ML upscaling (via ONNX Runtime + DirectML)
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
 * - INTEGER_2X/3X/4X: Lossless pixel replication (perfect for pixel art)
 * - ANIME4K: High-quality shader-based upscaling
 * 
 * Integer scaling is RECOMMENDED for pixel art:
 *   - Zero artifacts, perfect sharpness
 *   - Preserves original aesthetic
 *   - Ultra-fast (simple pixel replication)
 *   - Best for monitors that match output resolution
 * 
 * Configuration (fallout2.cfg):
 *   upscaler_mode=2  # Integer 2x (640x480 -> 1280x960)
 *   upscaler_mode=3  # Integer 3x (640x480 -> 1920x1440)
 *   upscaler_mode=4  # Integer 4x (640x480 -> 2560x1920)
 *   upscaler_mode=5  # Anime4K (shader-based, experimental)
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
 * RENDERING PIPELINE CONTEXT:
 * This initializes the upscaling pipeline that processes Fallout 2's "phantom display"
 * (640x480 indexed color) into modern display resolutions (e.g., 2560x1440).
 * 
 * PHANTOM DISPLAY CONCEPT:
 * - Fallout 2 renders to gSdlSurface (640x480, 8-bit indexed color)
 * - This is the game's native resolution and color depth
 * - The upscaler converts indexed → RGBA → applies filters → scales up
 * - Result is uploaded to GPU texture and rendered to screen
 * 
 * SUPPORTED MODES:
 * - INTEGER_2X: Perfect 2x pixel replication (640x480 → 1280x960)
 * - INTEGER_3X: Perfect 3x pixel replication (640x480 → 1920x1440) ★ RECOMMENDED
 * - INTEGER_4X: Perfect 4x pixel replication (640x480 → 2560x1920)
 * - ANIME4K: Fast shader upscaler (experimental, ML-inspired)
 * 
 * CONFIGURATION OVERRIDE:
 * Mode can be overridden by fallout2.cfg:
 *   upscaler_mode=3               # INTEGER_3X recommended for 2560x1440
 *   upscaler_kuwahara_enable=1    # Edge-preserving color smoothing
 *   upscaler_kuwahara_radius=2    # Smoothing strength (1-5)
 *   upscaler_mode=5               # Anime4K experimental shader
 * 
 * @param inputWidth Source resolution width (640 for Fallout 2)
 * @param inputHeight Source resolution height (480 for Fallout 2)
 * @param outputWidth Physical display width (e.g., 2560)
 * @param outputHeight Physical display height (e.g., 1440)
 * @param mode Initial upscaler mode (can be overridden by config)
 * @param window SDL Window handle (required for OpenGL context)
 * @return 0 on success, non-zero on failure
 */
int upscalerInit(int inputWidth, int inputHeight, int outputWidth, int outputHeight, UpscalerMode mode, void* window);

/**
 * Reconfigure output resolution (e.g., on window resize)
 * @param outputWidth New output width
 * @param outputHeight New output height
 * @return 0 on success, non-zero on failure
 */
int upscalerReconfigureOutput(int outputWidth, int outputHeight);

/**
 * Shutdown upscaler and release resources
 */
void upscalerShutdown();

/**
 * Set input buffer from indexed color source (Fallout 2 native format)
 * @param indexedBuffer Pointer to 640x480 indexed data
 * @param palette Pointer to 256-color RGBA palette
 * @return 0 on success, non-zero on failure
 */
int upscalerSetIndexedInput(const unsigned char* indexedBuffer, const uint32_t* palette);

/**
 * Set input buffer from RGBA source (alternative input)
 * @param rgbaBuffer Pointer to 640x480 RGBA data
 * @return 0 on success, non-zero on failure
 */
int upscalerSetRgbaInput(const uint32_t* rgbaBuffer);

/**
 * Execute upscaling pipeline for current frame
 * @return 0 on success, non-zero on failure
 */
int upscalerDispatch();

/**
 * Get pointer to upscaled output buffer (RGBA)
 * @return Pointer to output buffer
 */
const uint32_t* upscalerGetOutputBuffer();

/**
 * Get pitch (stride) of output buffer in bytes
 * @return Pitch in bytes
 */
int upscalerGetOutputPitch();

/**
 * Get dimensions of output buffer
 * @param width Output width
 * @param height Output height
 */
void upscalerGetOutputDimensions(int& width, int& height);

/**
 * Get current upscaler state
 * @return Current state
 */
UpscalerState upscalerGetState();

/**
 * Set upscaler quality/performance preference
 * @param quality Quality level
 * @return 0 on success
 */
int upscalerSetQuality(UpscalerQuality quality);

/**
 * Get currently configured upscaler mode
 * @return Current mode
 */
UpscalerMode upscalerGetConfiguredMode();

/**
 * Set sharpness level (0.0 - 1.0)
 * @param sharpness Sharpness value
 * @return 0 on success
 */
int upscalerSetSharpness(float sharpness);

/**
 * Enable/disable motion vectors (Deprecated/No-op)
 * @param enabled Enable flag
 */
void upscalerSetMotionVectorsEnabled(bool enabled);

/**
 * Set filter parameters (Deprecated)
 * @param edgeStrength Edge enhancement strength
 * @param colorStrength Color correction strength
 * @param saturation Saturation adjustment
 * @param contrast Contrast adjustment
 * @return 0 on success
 */
int upscalerSetFilterParams(float edgeStrength, float colorStrength, float saturation, float contrast);

/**
 * Enable/disable verbose logging
 * @param enabled Enable flag
 */
void upscalerSetVerboseLogging(bool enabled);

/**
 * Reload configuration from fallout2.cfg
 */
void upscalerReloadConfig();

/**
 * Check if upscaler is available and ready
 * @return true if ready
 */
bool upscalerIsAvailable();

/**
 * Get last error message
 * @return Error message string
 */
const char* upscalerGetLastError();

} // namespace fallout

#endif // FALLOUT_UPSCALER_H
