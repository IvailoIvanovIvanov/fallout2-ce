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
 * - FSR2: DISABLED (requires motion vectors/depth - incompatible with 2D games)
 * 
 * CONFIGURATION OVERRIDE:
 * Mode can be overridden by fallout2.cfg:
 *   upscaler_mode=3               # INTEGER_3X recommended for 2560x1440
 *   upscaler_kuwahara_enable=1    # Edge-preserving color smoothing
 *   upscaler_kuwahara_radius=2    # Smoothing strength (1-5)
 * 
 * @param inputWidth Source resolution width (640 for Fallout 2)
 * @param inputHeight Source resolution height (480 for Fallout 2)
 * @param outputWidth Physical display width (e.g., 2560)
 * @param outputHeight Physical display height (e.g., 1440)
 * @param mode Initial upscaler mode (can be overridden by config)
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
 * Convert indexed color buffer to RGBA and upload to upscaler input
 * 
 * INDEXED COLOR CONVERSION:
 * Fallout 2 uses 8-bit indexed color (256-color palette). This function:
 * 1. Reads each pixel's palette index (0-255) from indexedBuffer
 * 2. Looks up the RGBA color from the palette array
 * 3. Writes the RGBA color to internal input buffer (mInputBuffer)
 * 
 * INPUT FORMAT:
 * - indexedBuffer: 640x480 bytes (1 byte per pixel = palette index)
 * - palette: 256 colors × 4 bytes = 1024 bytes (RGBA format)
 * 
 * OUTPUT:
 * - Internal mInputBuffer: 640x480x4 bytes = 1,228,800 bytes (RGBA)
 * 
 * This converted RGBA buffer is then processed by filters (Kuwahara) and
 * scaling algorithms (INTEGER_2X/3X/4X).
 * 
 * TYPICAL USAGE (in renderPresent):
 *   uint32_t* palette = convertPaletteToRGBA(gSdlSurface->format->palette);
 *   upscalerSetIndexedInput(gSdlSurface->pixels, palette);
 *   upscalerDispatch();
 *   const uint32_t* output = upscalerGetOutputBuffer();
 * 
 * @param indexedBuffer Pointer to 640x480 indexed pixel data (from gSdlSurface->pixels)
 * @param palette Pointer to 256-color RGBA palette (from SDL_Palette)
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
 * Execute upscaling pipeline on current input buffer
 * 
 * PROCESSING FLOW:
 * 1. Input buffer contains 640x480 RGBA (set via upscalerSetIndexedInput)
 * 2. Route to mode-specific algorithm:
 *    - INTEGER_2X/3X/4X: Perfect pixel replication with optional Kuwahara filter
 *    - FSR2: DISABLED (incompatible with 2D games)
 * 3. Output buffer receives upscaled result (e.g., 2560x1440 RGBA)
 * 
 * INTEGER SCALING PIPELINE:
 * 1. Apply Kuwahara filter (optional): Edge-preserving color smoothing
 *    - Reduces color banding from 8-bit palette
 *    - Configurable radius (1-5 pixels)
 * 2. Replicate each pixel N×N times (N = 2, 3, or 4)
 * 3. Center scaled content in output buffer with black letterbox bars
 *    - E.g., INTEGER_3X: 1920x1440 content centered in 2560x1440 output
 *    - Maintains perfect 4:3 aspect ratio
 * 
 * PERFORMANCE:
 * - INTEGER_3X with Kuwahara: ~1-3ms per frame (Release build)
 * - FSR2: N/A (disabled)
 * 
 * After dispatch completes, retrieve output via upscalerGetOutputBuffer()
 * and upload to GPU texture for rendering.
 * 
 * @return 0 on success, non-zero on failure
 */
int upscalerDispatch();

/**
 * Get pointer to upscaled output buffer (read-only)
 * 
 * OUTPUT BUFFER CONTENTS:
 * After upscalerDispatch() completes, this returns a pointer to the
 * processed output buffer containing the upscaled frame data.
 * 
 * BUFFER FORMAT:
 * - Format: RGBA (32-bit, 4 bytes per pixel)
 * - Size: outputWidth × outputHeight × 4 bytes
 * - Example: 2560×1440×4 = 14,745,600 bytes
 * 
 * INTEGER MODE OUTPUT:
 * - Contains centered scaled content with black letterbox bars
 * - E.g., INTEGER_3X on 2560×1440 display:
 *   - 320px black bar (left)
 *   - 1920×1440 scaled content (center)
 *   - 320px black bar (right)
 * 
 * USAGE:
 * The returned buffer should be uploaded to GPU texture (gSdlTexture)
 * and rendered to screen. The buffer remains valid until next dispatch.
 * 
 * @return Pointer to RGBA output buffer, or nullptr if not ready/failed
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
 * Get the configured upscaler mode from config file
 * This may differ from the mode passed to upscalerInit() if config overrides it
 * 
 * @return Configured upscaler mode
 */
UpscalerMode upscalerGetConfiguredMode();

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
