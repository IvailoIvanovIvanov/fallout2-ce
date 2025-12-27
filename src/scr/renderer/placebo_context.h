#ifndef FALLOUT_RENDERER_PLACEBO_CONTEXT_H
#define FALLOUT_RENDERER_PLACEBO_CONTEXT_H

/**
 * @file placebo_context.h
 * @brief libplacebo-based GPU context implementation with Vulkan backend.
 *
 * This file provides a libplacebo implementation that replaces the custom
 * OpenGL/Vulkan rendering with industry-standard upscaling and HDR support.
 * libplacebo is the same library that powers mpv's rendering pipeline.
 */

#include "gpu_context.h"
#include "render_types.h"
#include "placebo_filter_chain.h"

#include <vector>
#include <string>
#include <memory>

// Only include libplacebo if available
#if FALLOUT_HAVE_LIBPLACEBO

#include <libplacebo/config.h>
#include <libplacebo/log.h>
#include <libplacebo/vulkan.h>
#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>
#include <libplacebo/gpu.h>
#include <libplacebo/filters.h>
#include <libplacebo/shaders/sampling.h>
#include <libplacebo/shaders/colorspace.h>

struct SDL_Window;

namespace fallout {
namespace renderer {

//-----------------------------------------------------------------------------
// Configuration Types
//-----------------------------------------------------------------------------

/**
 * @enum PlaceboUpscaler
 * @brief Available upscaling algorithms in libplacebo.
 */
enum class PlaceboUpscaler {
    BILINEAR,           ///< Fast bilinear interpolation
    BICUBIC,            ///< Bicubic interpolation
    HERMITE,            ///< Hermite spline
    LANCZOS,            ///< Lanczos (separable, 3-tap)
    EWA_LANCZOS,        ///< EWA Lanczos (polar, high quality)
    EWA_LANCZOSSHARP,   ///< Sharper EWA Lanczos variant
    EWA_LANCZOS4SHARPEST, ///< 4-lobe sharpest EWA Lanczos
    SPLINE16,           ///< 2-tap spline
    SPLINE36,           ///< 3-tap spline (good balance)
    SPLINE64,           ///< 4-tap spline
    MITCHELL,           ///< Mitchell-Netravali (balanced, no ringing)
    CATMULL_ROM,        ///< Catmull-Rom spline (sharp)
    ROBIDOUX,           ///< Robidoux filter
    ROBIDOUXSHARP,      ///< Sharper Robidoux variant
    GAUSSIAN,           ///< Gaussian blur-like
    OVERSAMPLE,         ///< Pixel art mode (preserves pixel ratios)
    COUNT
};

/**
 * @enum PlaceboColorMode
 * @brief Color processing and HDR output modes.
 */
enum class PlaceboColorMode {
    COLOR_PASSTHROUGH,  ///< No color processing
    SDR_ENHANCE,        ///< Enhanced SDR with color adjustments
    HDR10,              ///< HDR10 output (PQ transfer, BT.2020 primaries)
    HDR10_TONEMAP,      ///< Tonemap HDR content to display capability
};

/**
 * @struct PlaceboConfig
 * @brief Complete configuration for libplacebo rendering.
 */
struct PlaceboConfig {
    //-------------------------------------------------------------------------
    // Upscaling Configuration
    //-------------------------------------------------------------------------
    PlaceboUpscaler upscaler = PlaceboUpscaler::EWA_LANCZOS;
    PlaceboUpscaler downscaler = PlaceboUpscaler::LANCZOS;
    float antiringing = 0.5f;           ///< Anti-ringing strength (0.0-1.0)
    bool sigmoidize = true;             ///< Apply sigmoid curve before upscaling
    
    //-------------------------------------------------------------------------
    // Debanding Configuration
    //-------------------------------------------------------------------------
    bool debanding = true;
    int debandIterations = 1;           ///< Number of debanding passes
    float debandThreshold = 3.0f;       ///< Debanding strength
    float debandRadius = 16.0f;         ///< Debanding search radius
    float debandGrain = 4.0f;           ///< Grain to add after debanding
    
    //-------------------------------------------------------------------------
    // Color Adjustment
    //-------------------------------------------------------------------------
    PlaceboColorMode colorMode = PlaceboColorMode::SDR_ENHANCE;
    float brightness = 0.0f;            ///< Brightness adjustment (-1.0 to 1.0)
    float contrast = 1.1f;              ///< Contrast multiplier
    float saturation = 1.2f;            ///< Saturation multiplier
    float gamma = 1.0f;                 ///< Gamma adjustment
    float hue = 0.0f;                   ///< Hue rotation (degrees)
    
    //-------------------------------------------------------------------------
    // HDR Configuration
    //-------------------------------------------------------------------------
    float peakNits = 1000.0f;           ///< Display peak brightness
    float paperWhiteNits = 203.0f;      ///< Reference white level
    bool hdrPassthrough = false;        ///< Pass HDR metadata through
    
    //-------------------------------------------------------------------------
    // Dithering
    //-------------------------------------------------------------------------
    bool dithering = true;
    int ditherDepth = 8;                ///< Output bit depth for dithering
    
    //-------------------------------------------------------------------------
    // Performance
    //-------------------------------------------------------------------------
    bool skipAntiAliasing = false;      ///< Skip anti-aliasing on downscale
    bool preserveMixingCache = true;    ///< Preserve frame mixing cache on resize
    
    //-------------------------------------------------------------------------
    // Custom Shaders (Anime4K, etc.)
    //-------------------------------------------------------------------------
    bool enableCustomShaders = false;               ///< Enable custom shader chain
    Anime4KPreset anime4kPreset = Anime4KPreset::NONE;  ///< Anime4K preset to use
    std::vector<std::string> customShaderPaths;     ///< Custom shader file paths
    std::string shaderDirectory = "data/shaders";   ///< Base directory for shaders
};

//-----------------------------------------------------------------------------
// PlaceboContext Class
//-----------------------------------------------------------------------------

/**
 * @class PlaceboContext
 * @brief libplacebo-based GPU context with Vulkan backend.
 *
 * Implements the GpuContext interface using libplacebo for:
 * - High-quality upscaling (EWA Lanczos, Spline, etc.)
 * - Color management and HDR output
 * - Debanding and dithering
 * - Swapchain management with automatic HDR detection
 *
 * Thread-safety: Not thread-safe. All calls must be from the main thread.
 */
class PlaceboContext : public GpuContext {
public:
    /**
     * @brief Constructs a PlaceboContext.
     * @param window SDL window handle.
     * @param screenWidth Target screen width.
     * @param screenHeight Target screen height.
     */
    PlaceboContext(SDL_Window* window, int screenWidth, int screenHeight);
    ~PlaceboContext() override;

    //-------------------------------------------------------------------------
    // GpuContext Interface Implementation
    //-------------------------------------------------------------------------

    bool Init() override;
    void Shutdown() override;

    void* CreateTexture(const TextureDesc& desc, const void* initialData = nullptr) override;
    void DestroyTexture(void* textureHandle) override;

    bool CreateComputeShader(const std::string& source, void** outShader) override;
    void Dispatch(void* shader, int x, int y, int z) override;

    void BindTexture(int slot, void* textureHandle) override;
    void BindUnorderedAccessView(int slot, void* textureHandle, 
                                  TextureFormat format = TextureFormat::RGBA8) override;
    void SetConstants(int slot, const void* data, int size) override;

    void BeginFrame() override;
    void EndFrame() override;
    bool Reconfigure(int width, int height) override;
    void Present(void* textureHandle, int srcWidth, int srcHeight, 
                 int windowWidth, int windowHeight) override;

    void UpdateTexture(void* textureHandle, const void* data, int width, int height) override;
    void ReadbackTexture(void* textureHandle, void* data, int size) override;

    //-------------------------------------------------------------------------
    // PlaceboContext-Specific Interface
    //-------------------------------------------------------------------------

    /**
     * @brief Sets the rendering configuration.
     * @param config New configuration to apply.
     */
    void SetConfig(const PlaceboConfig& config);
    
    /**
     * @brief Gets the current configuration.
     * @return Reference to current configuration.
     */
    PlaceboConfig& GetConfig() { return mConfig; }
    const PlaceboConfig& GetConfig() const { return mConfig; }

    /**
     * @brief Renders source texture to swapchain with upscaling and effects.
     * @param srcTexture Source texture handle (from CreateTexture).
     * @param srcWidth Source texture width.
     * @param srcHeight Source texture height.
     */
    void RenderFrame(void* srcTexture, int srcWidth, int srcHeight);

    /**
     * @brief Uploads raw pixel data and renders to swapchain.
     * @param pixelData RGBA pixel data.
     * @param width Image width.
     * @param height Image height.
     */
    void RenderFromPixels(const uint32_t* pixelData, int width, int height);

    /**
     * @brief Checks if HDR output is currently active.
     * @return true if HDR swapchain is active.
     */
    bool IsHDRActive() const;

    /**
     * @brief Gets the current swapchain colorspace.
     * @return Current colorspace description string.
     */
    std::string GetCurrentColorspace() const;

    /**
     * @brief Gets list of available upscaler names.
     * @return Vector of upscaler name strings.
     */
    static std::vector<std::string> GetAvailableUpscalers();

    /**
     * @brief Gets the GPU/device name.
     * @return Device name string.
     */
    std::string GetDeviceName() const;
    
    //-------------------------------------------------------------------------
    // Filter Chain Access
    //-------------------------------------------------------------------------
    
    /**
     * @brief Gets the filter chain for custom shader management.
     * @return Reference to the filter chain.
     */
    PlaceboFilterChain& GetFilterChain() { return mFilterChain; }
    const PlaceboFilterChain& GetFilterChain() const { return mFilterChain; }
    
    /**
     * @brief Reloads custom shaders based on current configuration.
     * @return true if shaders loaded successfully.
     */
    bool ReloadShaders();
    
    /**
     * @brief Logs all active filters to the renderer log.
     */
    void LogActiveFilters() const;

private:
    //-------------------------------------------------------------------------
    // Initialization Helpers
    //-------------------------------------------------------------------------
    bool CreateLogContext();
    bool CreateVulkanInstance();
    bool CreateVulkanDevice();
    bool CreateSwapchain();
    bool CreateRenderer();
    bool CreateUploadTexture(int width, int height);
    bool InitializeFilterChain();

    //-------------------------------------------------------------------------
    // Rendering Helpers
    //-------------------------------------------------------------------------
    void UpdateRenderParams();
    void RegisterBuiltinFilters();
    const struct pl_filter_config* GetFilterConfig(PlaceboUpscaler upscaler) const;
    std::string GetUpscalerName(PlaceboUpscaler upscaler) const;
    void ConfigureSourceFrame(struct pl_frame* frame, pl_tex texture);
    void ConfigureColorspace();
    void ApplyColorAdjustments();

    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    SDL_Window* mWindow = nullptr;
    int mScreenWidth = 0;
    int mScreenHeight = 0;

    // libplacebo core objects
    pl_log mLog = nullptr;
    pl_vk_inst mVkInst = nullptr;
    pl_vulkan mVulkan = nullptr;
    pl_swapchain mSwapchain = nullptr;
    pl_renderer mRenderer = nullptr;
    VkSurfaceKHR mSurface = VK_NULL_HANDLE;

    // Upload texture for game frames
    pl_tex mUploadTex = nullptr;
    int mUploadTexWidth = 0;
    int mUploadTexHeight = 0;

    // Render parameters (cached)
    struct pl_render_params mRenderParams;
    struct pl_deband_params mDebandParams;
    struct pl_sigmoid_params mSigmoidParams;
    struct pl_color_adjustment mColorAdj;
    struct pl_color_map_params mColorMapParams;
    struct pl_dither_params mDitherParams;

    // Custom shader chain
    PlaceboFilterChain mFilterChain;

    // Configuration
    PlaceboConfig mConfig;
    bool mConfigDirty = true;
    bool mInitialized = false;
};

} // namespace renderer
} // namespace fallout

#else // !FALLOUT_HAVE_LIBPLACEBO

//-----------------------------------------------------------------------------
// Stub Implementation When libplacebo Not Available
//-----------------------------------------------------------------------------

namespace fallout {
namespace renderer {

enum class PlaceboUpscaler { BILINEAR, COUNT };
enum class PlaceboColorMode { PASSTHROUGH };

struct PlaceboConfig {
    PlaceboUpscaler upscaler = PlaceboUpscaler::BILINEAR;
    PlaceboColorMode colorMode = PlaceboColorMode::PASSTHROUGH;
};

class PlaceboContext : public GpuContext {
public:
    PlaceboContext(void*, int, int) {}
    ~PlaceboContext() override = default;
    
    bool Init() override { return false; }
    void Shutdown() override {}
    
    void* CreateTexture(const TextureDesc&, const void* = nullptr) override { return nullptr; }
    void DestroyTexture(void*) override {}
    
    bool CreateComputeShader(const std::string&, void**) override { return false; }
    void Dispatch(void*, int, int, int) override {}
    
    void BindTexture(int, void*) override {}
    void BindUnorderedAccessView(int, void*, TextureFormat = TextureFormat::RGBA8) override {}
    void SetConstants(int, const void*, int) override {}
    
    void BeginFrame() override {}
    void EndFrame() override {}
    bool Reconfigure(int, int) override { return false; }
    void Present(void*, int, int, int, int) override {}
    
    void UpdateTexture(void*, const void*, int, int) override {}
    void ReadbackTexture(void*, void*, int) override {}
    
    void SetConfig(const PlaceboConfig&) {}
    PlaceboConfig& GetConfig() { static PlaceboConfig c; return c; }
    void RenderFrame(void*, int, int) {}
    void RenderFromPixels(const uint32_t*, int, int) {}
    bool IsHDRActive() const { return false; }
    std::string GetCurrentColorspace() const { return "sRGB"; }
    static std::vector<std::string> GetAvailableUpscalers() { return {}; }
    std::string GetDeviceName() const { return "N/A"; }
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_HAVE_LIBPLACEBO

#endif // FALLOUT_RENDERER_PLACEBO_CONTEXT_H
