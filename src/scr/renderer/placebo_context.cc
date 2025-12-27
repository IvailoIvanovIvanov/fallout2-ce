/**
 * @file placebo_context.cc
 * @brief libplacebo GPU context implementation.
 *
 * This file implements the PlaceboContext class which provides high-quality
 * upscaling and HDR output using the libplacebo library with Vulkan backend.
 */

#include "placebo_context.h"

#if FALLOUT_HAVE_LIBPLACEBO

#include "logger.h"
#include "placebo_filter_chain.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <cstring>
#include <algorithm>
#include <sstream>

namespace fallout {
namespace renderer {

//-----------------------------------------------------------------------------
// libplacebo Logging Callback
//-----------------------------------------------------------------------------

static void placebo_log_callback(void* ctx, enum pl_log_level level, const char* msg) {
    const char* prefix = "";
    LogLevel logLevel = LogLevel::Info;
    switch (level) {
        case PL_LOG_FATAL: prefix = "[FATAL]"; logLevel = LogLevel::Error; break;
        case PL_LOG_ERR:   prefix = "[ERROR]"; logLevel = LogLevel::Error; break;
        case PL_LOG_WARN:  prefix = "[WARN]";  logLevel = LogLevel::Warning; break;
        case PL_LOG_INFO:  prefix = "[INFO]";  logLevel = LogLevel::Info; break;
        case PL_LOG_DEBUG: prefix = "[DEBUG]"; logLevel = LogLevel::Info; break;
        case PL_LOG_TRACE: prefix = "[TRACE]"; logLevel = LogLevel::Info; break;
        default: break;
    }
    
    // Use our renderer logging system
    Logger::Log(logLevel, "libplacebo %s: %s", prefix, msg);
}

//-----------------------------------------------------------------------------
// Constructor / Destructor
//-----------------------------------------------------------------------------

PlaceboContext::PlaceboContext(SDL_Window* window, int screenWidth, int screenHeight)
    : mWindow(window)
    , mScreenWidth(screenWidth)
    , mScreenHeight(screenHeight)
{
    // Initialize with default parameters
    std::memset(&mRenderParams, 0, sizeof(mRenderParams));
    std::memset(&mDebandParams, 0, sizeof(mDebandParams));
    std::memset(&mSigmoidParams, 0, sizeof(mSigmoidParams));
    std::memset(&mColorAdj, 0, sizeof(mColorAdj));
    std::memset(&mColorMapParams, 0, sizeof(mColorMapParams));
    std::memset(&mDitherParams, 0, sizeof(mDitherParams));
}

PlaceboContext::~PlaceboContext() {
    Shutdown();
}

//-----------------------------------------------------------------------------
// Initialization
//-----------------------------------------------------------------------------

bool PlaceboContext::Init() {
    Logger::Log(LogLevel::Info, "Initializing PlaceboContext...");
    
    if (!CreateLogContext()) {
        Logger::Log(LogLevel::Error, "Failed to create libplacebo log context");
        return false;
    }
    
    if (!CreateVulkanInstance()) {
        Logger::Log(LogLevel::Error, "Failed to create Vulkan instance");
        return false;
    }
    
    if (!CreateVulkanDevice()) {
        Logger::Log(LogLevel::Error, "Failed to create Vulkan device");
        return false;
    }
    
    if (!CreateSwapchain()) {
        Logger::Log(LogLevel::Error, "Failed to create swapchain");
        return false;
    }
    
    if (!CreateRenderer()) {
        Logger::Log(LogLevel::Error, "Failed to create renderer");
        return false;
    }
    
    if (!InitializeFilterChain()) {
        Logger::Log(LogLevel::Warning, "Filter chain initialization failed, continuing without custom shaders");
    }
    
    // Apply initial configuration
    UpdateRenderParams();
    RegisterBuiltinFilters();
    
    mInitialized = true;
    Logger::Log(LogLevel::Info, "PlaceboContext initialized successfully");
    Logger::Log(LogLevel::Info, "  Device: %s", GetDeviceName().c_str());
    Logger::Log(LogLevel::Info, "  HDR: %s", IsHDRActive() ? "Active" : "Inactive");
    
    // Log active filter pipeline
    LogActiveFilters();
    
    return true;
}

bool PlaceboContext::CreateLogContext() {
    struct pl_log_params log_params = {0};
    log_params.log_cb = placebo_log_callback;
    log_params.log_priv = nullptr;
    log_params.log_level = PL_LOG_INFO;
    
    mLog = pl_log_create(PL_API_VER, &log_params);
    return mLog != nullptr;
}

bool PlaceboContext::CreateVulkanInstance() {
    // Get required Vulkan extensions from SDL
    unsigned int extensionCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(mWindow, &extensionCount, nullptr)) {
        Logger::Log(LogLevel::Error, "Failed to get Vulkan extension count: %s", SDL_GetError());
        return false;
    }
    
    std::vector<const char*> extensions(extensionCount);
    if (!SDL_Vulkan_GetInstanceExtensions(mWindow, &extensionCount, extensions.data())) {
        Logger::Log(LogLevel::Error, "Failed to get Vulkan extensions: %s", SDL_GetError());
        return false;
    }
    
    // Log extensions
    Logger::Log(LogLevel::Info, "Required Vulkan extensions:");
    for (const char* ext : extensions) {
        Logger::Log(LogLevel::Info, "  - %s", ext);
    }
    
    // Create Vulkan instance through libplacebo
    struct pl_vk_inst_params inst_params = {0};
    inst_params.debug = false;
    inst_params.extensions = extensions.data();
    inst_params.num_extensions = static_cast<int>(extensions.size());
    
    mVkInst = pl_vk_inst_create(mLog, &inst_params);
    if (!mVkInst) {
        Logger::Log(LogLevel::Error, "pl_vk_inst_create failed");
        return false;
    }
    
    Logger::Log(LogLevel::Info, "Vulkan instance created (API version: %u.%u.%u)",
                VK_API_VERSION_MAJOR(mVkInst->api_version),
                VK_API_VERSION_MINOR(mVkInst->api_version),
                VK_API_VERSION_PATCH(mVkInst->api_version));
    
    return true;
}

bool PlaceboContext::CreateVulkanDevice() {
    // Create Vulkan surface via SDL
    mSurface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(mWindow, mVkInst->instance, &mSurface)) {
        Logger::Log(LogLevel::Error, "Failed to create Vulkan surface: %s", SDL_GetError());
        return false;
    }
    
    // Create Vulkan device through libplacebo
    struct pl_vulkan_params vk_params = pl_vulkan_default_params;
    vk_params.instance = mVkInst->instance;
    vk_params.get_proc_addr = mVkInst->get_proc_addr;
    vk_params.surface = mSurface;
    vk_params.allow_software = false;
    // Note: async_transfer and async_compute are enabled by default via PL_VULKAN_DEFAULTS
    vk_params.extra_queues = VK_QUEUE_VIDEO_DECODE_BIT_KHR;
    
    mVulkan = pl_vulkan_create(mLog, &vk_params);
    if (!mVulkan) {
        Logger::Log(LogLevel::Error, "pl_vulkan_create failed");
        vkDestroySurfaceKHR(mVkInst->instance, mSurface, nullptr);
        mSurface = VK_NULL_HANDLE;
        return false;
    }
    
    Logger::Log(LogLevel::Info, "Vulkan device created (API version: %u.%u.%u)",
                VK_API_VERSION_MAJOR(mVulkan->api_version),
                VK_API_VERSION_MINOR(mVulkan->api_version),
                VK_API_VERSION_PATCH(mVulkan->api_version));
    
    return true;
}

bool PlaceboContext::CreateSwapchain() {
    // Create swapchain for presenting to window
    struct pl_vulkan_swapchain_params sw_params = {0};
    sw_params.surface = mSurface;
    sw_params.present_mode = VK_PRESENT_MODE_FIFO_KHR;
    sw_params.swapchain_depth = 3;
    sw_params.allow_suboptimal = false;
    
    mSwapchain = pl_vulkan_create_swapchain(mVulkan, &sw_params);
    if (!mSwapchain) {
        Logger::Log(LogLevel::Error, "pl_vulkan_create_swapchain failed");
        return false;
    }
    
    // Query/set swapchain size
    int width = mScreenWidth;
    int height = mScreenHeight;
    if (!pl_swapchain_resize(mSwapchain, &width, &height)) {
        Logger::Log(LogLevel::Warning, "pl_swapchain_resize returned false");
    }
    
    Logger::Log(LogLevel::Info, "Swapchain created: %dx%d", width, height);
    
    // Configure colorspace hint for HDR if enabled
    ConfigureColorspace();
    
    return true;
}

bool PlaceboContext::CreateRenderer() {
    mRenderer = pl_renderer_create(mLog, mVulkan->gpu);
    if (!mRenderer) {
        Logger::Log(LogLevel::Error, "pl_renderer_create failed");
        return false;
    }
    return true;
}

bool PlaceboContext::CreateUploadTexture(int width, int height) {
    // Find suitable format
    pl_fmt fmt = pl_find_fmt(mVulkan->gpu, PL_FMT_UNORM, 4, 8, 8,
                              static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_HOST_READABLE));
    if (!fmt) {
        Logger::Log(LogLevel::Error, "Failed to find RGBA8 format");
        return false;
    }
    
    // Destroy existing texture if size changed
    if (mUploadTex && (mUploadTexWidth != width || mUploadTexHeight != height)) {
        pl_tex_destroy(mVulkan->gpu, &mUploadTex);
        mUploadTex = nullptr;
    }
    
    if (!mUploadTex) {
        struct pl_tex_params tex_params = {0};
        tex_params.w = width;
        tex_params.h = height;
        tex_params.format = fmt;
        tex_params.sampleable = true;
        tex_params.blit_src = true;
        tex_params.host_writable = true;
        
        mUploadTex = pl_tex_create(mVulkan->gpu, &tex_params);
        if (!mUploadTex) {
            Logger::Log(LogLevel::Error, "Failed to create upload texture");
            return false;
        }
        
        mUploadTexWidth = width;
        mUploadTexHeight = height;
    }
    
    return true;
}

bool PlaceboContext::InitializeFilterChain() {
    if (!mLog || !mVulkan || !mVulkan->gpu) {
        Logger::Log(LogLevel::Error, "Cannot initialize filter chain: GPU not ready");
        return false;
    }
    
    if (!mFilterChain.Init(mLog, mVulkan->gpu)) {
        Logger::Log(LogLevel::Error, "Failed to initialize filter chain");
        return false;
    }
    
    // Load shaders based on config
    return ReloadShaders();
}

bool PlaceboContext::ReloadShaders() {
    if (!mFilterChain.Init(mLog, mVulkan->gpu)) {
        return false;
    }
    
    mFilterChain.ClearShaders();
    
    if (!mConfig.enableCustomShaders) {
        Logger::Log(LogLevel::Info, "[Placebo] Custom shaders disabled");
        return true;
    }
    
    // Load Anime4K preset if configured
    if (mConfig.anime4kPreset != Anime4KPreset::NONE) {
        Logger::Log(LogLevel::Info, "[Placebo] Loading Anime4K preset...");
        if (!mFilterChain.LoadAnime4KPreset(mConfig.anime4kPreset, mConfig.shaderDirectory)) {
            Logger::Log(LogLevel::Warning, "[Placebo] Failed to load Anime4K preset");
        }
    }
    
    // Load custom shader paths
    for (const auto& path : mConfig.customShaderPaths) {
        std::string fullPath = path;
        // If path is relative, prepend shader directory
        if (path.find(':') == std::string::npos && path[0] != '/' && path[0] != '\\') {
            fullPath = mConfig.shaderDirectory + "/" + path;
        }
        mFilterChain.LoadShader(fullPath);
    }
    
    // Register built-in filters for logging
    RegisterBuiltinFilters();
    
    // Log the complete filter chain
    LogActiveFilters();
    
    // Mark config dirty so UpdateRenderParams will update hooks
    mConfigDirty = true;
    
    return true;
}

void PlaceboContext::LogActiveFilters() const {
    mFilterChain.LogActiveFilters();
}

void PlaceboContext::Shutdown() {
    Logger::Log(LogLevel::Info, "Shutting down PlaceboContext...");
    
    // Shutdown filter chain first
    mFilterChain.Shutdown();
    
    if (mUploadTex) {
        pl_tex_destroy(mVulkan->gpu, &mUploadTex);
        mUploadTex = nullptr;
    }
    
    if (mRenderer) {
        pl_renderer_destroy(&mRenderer);
        mRenderer = nullptr;
    }
    
    if (mSwapchain) {
        pl_swapchain_destroy(&mSwapchain);
        mSwapchain = nullptr;
    }
    
    if (mVulkan) {
        // Destroy surface before Vulkan device (we need the instance still)
        if (mSurface && mVkInst) {
            vkDestroySurfaceKHR(mVkInst->instance, mSurface, nullptr);
            mSurface = VK_NULL_HANDLE;
        }
        pl_vulkan_destroy(&mVulkan);
        mVulkan = nullptr;
    }
    
    if (mVkInst) {
        pl_vk_inst_destroy(&mVkInst);
        mVkInst = nullptr;
    }
    
    if (mLog) {
        pl_log_destroy(&mLog);
        mLog = nullptr;
    }
    
    mInitialized = false;
}

//-----------------------------------------------------------------------------
// Configuration
//-----------------------------------------------------------------------------

void PlaceboContext::SetConfig(const PlaceboConfig& config) {
    bool shadersChanged = (mConfig.enableCustomShaders != config.enableCustomShaders ||
                           mConfig.anime4kPreset != config.anime4kPreset ||
                           mConfig.shaderDirectory != config.shaderDirectory ||
                           mConfig.customShaderPaths != config.customShaderPaths);
    
    mConfig = config;
    mConfigDirty = true;
    
    // Reload shaders if shader config changed
    if (shadersChanged && mInitialized) {
        Logger::Log(LogLevel::Info, "[Placebo] Shader configuration changed, reloading...");
        ReloadShaders();
    }
}

void PlaceboContext::UpdateRenderParams() {
    // Start with defaults
    mRenderParams = pl_render_default_params;
    mDebandParams = pl_deband_default_params;
    mSigmoidParams = pl_sigmoid_default_params;
    mColorAdj = pl_color_adjustment_neutral;
    mColorMapParams = pl_color_map_default_params;
    mDitherParams = pl_dither_default_params;
    
    // Configure upscaler/downscaler
    mRenderParams.upscaler = GetFilterConfig(mConfig.upscaler);
    mRenderParams.downscaler = GetFilterConfig(mConfig.downscaler);
    mRenderParams.antiringing_strength = mConfig.antiringing;
    
    // Sigmoidization
    if (mConfig.sigmoidize) {
        mRenderParams.sigmoid_params = &mSigmoidParams;
    } else {
        mRenderParams.sigmoid_params = nullptr;
    }
    
    // Debanding
    if (mConfig.debanding) {
        mDebandParams.iterations = mConfig.debandIterations;
        mDebandParams.threshold = mConfig.debandThreshold;
        mDebandParams.radius = mConfig.debandRadius;
        mDebandParams.grain = mConfig.debandGrain;
        mRenderParams.deband_params = &mDebandParams;
    } else {
        mRenderParams.deband_params = nullptr;
    }
    
    // Color adjustment
    ApplyColorAdjustments();
    mRenderParams.color_adjustment = &mColorAdj;
    
    // Color mapping (for HDR)
    mRenderParams.color_map_params = &mColorMapParams;
    
    // Dithering
    if (mConfig.dithering) {
        mRenderParams.dither_params = &mDitherParams;
    } else {
        mRenderParams.dither_params = nullptr;
    }
    
    // Performance options
    mRenderParams.skip_anti_aliasing = mConfig.skipAntiAliasing;
    mRenderParams.preserve_mixing_cache = mConfig.preserveMixingCache;
    
    // Custom shaders (Anime4K, etc.)
    if (mFilterChain.HasActiveShaders()) {
        mRenderParams.hooks = mFilterChain.GetHooks();
        mRenderParams.num_hooks = static_cast<int>(mFilterChain.GetShaderCount());
        Logger::Log(LogLevel::Info, "[Placebo] Configured %d custom shader hooks", 
                    mRenderParams.num_hooks);
    } else {
        mRenderParams.hooks = nullptr;
        mRenderParams.num_hooks = 0;
    }
    
    mConfigDirty = false;
}

void PlaceboContext::RegisterBuiltinFilters() {
    mFilterChain.ClearBuiltinFilters();
    
    // Upscaler
    std::stringstream upscalerDesc;
    upscalerDesc << GetUpscalerName(mConfig.upscaler);
    if (mConfig.antiringing > 0.0f) {
        upscalerDesc << " (antiring=" << mConfig.antiringing << ")";
    }
    mFilterChain.RegisterBuiltinFilter(FilterStageType::BUILTIN_UPSCALER, 
                                        "Upscaler", upscalerDesc.str());
    
    // Downscaler
    mFilterChain.RegisterBuiltinFilter(FilterStageType::BUILTIN_UPSCALER,
                                        "Downscaler", GetUpscalerName(mConfig.downscaler));
    
    // Debanding
    if (mConfig.debanding) {
        std::stringstream debandDesc;
        debandDesc << "iter=" << mConfig.debandIterations 
                   << " thresh=" << mConfig.debandThreshold
                   << " grain=" << mConfig.debandGrain;
        mFilterChain.RegisterBuiltinFilter(FilterStageType::BUILTIN_DEBANDING,
                                            "Debanding", debandDesc.str());
    }
    
    // Sigmoidization
    if (mConfig.sigmoidize) {
        mFilterChain.RegisterBuiltinFilter(FilterStageType::BUILTIN_UPSCALER,
                                            "Sigmoidization", "Pre-upscale");
    }
    
    // Color adjustment
    if (mConfig.colorMode != PlaceboColorMode::COLOR_PASSTHROUGH) {
        std::stringstream colorDesc;
        colorDesc << "brightness=" << mConfig.brightness 
                  << " contrast=" << mConfig.contrast
                  << " saturation=" << mConfig.saturation;
        if (mConfig.gamma != 1.0f) colorDesc << " gamma=" << mConfig.gamma;
        mFilterChain.RegisterBuiltinFilter(FilterStageType::BUILTIN_COLOR_ADJUST,
                                            GetCurrentColorspace(), colorDesc.str());
    }
    
    // Dithering
    if (mConfig.dithering) {
        std::stringstream ditherDesc;
        ditherDesc << mConfig.ditherDepth << "-bit";
        mFilterChain.RegisterBuiltinFilter(FilterStageType::BUILTIN_DITHER,
                                            "Dithering", ditherDesc.str());
    }
}

std::string PlaceboContext::GetUpscalerName(PlaceboUpscaler upscaler) const {
    switch (upscaler) {
        case PlaceboUpscaler::BILINEAR:          return "Bilinear";
        case PlaceboUpscaler::BICUBIC:           return "Bicubic";
        case PlaceboUpscaler::HERMITE:           return "Hermite";
        case PlaceboUpscaler::LANCZOS:           return "Lanczos";
        case PlaceboUpscaler::EWA_LANCZOS:       return "EWA Lanczos";
        case PlaceboUpscaler::EWA_LANCZOSSHARP:  return "EWA Lanczos (Sharp)";
        case PlaceboUpscaler::EWA_LANCZOS4SHARPEST: return "EWA Lanczos 4 (Sharpest)";
        case PlaceboUpscaler::SPLINE16:          return "Spline16";
        case PlaceboUpscaler::SPLINE36:          return "Spline36";
        case PlaceboUpscaler::SPLINE64:          return "Spline64";
        case PlaceboUpscaler::MITCHELL:          return "Mitchell";
        case PlaceboUpscaler::CATMULL_ROM:       return "Catmull-Rom";
        case PlaceboUpscaler::ROBIDOUX:          return "Robidoux";
        case PlaceboUpscaler::ROBIDOUXSHARP:     return "Robidoux (Sharp)";
        case PlaceboUpscaler::GAUSSIAN:          return "Gaussian";
        case PlaceboUpscaler::OVERSAMPLE:        return "Oversample (Pixel Art)";
        default:                                  return "Unknown";
    }
}

const struct pl_filter_config* PlaceboContext::GetFilterConfig(PlaceboUpscaler upscaler) const {
    switch (upscaler) {
        case PlaceboUpscaler::BILINEAR:          return &pl_filter_bilinear;
        case PlaceboUpscaler::BICUBIC:           return &pl_filter_bicubic;
        case PlaceboUpscaler::HERMITE:           return &pl_filter_hermite;
        case PlaceboUpscaler::LANCZOS:           return &pl_filter_lanczos;
        case PlaceboUpscaler::EWA_LANCZOS:       return &pl_filter_ewa_lanczos;
        case PlaceboUpscaler::EWA_LANCZOSSHARP:  return &pl_filter_ewa_lanczossharp;
        case PlaceboUpscaler::EWA_LANCZOS4SHARPEST: return &pl_filter_ewa_lanczos4sharpest;
        case PlaceboUpscaler::SPLINE16:          return &pl_filter_spline16;
        case PlaceboUpscaler::SPLINE36:          return &pl_filter_spline36;
        case PlaceboUpscaler::SPLINE64:          return &pl_filter_spline64;
        case PlaceboUpscaler::MITCHELL:          return &pl_filter_mitchell;
        case PlaceboUpscaler::CATMULL_ROM:       return &pl_filter_catmull_rom;
        case PlaceboUpscaler::ROBIDOUX:          return &pl_filter_robidoux;
        case PlaceboUpscaler::ROBIDOUXSHARP:     return &pl_filter_robidouxsharp;
        case PlaceboUpscaler::GAUSSIAN:          return &pl_filter_gaussian;
        case PlaceboUpscaler::OVERSAMPLE:        return &pl_filter_oversample;
        default:                                  return &pl_filter_ewa_lanczos;
    }
}

void PlaceboContext::ApplyColorAdjustments() {
    mColorAdj = pl_color_adjustment_neutral;
    mColorAdj.brightness = mConfig.brightness;
    mColorAdj.contrast = mConfig.contrast;
    mColorAdj.saturation = mConfig.saturation;
    mColorAdj.gamma = mConfig.gamma;
    mColorAdj.hue = mConfig.hue;
}

void PlaceboContext::ConfigureColorspace() {
    if (!mSwapchain) return;
    
    struct pl_color_space csp = {0};
    
    switch (mConfig.colorMode) {
        case PlaceboColorMode::HDR10:
            csp.primaries = PL_COLOR_PRIM_BT_2020;
            csp.transfer = PL_COLOR_TRC_PQ;
            csp.hdr.min_luma = 0.0f;
            csp.hdr.max_luma = mConfig.peakNits;
            break;
            
        case PlaceboColorMode::HDR10_TONEMAP:
            // Use HDR10 output with tonemapping enabled
            csp.primaries = PL_COLOR_PRIM_BT_2020;
            csp.transfer = PL_COLOR_TRC_PQ;
            break;
            
        case PlaceboColorMode::SDR_ENHANCE:
        case PlaceboColorMode::COLOR_PASSTHROUGH:
        default:
            // Standard sRGB
            csp.primaries = PL_COLOR_PRIM_BT_709;
            csp.transfer = PL_COLOR_TRC_SRGB;
            break;
    }
    
    pl_swapchain_colorspace_hint(mSwapchain, &csp);
}

//-----------------------------------------------------------------------------
// Frame Rendering
//-----------------------------------------------------------------------------

void PlaceboContext::BeginFrame() {
    // libplacebo handles frame pacing internally
}

void PlaceboContext::EndFrame() {
    // Handled by swapchain
}

void PlaceboContext::RenderFrame(void* srcTexture, int srcWidth, int srcHeight) {
    if (!mInitialized || !mSwapchain || !mRenderer) {
        return;
    }
    
    // Update params if config changed
    if (mConfigDirty) {
        UpdateRenderParams();
    }
    
    // Get next swapchain frame
    struct pl_swapchain_frame frame;
    if (!pl_swapchain_start_frame(mSwapchain, &frame)) {
        // Window minimized or not ready
        return;
    }
    
    pl_tex srcTex = reinterpret_cast<pl_tex>(srcTexture);
    
    // Configure source frame
    struct pl_frame srcFrame = {0};
    srcFrame.num_planes = 1;
    srcFrame.planes[0].texture = srcTex;
    srcFrame.planes[0].components = 4;
    srcFrame.planes[0].component_mapping[0] = PL_CHANNEL_R;
    srcFrame.planes[0].component_mapping[1] = PL_CHANNEL_G;
    srcFrame.planes[0].component_mapping[2] = PL_CHANNEL_B;
    srcFrame.planes[0].component_mapping[3] = PL_CHANNEL_A;
    
    // Source colorspace (game renders in sRGB)
    srcFrame.repr = pl_color_repr_rgb;
    srcFrame.color = pl_color_space_srgb;
    
    // Source crop (full source image)
    srcFrame.crop.x0 = 0;
    srcFrame.crop.y0 = 0;
    srcFrame.crop.x1 = static_cast<float>(srcWidth);
    srcFrame.crop.y1 = static_cast<float>(srcHeight);
    
    // Configure target frame from swapchain
    struct pl_frame targetFrame;
    pl_frame_from_swapchain(&targetFrame, &frame);
    
    // Calculate letterbox/pillarbox coordinates to preserve aspect ratio
    float srcAspect = static_cast<float>(srcWidth) / static_cast<float>(srcHeight);
    float dstWidth = static_cast<float>(frame.fbo->params.w);
    float dstHeight = static_cast<float>(frame.fbo->params.h);
    float dstAspect = dstWidth / dstHeight;
    
    float scaledWidth, scaledHeight;
    float offsetX = 0.0f, offsetY = 0.0f;
    
    if (srcAspect > dstAspect) {
        // Source is wider - pillarbox (bars on top/bottom)
        scaledWidth = dstWidth;
        scaledHeight = dstWidth / srcAspect;
        offsetY = (dstHeight - scaledHeight) / 2.0f;
    } else {
        // Source is taller or equal - letterbox (bars on left/right)
        scaledHeight = dstHeight;
        scaledWidth = dstHeight * srcAspect;
        offsetX = (dstWidth - scaledWidth) / 2.0f;
    }
    
    // Apply letterbox/pillarbox crop to target
    targetFrame.crop.x0 = offsetX;
    targetFrame.crop.y0 = offsetY;
    targetFrame.crop.x1 = offsetX + scaledWidth;
    targetFrame.crop.y1 = offsetY + scaledHeight;
    
    // Render with upscaling and effects
    if (!pl_render_image(mRenderer, &srcFrame, &targetFrame, &mRenderParams)) {
        Logger::Log(LogLevel::Error, "pl_render_image failed");
    }
    
    // Submit and present
    if (!pl_swapchain_submit_frame(mSwapchain)) {
        Logger::Log(LogLevel::Error, "pl_swapchain_submit_frame failed");
    }
    
    // Swap buffers (may block for vsync)
    pl_swapchain_swap_buffers(mSwapchain);
}

void PlaceboContext::RenderFromPixels(const uint32_t* pixelData, int width, int height) {
    if (!mInitialized) return;
    
    // Ensure upload texture exists and is correct size
    if (!CreateUploadTexture(width, height)) {
        return;
    }
    
    // Upload pixel data to GPU
    struct pl_tex_transfer_params upload_params = {0};
    upload_params.tex = mUploadTex;
    upload_params.ptr = const_cast<void*>(static_cast<const void*>(pixelData));
    upload_params.row_pitch = width * 4;
    
    if (!pl_tex_upload(mVulkan->gpu, &upload_params)) {
        Logger::Log(LogLevel::Error, "pl_tex_upload failed");
        return;
    }
    
    // Render the uploaded frame
    RenderFrame(const_cast<void*>(static_cast<const void*>(mUploadTex)), width, height);
}

void PlaceboContext::Present(void* textureHandle, int srcWidth, int srcHeight,
                              int windowWidth, int windowHeight) {
    RenderFrame(textureHandle, srcWidth, srcHeight);
}

//-----------------------------------------------------------------------------
// Texture Management
//-----------------------------------------------------------------------------

void* PlaceboContext::CreateTexture(const TextureDesc& desc, const void* initialData) {
    if (!mVulkan || !mVulkan->gpu) {
        return nullptr;
    }
    
    // Determine format
    enum pl_fmt_type fmtType;
    int depth;
    
    switch (desc.format) {
        case TextureFormat::R8_UNORM:
            fmtType = PL_FMT_UNORM;
            depth = 8;
            break;
        case TextureFormat::RGBA8:
            fmtType = PL_FMT_UNORM;
            depth = 8;
            break;
        case TextureFormat::RGBA16F:
            fmtType = PL_FMT_FLOAT;
            depth = 16;
            break;
        case TextureFormat::RGBA32F:
            fmtType = PL_FMT_FLOAT;
            depth = 32;
            break;
        default:
            return nullptr;
    }
    
    int numComponents = (desc.format == TextureFormat::R8_UNORM) ? 1 : 4;
    
    // Find compatible format
    int caps = PL_FMT_CAP_SAMPLEABLE;
    if (desc.isStorage) {
        caps |= PL_FMT_CAP_STORABLE;
    }
    
    pl_fmt fmt = pl_find_fmt(mVulkan->gpu, fmtType, numComponents, depth, depth, static_cast<enum pl_fmt_caps>(caps));
    if (!fmt) {
        Logger::Log(LogLevel::Error, "Failed to find compatible texture format");
        return nullptr;
    }
    
    // Create texture
    struct pl_tex_params tex_params = {0};
    tex_params.w = desc.width;
    tex_params.h = desc.height;
    tex_params.format = fmt;
    tex_params.sampleable = true;
    tex_params.renderable = desc.isStorage;
    tex_params.storable = desc.isStorage;
    tex_params.blit_src = true;
    tex_params.blit_dst = true;
    tex_params.host_writable = true;
    tex_params.host_readable = true;
    tex_params.initial_data = initialData;
    
    pl_tex tex = pl_tex_create(mVulkan->gpu, &tex_params);
    return const_cast<void*>(static_cast<const void*>(tex));
}

void PlaceboContext::DestroyTexture(void* textureHandle) {
    if (!textureHandle || !mVulkan || !mVulkan->gpu) return;
    
    pl_tex tex = reinterpret_cast<pl_tex>(textureHandle);
    pl_tex_destroy(mVulkan->gpu, &tex);
}

void PlaceboContext::UpdateTexture(void* textureHandle, const void* data, int width, int height) {
    if (!textureHandle || !data || !mVulkan || !mVulkan->gpu) return;
    
    pl_tex tex = reinterpret_cast<pl_tex>(textureHandle);
    
    struct pl_tex_transfer_params params = {0};
    params.tex = tex;
    params.ptr = const_cast<void*>(data);
    
    pl_tex_upload(mVulkan->gpu, &params);
}

void PlaceboContext::ReadbackTexture(void* textureHandle, void* data, int size) {
    if (!textureHandle || !data || !mVulkan || !mVulkan->gpu) return;
    
    pl_tex tex = reinterpret_cast<pl_tex>(textureHandle);
    
    struct pl_tex_transfer_params params = {0};
    params.tex = tex;
    params.ptr = data;
    
    pl_tex_download(mVulkan->gpu, &params);
}

bool PlaceboContext::Reconfigure(int width, int height) {
    if (!mSwapchain) return false;
    
    mScreenWidth = width;
    mScreenHeight = height;
    
    if (!pl_swapchain_resize(mSwapchain, &width, &height)) {
        Logger::Log(LogLevel::Warning, "pl_swapchain_resize failed");
        return false;
    }
    return true;
}

//-----------------------------------------------------------------------------
// Compute Shader Interface (Not Used with High-Level API)
//-----------------------------------------------------------------------------

bool PlaceboContext::CreateComputeShader(const std::string& source, void** outShader) {
    // Not needed - using pl_renderer high-level API
    return false;
}

void PlaceboContext::Dispatch(void* shader, int x, int y, int z) {
    // Not used
}

void PlaceboContext::BindTexture(int slot, void* textureHandle) {
    // Not used
}

void PlaceboContext::BindUnorderedAccessView(int slot, void* textureHandle, TextureFormat format) {
    // Not used
}

void PlaceboContext::SetConstants(int slot, const void* data, int size) {
    // Not used
}

//-----------------------------------------------------------------------------
// Query Methods
//-----------------------------------------------------------------------------

bool PlaceboContext::IsHDRActive() const {
    if (!mSwapchain) return false;
    
    // Check if we requested HDR and it was granted
    // This is a simplified check - in practice you'd query the swapchain frame
    return (mConfig.colorMode == PlaceboColorMode::HDR10 ||
            mConfig.colorMode == PlaceboColorMode::HDR10_TONEMAP);
}

std::string PlaceboContext::GetCurrentColorspace() const {
    switch (mConfig.colorMode) {
        case PlaceboColorMode::COLOR_PASSTHROUGH:  return "Passthrough";
        case PlaceboColorMode::SDR_ENHANCE:  return "sRGB (Enhanced)";
        case PlaceboColorMode::HDR10:        return "HDR10 (PQ/BT.2020)";
        case PlaceboColorMode::HDR10_TONEMAP:return "HDR10 (Tonemapped)";
        default:                              return "Unknown";
    }
}

std::string PlaceboContext::GetDeviceName() const {
    if (!mVulkan || !mVulkan->gpu) {
        return "Unknown";
    }
    
    // libplacebo doesn't directly expose device name, but we can get it from Vulkan
    // For now, return a generic string
    return "Vulkan GPU";
}

std::vector<std::string> PlaceboContext::GetAvailableUpscalers() {
    std::vector<std::string> names;
    
    for (int i = 0; i < pl_num_filter_configs; i++) {
        const struct pl_filter_config* cfg = pl_filter_configs[i];
        if (cfg && cfg->name && (cfg->allowed & PL_FILTER_UPSCALING)) {
            names.push_back(cfg->name);
        }
    }
    
    // Sort alphabetically
    std::sort(names.begin(), names.end());
    
    return names;
}

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_HAVE_LIBPLACEBO
