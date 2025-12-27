#include "render_pipeline.h"
#include "display_scaler.h"
#include "logger.h"
#include "renderer_config.h"

#if FALLOUT_HAVE_LIBPLACEBO
#include "placebo_context.h"
#endif

#ifdef _WIN32
#include "hdr_utils.h"
#endif

#include <SDL.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <ctime>
#include <iomanip>

namespace fallout {
namespace renderer {

//-----------------------------------------------------------------------------
// Construction / Destruction
//-----------------------------------------------------------------------------

RenderPipeline::RenderPipeline() = default;

RenderPipeline::~RenderPipeline() {
    Shutdown();
}

//-----------------------------------------------------------------------------
// Lifecycle
//-----------------------------------------------------------------------------

bool RenderPipeline::Init(int inputWidth, int inputHeight, 
                           int outputWidth, int outputHeight, SDL_Window* window) {
    if (mInitialized) {
        return false;
    }

    Logger::Init("renderer.log");
    LogDiagnostic("Initializing RenderPipeline: %dx%d -> %dx%d", 
                  inputWidth, inputHeight, outputWidth, outputHeight);

    mWindow = window;
    mInputDimensions = {inputWidth, inputHeight};
    mWindowDimensions = {outputWidth, outputHeight};
    mRenderDimensions = CalculateRenderDimensions();

    LogDiagnostic("Render Resolution: %dx%d (Window: %dx%d)", 
                  mRenderDimensions.width, mRenderDimensions.height,
                  mWindowDimensions.width, mWindowDimensions.height);

    displayScalerInit(inputWidth, inputHeight);
    displayScalerUpdatePhysicalSize(outputWidth, outputHeight);

    InitializeDisplays();
    LoadConfiguration();
    mVerboseLogging = true;

    // Try libplacebo first (preferred), fallback to simple mode
#if FALLOUT_HAVE_LIBPLACEBO
    if (mConfiguredMode == RenderMode::LIBPLACEBO) {
        if (InitializePlaceboContext()) {
            mUsingPlacebo = true;
            mInitialized = true;
            LogDiagnostic("RenderPipeline initialized with libplacebo backend");
            return true;
        }
        LogDiagnostic("libplacebo initialization failed, falling back to simple mode");
    }
#endif

    // Simple mode fallback
    if (InitializeSimpleContext()) {
        mUsingPlacebo = false;
        mInitialized = true;
        LogDiagnostic("RenderPipeline initialized with simple backend");
        return true;
    }

    LogDiagnostic("Failed to initialize any rendering backend");
    return false;
}

Dimensions RenderPipeline::CalculateRenderDimensions() {
    float inputAspect = mInputDimensions.AspectRatio();
    float windowAspect = mWindowDimensions.AspectRatio();

    Dimensions result;
    if (windowAspect > inputAspect) {
        // Window wider than content (pillarbox)
        result.height = mWindowDimensions.height;
        result.width = static_cast<int>(mWindowDimensions.height * inputAspect);
    } else {
        // Window taller than content (letterbox)
        result.width = mWindowDimensions.width;
        result.height = static_cast<int>(mWindowDimensions.width / inputAspect);
    }
    
    // Ensure even dimensions
    result.width &= ~1;
    result.height &= ~1;
    return result;
}

void RenderPipeline::InitializeDisplays() {
    mPhantomDisplay = std::make_unique<PhantomDisplay>(
        mInputDimensions.width, mInputDimensions.height);
    mRealDisplay = std::make_unique<RealDisplay>(
        mWindowDimensions.width, mWindowDimensions.height);
}

#if FALLOUT_HAVE_LIBPLACEBO
bool RenderPipeline::InitializePlaceboContext() {
    LogDiagnostic("Initializing libplacebo context with Vulkan backend...");
    
    mPlaceboContext = std::make_unique<PlaceboContext>(
        mWindow, mWindowDimensions.width, mWindowDimensions.height);
    
    if (!mPlaceboContext->Init()) {
        LogDiagnostic("PlaceboContext::Init() failed");
        mPlaceboContext.reset();
        return false;
    }
    
    // Apply configuration
    mPlaceboContext->SetConfig(mPlaceboConfig);
    
    LogDiagnostic("libplacebo context initialized successfully");
    return true;
}
#endif

bool RenderPipeline::InitializeSimpleContext() {
    LogDiagnostic("Initializing simple rendering context...");
    // Simple mode just uses SDL for presentation
    // PhantomDisplay already handles palette conversion
    return true;
}

void RenderPipeline::Shutdown() {
#if FALLOUT_HAVE_LIBPLACEBO
    if (mPlaceboContext) {
        mPlaceboContext->Shutdown();
        mPlaceboContext.reset();
    }
#endif
    
    mPhantomDisplay.reset();
    mRealDisplay.reset();
    mInitialized = false;
    mUsingPlacebo = false;
}

bool RenderPipeline::Reconfigure(int outputWidth, int outputHeight) {
    if (mWindowDimensions.width == outputWidth && 
        mWindowDimensions.height == outputHeight) {
        return true;
    }

    LogDiagnostic("Reconfiguring output: %dx%d -> %dx%d", 
                  mWindowDimensions.width, mWindowDimensions.height, 
                  outputWidth, outputHeight);
    
    mWindowDimensions.width = outputWidth;
    mWindowDimensions.height = outputHeight;
    mRenderDimensions = CalculateRenderDimensions();

#if FALLOUT_HAVE_LIBPLACEBO
    if (mUsingPlacebo && mPlaceboContext) {
        return mPlaceboContext->Reconfigure(outputWidth, outputHeight);
    }
#endif
    
    return true;
}

//-----------------------------------------------------------------------------
// Input Methods
//-----------------------------------------------------------------------------

bool RenderPipeline::SetIndexedInput(SDL_Surface* surface) {
    if (!mInitialized || !mPhantomDisplay) {
        LogDiagnostic("SetIndexedInput failed: Not initialized");
        return false;
    }

    if (!surface || !surface->pixels) {
        LogDiagnostic("SetIndexedInput failed: Null surface or pixels");
        return false;
    }

    uint32_t paletteRGBA[256];
    ConvertPaletteToRGBA(surface, paletteRGBA);
    
    uint8_t* pixels = static_cast<uint8_t*>(surface->pixels);
    mPhantomDisplay->SetData(pixels, paletteRGBA, surface->pitch);
    return true;
}

void RenderPipeline::ConvertPaletteToRGBA(SDL_Surface* surface, uint32_t* paletteRGBA) {
    if (surface->format && surface->format->palette) {
        SDL_Color* colors = surface->format->palette->colors;
        for (int i = 0; i < 256; ++i) {
            // Pack as 0xAABBGGRR for OpenGL/Vulkan (Little Endian)
            paletteRGBA[i] = 0xFF000000 | 
                             (colors[i].b << 16) | 
                             (colors[i].g << 8) | 
                             colors[i].r;
        }
    } else {
        LogDiagnostic("SetIndexedInput: Surface has no palette!");
        memset(paletteRGBA, 0, 256 * sizeof(uint32_t));
    }
}

bool RenderPipeline::SetRgbaInput(const uint32_t* rgbaBuffer) {
    if (!mInitialized || !mPhantomDisplay) return false;
    mPhantomDisplay->SetData(rgbaBuffer);
    return true;
}

//-----------------------------------------------------------------------------
// Frame Execution
//-----------------------------------------------------------------------------

void RenderPipeline::Dispatch() {
    if (!mInitialized || !mPhantomDisplay) return;

    HandleScreenshotRequest();

#if FALLOUT_HAVE_LIBPLACEBO
    if (mUsingPlacebo) {
        DispatchPlacebo();
        return;
    }
#endif

    DispatchSimple();
}

#if FALLOUT_HAVE_LIBPLACEBO
void RenderPipeline::DispatchPlacebo() {
    if (!mPlaceboContext) return;
    
    bool capture = mScreenshotManager.IsCaptureRequested();
    
    // Render frame through libplacebo
    // PlaceboContext handles all upscaling, effects, and presentation
    mPlaceboContext->RenderFromPixels(
        static_cast<const uint32_t*>(mPhantomDisplay->GetPixels()),
        mPhantomDisplay->GetWidth(),
        mPhantomDisplay->GetHeight()
    );
    
    if (capture) {
        SaveCpuScreenshot("libplacebo_input");
        mScreenshotManager.EndCapture();
    }
}
#endif

void RenderPipeline::DispatchSimple() {
    // Simple mode: just present via SDL
    // The game's native SDL rendering handles this
    bool capture = mScreenshotManager.IsCaptureRequested();
    
    if (capture) {
        SaveCpuScreenshot("simple_input");
        mScreenshotManager.EndCapture();
    }
}

void RenderPipeline::HandleScreenshotRequest() {
    const Uint8* state = SDL_GetKeyboardState(nullptr);
    if (state[SDL_SCANCODE_F8]) {
        if (!mF8Pressed) {
            mScreenshotManager.RequestCapture();
            mF8Pressed = true;
        }
    } else {
        mF8Pressed = false;
    }
}

const void* RenderPipeline::GetOutput() {
    if (!mInitialized) return nullptr;
    return mPhantomDisplay ? mPhantomDisplay->GetPixels() : nullptr;
}

//-----------------------------------------------------------------------------
// Configuration
//-----------------------------------------------------------------------------

void RenderPipeline::LoadConfiguration() {
    auto& config = RendererConfig::GetInstance();
    
    if (!config.Load("renderer_config.ini")) {
        LogDiagnostic("Failed to load renderer_config.ini, using defaults");
    } else {
        LogDiagnostic("Loaded renderer_config.ini");
    }

    // Parse render mode (0=simple, 1=libplacebo)
    int mode = config.GetInt("General", "Mode", 1);
    switch (mode) {
        case 0:
            mConfiguredMode = RenderMode::SIMPLE;
            break;
        case 1:
        default:
#if FALLOUT_HAVE_LIBPLACEBO
            mConfiguredMode = RenderMode::LIBPLACEBO;
#else
            mConfiguredMode = RenderMode::SIMPLE;
            LogDiagnostic("libplacebo not available, using simple mode");
#endif
            break;
    }
    
    mVerboseLogging = config.GetBool("General", "VerboseLogging", false);

#if FALLOUT_HAVE_LIBPLACEBO
    LoadPlaceboConfiguration(config);
#endif

    LogDiagnostic("Configuration Loaded: Mode=%d", static_cast<int>(mConfiguredMode));
}

#if FALLOUT_HAVE_LIBPLACEBO
void RenderPipeline::LoadPlaceboConfiguration(RendererConfig& config) {
    // Upscaler/Downscaler
    std::string upscaler = config.GetString("libplacebo", "Upscaler", "ewa_lanczos");
    mPlaceboConfig.upscaler = ParseUpscaler(upscaler);
    
    std::string downscaler = config.GetString("libplacebo", "Downscaler", "lanczos");
    mPlaceboConfig.downscaler = ParseUpscaler(downscaler);
    
    mPlaceboConfig.antiringing = config.GetFloat("libplacebo", "Antiringing", 0.5f);
    mPlaceboConfig.sigmoidize = config.GetBool("libplacebo", "Sigmoidize", true);
    
    // Debanding
    mPlaceboConfig.debanding = config.GetBool("libplacebo", "Debanding", true);
    mPlaceboConfig.debandIterations = config.GetInt("libplacebo", "DebandIterations", 1);
    mPlaceboConfig.debandThreshold = config.GetFloat("libplacebo", "DebandThreshold", 3.0f);
    mPlaceboConfig.debandRadius = config.GetFloat("libplacebo", "DebandRadius", 16.0f);
    mPlaceboConfig.debandGrain = config.GetFloat("libplacebo", "DebandGrain", 4.0f);
    
    // Color adjustments
    mPlaceboConfig.brightness = config.GetFloat("libplacebo", "Brightness", 0.0f);
    mPlaceboConfig.contrast = config.GetFloat("libplacebo", "Contrast", 1.1f);
    mPlaceboConfig.saturation = config.GetFloat("libplacebo", "Saturation", 1.2f);
    mPlaceboConfig.gamma = config.GetFloat("libplacebo", "Gamma", 1.0f);
    mPlaceboConfig.hue = config.GetFloat("libplacebo", "Hue", 0.0f);
    
    // Color mode
    std::string colorMode = config.GetString("libplacebo", "ColorMode", "sdr_enhance");
    mPlaceboConfig.colorMode = ParseColorMode(colorMode);
    
    // HDR settings
    mPlaceboConfig.peakNits = config.GetFloat("libplacebo", "PeakNits", 1000.0f);
    mPlaceboConfig.paperWhiteNits = config.GetFloat("libplacebo", "PaperWhiteNits", 203.0f);
    mPlaceboConfig.hdrPassthrough = config.GetBool("libplacebo", "HDRPassthrough", false);
    
    // Dithering
    mPlaceboConfig.dithering = config.GetBool("libplacebo", "Dithering", true);
    mPlaceboConfig.ditherDepth = config.GetInt("libplacebo", "DitherDepth", 8);
    
    // Performance
    mPlaceboConfig.skipAntiAliasing = config.GetBool("libplacebo", "SkipAntiAliasing", false);
    mPlaceboConfig.preserveMixingCache = config.GetBool("libplacebo", "PreserveMixingCache", true);
    
    // Custom shaders (Anime4K via libplacebo's shader system)
    mPlaceboConfig.enableCustomShaders = config.GetBool("libplacebo", "EnableCustomShaders", false);
    mPlaceboConfig.shaderDirectory = config.GetString("libplacebo", "ShaderDirectory", "data/shaders");
    
    // Anime4K preset
    std::string preset = config.GetString("libplacebo", "Anime4KPreset", "none");
    mPlaceboConfig.anime4kPreset = ParseAnime4KPreset(preset);
    
    // Custom shader paths (semicolon-separated)
    std::string customShaders = config.GetString("libplacebo", "CustomShaders", "");
    if (!customShaders.empty()) {
        std::stringstream ss(customShaders);
        std::string path;
        while (std::getline(ss, path, ';')) {
            if (!path.empty()) {
                mPlaceboConfig.customShaderPaths.push_back(path);
            }
        }
    }
    
    LogDiagnostic("[libplacebo] Config: upscaler=%s, debanding=%s, customShaders=%s, preset=%s",
                  upscaler.c_str(),
                  mPlaceboConfig.debanding ? "on" : "off",
                  mPlaceboConfig.enableCustomShaders ? "on" : "off",
                  preset.c_str());
}

PlaceboUpscaler RenderPipeline::ParseUpscaler(const std::string& name) {
    static const std::unordered_map<std::string, PlaceboUpscaler> map = {
        {"bilinear", PlaceboUpscaler::BILINEAR},
        {"bicubic", PlaceboUpscaler::BICUBIC},
        {"hermite", PlaceboUpscaler::HERMITE},
        {"lanczos", PlaceboUpscaler::LANCZOS},
        {"ewa_lanczos", PlaceboUpscaler::EWA_LANCZOS},
        {"ewa_lanczossharp", PlaceboUpscaler::EWA_LANCZOSSHARP},
        {"ewa_lanczos4sharpest", PlaceboUpscaler::EWA_LANCZOS4SHARPEST},
        {"spline16", PlaceboUpscaler::SPLINE16},
        {"spline36", PlaceboUpscaler::SPLINE36},
        {"spline64", PlaceboUpscaler::SPLINE64},
        {"mitchell", PlaceboUpscaler::MITCHELL},
        {"catmull_rom", PlaceboUpscaler::CATMULL_ROM},
        {"robidoux", PlaceboUpscaler::ROBIDOUX},
        {"robidouxsharp", PlaceboUpscaler::ROBIDOUXSHARP},
        {"gaussian", PlaceboUpscaler::GAUSSIAN},
        {"oversample", PlaceboUpscaler::OVERSAMPLE}
    };
    
    auto it = map.find(name);
    return (it != map.end()) ? it->second : PlaceboUpscaler::EWA_LANCZOS;
}

PlaceboColorMode RenderPipeline::ParseColorMode(const std::string& name) {
    if (name == "passthrough") return PlaceboColorMode::COLOR_PASSTHROUGH;
    if (name == "sdr_enhance") return PlaceboColorMode::SDR_ENHANCE;
    if (name == "hdr10") return PlaceboColorMode::HDR10;
    if (name == "hdr10_tonemap") return PlaceboColorMode::HDR10_TONEMAP;
    return PlaceboColorMode::SDR_ENHANCE;
}

Anime4KPreset RenderPipeline::ParseAnime4KPreset(const std::string& name) {
    if (name == "mode_a") return Anime4KPreset::MODE_A;
    if (name == "mode_b") return Anime4KPreset::MODE_B;
    if (name == "mode_c") return Anime4KPreset::MODE_C;
    if (name == "mode_a_hq") return Anime4KPreset::MODE_A_HQ;
    if (name == "mode_b_hq") return Anime4KPreset::MODE_B_HQ;
    if (name == "mode_c_hq") return Anime4KPreset::MODE_C_HQ;
    if (name == "custom") return Anime4KPreset::CUSTOM;
    return Anime4KPreset::NONE;
}
#endif // FALLOUT_HAVE_LIBPLACEBO

//-----------------------------------------------------------------------------
// Utility
//-----------------------------------------------------------------------------

void RenderPipeline::LogDiagnostic(const char* format, ...) {
    bool isError = (strstr(format, "Failed") != nullptr || strstr(format, "Error") != nullptr);
    
    if (!mVerboseLogging && !isError) return;
    
    va_list args;
    va_start(args, format);
    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    Logger::Log(isError ? LogLevel::Error : LogLevel::Info, "%s", buffer);
}

void RenderPipeline::SaveCpuScreenshot(const std::string& stageName) {
    if (!mPhantomDisplay) {
        Logger::Log(LogLevel::Error, "SaveCpuScreenshot: No phantom display");
        return;
    }
    
    const void* pixels = mPhantomDisplay->GetPixels();
    int width = mPhantomDisplay->GetWidth();
    int height = mPhantomDisplay->GetHeight();
    
    if (!pixels || width <= 0 || height <= 0) {
        Logger::Log(LogLevel::Error, "SaveCpuScreenshot: Invalid pixel data");
        return;
    }
    
    // Create screenshots directory
    std::filesystem::path dir("screenshots");
    if (!std::filesystem::exists(dir)) {
        std::filesystem::create_directory(dir);
    }
    
    // Generate filename with timestamp
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    std::stringstream ss;
    ss << "screenshots/shot_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_" << stageName << ".bmp";
    std::string filename = ss.str();
    
    int stride = width * 4;
    
    #if SDL_BYTEORDER == SDL_BIG_ENDIAN
        uint32_t rmask = 0xff000000;
        uint32_t gmask = 0x00ff0000;
        uint32_t bmask = 0x0000ff00;
        uint32_t amask = 0x000000ff;
    #else
        uint32_t rmask = 0x000000ff;
        uint32_t gmask = 0x0000ff00;
        uint32_t bmask = 0x00ff0000;
        uint32_t amask = 0xff000000;
    #endif
    
    SDL_Surface* surf = SDL_CreateRGBSurfaceFrom(
        const_cast<void*>(pixels), width, height, 32, stride,
        rmask, gmask, bmask, amask
    );
    
    if (surf) {
        if (SDL_SaveBMP(surf, filename.c_str()) == 0) {
            Logger::Log(LogLevel::Info, "Screenshot saved: %s (%dx%d)", filename.c_str(), width, height);
        } else {
            Logger::Log(LogLevel::Error, "Failed to save screenshot: %s", SDL_GetError());
        }
        SDL_FreeSurface(surf);
    }
}

} // namespace renderer
} // namespace fallout
