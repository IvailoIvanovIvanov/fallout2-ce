#ifndef FALLOUT_RENDERER_RENDER_PIPELINE_H
#define FALLOUT_RENDERER_RENDER_PIPELINE_H

/**
 * @file render_pipeline.h
 * @brief Main rendering pipeline for GPU-accelerated upscaling.
 *
 * The RenderPipeline orchestrates the complete rendering process:
 * 1. Receives indexed or RGBA frame data from the game
 * 2. Uploads to GPU and executes shader passes
 * 3. Presents the upscaled result to the screen
 */

#include "gpu_context.h"
#include "buffer_manager.h"
#include "shader_pass.h"
#include "scaler_pass.h"
#include "phantom_display.h"
#include "real_display.h"
#include "render_types.h"
#include "screenshot_manager.h"

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
 * @brief Rendering quality modes.
 */
enum class RenderMode {
    SIMPLE = 0,   ///< Basic bilinear scaling only
    ANIME4K = 1   ///< Multi-pass Anime4K upscaling pipeline
};

/**
 * @struct PipelineStepConfig
 * @brief Configuration for a single shader pass in the pipeline.
 */
struct PipelineStepConfig {
    bool enabled = false;
    std::string shaderPath;
    int scaleFactor = 1;
};

/**
 * @struct Anime4KConfig
 * @brief Complete configuration for the Anime4K pipeline.
 */
struct Anime4KConfig {
    PipelineStepConfig prePass;
    PipelineStepConfig clean1;
    PipelineStepConfig clean2;
    PipelineStepConfig scale1;
    PipelineStepConfig optimize;
    PipelineStepConfig scale2;
    PipelineStepConfig polish;
    PipelineStepConfig postPass;
};

//-----------------------------------------------------------------------------
// RenderPipeline Class
//-----------------------------------------------------------------------------

/**
 * @class RenderPipeline
 * @brief Orchestrates GPU-accelerated rendering with optional upscaling.
 *
 * Manages the complete rendering workflow including:
 * - GPU context and resource initialization
 * - Double-buffered frame management
 * - Multi-pass shader execution
 * - Screen presentation
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

private:
    //-------------------------------------------------------------------------
    // Initialization Helpers
    //-------------------------------------------------------------------------
    
    bool InitializeContext();
    bool InitializeBuffers();
    void InitializeDisplays();
    Dimensions CalculateRenderDimensions();
    
    //-------------------------------------------------------------------------
    // Pass Management
    //-------------------------------------------------------------------------
    
    void SetupPasses();
    void SetupAnime4KPipeline(const RenderSurface& inputSurface, 
                               const RenderSurface& outputSurface);
    void SetupSimplePipeline(const RenderSurface& inputSurface, 
                              const RenderSurface& outputSurface);
    void CleanupPasses();
    
    //-------------------------------------------------------------------------
    // Frame Execution Helpers
    //-------------------------------------------------------------------------
    
    void HandleScreenshotRequest();
    void ExecuteAnime4KChain(RenderSurface& currentInput, bool capture, int& passIndex);
    void ExecuteScalerPass(const RenderSurface& input, const RenderSurface& output, 
                            bool capture, int passIndex);
    
    //-------------------------------------------------------------------------
    // Utility
    //-------------------------------------------------------------------------
    
    void LogDiagnostic(const char* format, ...);
    void ConvertPaletteToRGBA(SDL_Surface* surface, uint32_t* paletteRGBA);

    //-------------------------------------------------------------------------
    // Core Components
    //-------------------------------------------------------------------------
    
    std::unique_ptr<GpuContext> mContext;
    BufferManager mBuffers;
    ScreenshotManager mScreenshotManager;
    SDL_Window* mWindow = nullptr;
    
    //-------------------------------------------------------------------------
    // Displays
    //-------------------------------------------------------------------------
    
    std::unique_ptr<PhantomDisplay> mPhantomDisplay;
    std::unique_ptr<RealDisplay> mRealDisplay;

    //-------------------------------------------------------------------------
    // Shader Passes
    //-------------------------------------------------------------------------
    
    std::unique_ptr<ShaderPass> mScalerPass;
    std::vector<std::unique_ptr<ShaderPass>> mAnime4KPasses;
    std::vector<RenderSurface> mScalingBuffers;

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

    //-------------------------------------------------------------------------
    // Configuration
    //-------------------------------------------------------------------------
    
    RenderMode mConfiguredMode = RenderMode::ANIME4K;
    Anime4KConfig mAnime4KConfig;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_RENDER_PIPELINE_H
