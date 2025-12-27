#ifndef FALLOUT_RENDERER_RENDER_PIPELINE_H
#define FALLOUT_RENDERER_RENDER_PIPELINE_H

/**
 * @file render_pipeline.h
 * @brief Main rendering pipeline for GPU-accelerated upscaling.
 *
 * The RenderPipeline orchestrates the complete rendering process:
 * 1. Receives indexed or RGBA frame data from the game
 * 2. Processes through libplacebo for upscaling/effects
 * 3. Presents the result to the screen
 *
 * Two modes are supported:
 * - SIMPLE: Basic SDL rendering (fallback)
 * - LIBPLACEBO: High-quality upscaling via libplacebo + Vulkan
 */

#include "gpu_context.h"
#include "phantom_display.h"
#include "real_display.h"
#include "render_types.h"
#include "screenshot_manager.h"
#include "renderer_config.h"

#if FALLOUT_HAVE_LIBPLACEBO
#include "placebo_context.h"
#include "placebo_filter_chain.h"
#endif

#include <vector>
#include <memory>
#include <string>

struct SDL_Window;
struct SDL_Surface;

namespace fallout {
namespace renderer {

//-----------------------------------------------------------------------------
// Configuration Types
//-----------------------------------------------------------------------------

/**
 * @enum RenderMode
 * @brief Rendering backend modes.
 */
enum class RenderMode {
    SIMPLE = 0,     ///< Basic SDL/software scaling (fallback)
    LIBPLACEBO = 1  ///< libplacebo with Vulkan (high quality)
};

//-----------------------------------------------------------------------------
// RenderPipeline Class
//-----------------------------------------------------------------------------

/**
 * @class RenderPipeline
 * @brief Orchestrates GPU-accelerated rendering with libplacebo.
 *
 * Manages the complete rendering workflow including:
 * - libplacebo/Vulkan context initialization
 * - Frame upload and processing
 * - Screen presentation with upscaling and effects
 *
 * Usage:
 * @code
 *   RenderPipeline pipeline;
 *   pipeline.Init(640, 480, 1920, 1080, window);
 *   
 *   // Each frame:
 *   pipeline.SetIndexedInput(surface);
 *   pipeline.Dispatch();
 * @endcode
 */
class RenderPipeline {
public:
    RenderPipeline();
    ~RenderPipeline();

    //-------------------------------------------------------------------------
    // Lifecycle
    //-------------------------------------------------------------------------

    /**
     * @brief Initializes the rendering pipeline.
     * @param inputWidth Game's native render width.
     * @param inputHeight Game's native render height.
     * @param outputWidth Target window width.
     * @param outputHeight Target window height.
     * @param window SDL window for presentation.
     * @return true if initialization succeeded.
     */
    bool Init(int inputWidth, int inputHeight, int outputWidth, int outputHeight, 
              SDL_Window* window);

    /**
     * @brief Releases all resources and shuts down the pipeline.
     */
    void Shutdown();

    /**
     * @brief Reconfigures the pipeline for a new output resolution.
     * @param outputWidth New window width.
     * @param outputHeight New window height.
     * @return true if reconfiguration succeeded.
     */
    bool Reconfigure(int outputWidth, int outputHeight);

    //-------------------------------------------------------------------------
    // Input Methods
    //-------------------------------------------------------------------------

    /**
     * @brief Sets frame input from an indexed (8-bit palette) surface.
     * @param surface SDL surface with indexed pixel data.
     * @return true if input was accepted.
     */
    bool SetIndexedInput(SDL_Surface* surface);

    /**
     * @brief Sets frame input from an RGBA buffer.
     * @param rgbaBuffer Pointer to 32-bit RGBA pixel data.
     * @return true if input was accepted.
     */
    bool SetRgbaInput(const uint32_t* rgbaBuffer);

    //-------------------------------------------------------------------------
    // Execution
    //-------------------------------------------------------------------------

    /**
     * @brief Executes the complete rendering pipeline for the current frame.
     */
    void Dispatch();

    /**
     * @brief Returns the readback buffer from the previous frame.
     * @return Pointer to RGBA pixel data, or nullptr if not available.
     */
    const void* GetOutput();

    //-------------------------------------------------------------------------
    // Configuration
    //-------------------------------------------------------------------------

    /**
     * @brief Loads configuration from renderer_config.ini.
     */
    void LoadConfiguration();

    /**
     * @brief Sets the rendering mode.
     */
    void SetMode(RenderMode mode) { mConfiguredMode = mode; }

    /**
     * @brief Gets the current rendering mode.
     */
    RenderMode GetMode() const { return mConfiguredMode; }

    /**
     * @brief Returns true if using libplacebo backend.
     */
    bool IsUsingPlacebo() const { return mUsingPlacebo; }

private:
    //-------------------------------------------------------------------------
    // Initialization Helpers
    //-------------------------------------------------------------------------
    
    bool InitializePlaceboContext();
    bool InitializeSimpleContext();
    void InitializeDisplays();
    Dimensions CalculateRenderDimensions();
    
    //-------------------------------------------------------------------------
    // Frame Execution Helpers
    //-------------------------------------------------------------------------
    
    void DispatchPlacebo();
    void DispatchSimple();
    void HandleScreenshotRequest();
    void SaveCpuScreenshot(const std::string& stageName);
    
    //-------------------------------------------------------------------------
    // Utility
    //-------------------------------------------------------------------------
    
    void LogDiagnostic(const char* format, ...);
    void ConvertPaletteToRGBA(SDL_Surface* surface, uint32_t* paletteRGBA);
    
#if FALLOUT_HAVE_LIBPLACEBO
    void LoadPlaceboConfiguration(RendererConfig& config);
    PlaceboUpscaler ParseUpscaler(const std::string& name);
    PlaceboColorMode ParseColorMode(const std::string& name);
    Anime4KPreset ParseAnime4KPreset(const std::string& name);
#endif

    //-------------------------------------------------------------------------
    // Core Components
    //-------------------------------------------------------------------------
    
#if FALLOUT_HAVE_LIBPLACEBO
    std::unique_ptr<PlaceboContext> mPlaceboContext;
#endif
    ScreenshotManager mScreenshotManager;
    SDL_Window* mWindow = nullptr;
    
    //-------------------------------------------------------------------------
    // Displays
    //-------------------------------------------------------------------------
    
    std::unique_ptr<PhantomDisplay> mPhantomDisplay;
    std::unique_ptr<RealDisplay> mRealDisplay;

    //-------------------------------------------------------------------------
    // Dimensions
    //-------------------------------------------------------------------------
    
    Dimensions mInputDimensions;
    Dimensions mWindowDimensions;
    Dimensions mRenderDimensions;

    //-------------------------------------------------------------------------
    // State
    //-------------------------------------------------------------------------
    
    bool mInitialized = false;
    bool mF8Pressed = false;
    bool mVerboseLogging = false;
    bool mUsingPlacebo = false;

    //-------------------------------------------------------------------------
    // Configuration
    //-------------------------------------------------------------------------
    
    RenderMode mConfiguredMode = RenderMode::LIBPLACEBO;
#if FALLOUT_HAVE_LIBPLACEBO
    PlaceboConfig mPlaceboConfig;
#endif
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_RENDER_PIPELINE_H
