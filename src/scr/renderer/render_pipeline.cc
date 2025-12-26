#include "render_pipeline.h"
#include "opengl_context.h"
#include "vulkan_context.h"
#include "display_scaler.h"
#include "logger.h"
#include "renderer_config.h"
#include "generic_shader_pass.h"

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

    if (!InitializeContext()) {
        return false;
    }

    if (!InitializeBuffers()) {
        return false;
    }

    SetupPasses();
    mInitialized = true;
    LogDiagnostic("RenderPipeline initialized successfully");
    return true;
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

bool RenderPipeline::InitializeContext() {
    // Check configuration for backend preference
    auto& config = RendererConfig::GetInstance();
    std::string backendPref = config.GetString("General", "Backend", "auto");
    
    bool useVulkan = false;
    
    if (backendPref == "vulkan") {
        useVulkan = true;
        LogDiagnostic("Backend preference: Vulkan (forced)");
    } else if (backendPref == "opengl") {
        useVulkan = false;
        LogDiagnostic("Backend preference: OpenGL (forced)");
    } else {
        // Auto-detect: use Vulkan if HDR is enabled and Vulkan is available
#if FALLOUT_HAVE_VULKAN
        if (IsHDREnabled()) {
            useVulkan = true;
            LogDiagnostic("Backend preference: Vulkan (auto - HDR detected)");
        } else {
            LogDiagnostic("Backend preference: OpenGL (auto - no HDR)");
        }
#else
        LogDiagnostic("Backend preference: OpenGL (Vulkan not available)");
#endif
    }
    
#if FALLOUT_HAVE_VULKAN
    if (useVulkan) {
        LogDiagnostic("Initializing Vulkan context for HDR rendering...");
        auto vulkanCtx = std::make_unique<VulkanContext>(mWindow, 
                                                          mWindowDimensions.width, 
                                                          mWindowDimensions.height, 
                                                          true);
        if (vulkanCtx->Init()) {
            mContext = std::move(vulkanCtx);
            mUsingVulkan = true;
            LogDiagnostic("Vulkan context initialized successfully");
            return true;
        } else {
            LogDiagnostic("Vulkan initialization failed, falling back to OpenGL");
        }
    }
#endif
    
    // Fall back to OpenGL
    LogDiagnostic("Initializing OpenGL context...");
    mContext = std::make_unique<OpenGLContext>(mWindow);
    mUsingVulkan = false;
    if (!mContext->Init()) {
        LogDiagnostic("Failed to initialize OpenGL context");
        return false;
    }
    LogDiagnostic("OpenGL context initialized successfully");
    return true;
}

bool RenderPipeline::InitializeBuffers() {
    if (!mBuffers.Init(*mContext, 
                        mInputDimensions.width, mInputDimensions.height,
                        mRenderDimensions.width, mRenderDimensions.height)) {
        LogDiagnostic("Failed to initialize BufferManager");
        return false;
    }
    return true;
}

void RenderPipeline::Shutdown() {
    if (!mContext) return;
    
    CleanupPasses();
    mBuffers.Shutdown(*mContext);
    mContext->Shutdown();
    mPhantomDisplay.reset();
    mRealDisplay.reset();
    mInitialized = false;
}

bool RenderPipeline::Reconfigure(int outputWidth, int outputHeight) {
    if (mWindowDimensions.width == outputWidth && 
        mWindowDimensions.height == outputHeight) {
        return true;
    }

    LogDiagnostic("Reconfiguring output: %dx%d -> %dx%d", 
                  mWindowDimensions.width, mWindowDimensions.height, 
                  outputWidth, outputHeight);
    
    // For Vulkan, just reconfigure the swapchain - don't recreate everything
    if (mUsingVulkan && mContext) {
        mWindowDimensions.width = outputWidth;
        mWindowDimensions.height = outputHeight;
        
        // Recalculate render dimensions
        float aspectRatio = static_cast<float>(mInputDimensions.width) / mInputDimensions.height;
        int maxScaleWidth = outputWidth / mInputDimensions.width;
        int maxScaleHeight = outputHeight / mInputDimensions.height;
        int scale = std::max(1, std::min(maxScaleWidth, maxScaleHeight));
        mRenderDimensions.width = mInputDimensions.width * scale;
        mRenderDimensions.height = mInputDimensions.height * scale;
        LogDiagnostic("[DEBUG] Vulkan Reconfigure: aspectRatio=%.3f, maxScaleWidth=%d, maxScaleHeight=%d, scale=%d, renderDims=%dx%d", aspectRatio, maxScaleWidth, maxScaleHeight, scale, mRenderDimensions.width, mRenderDimensions.height);
        return mContext->Reconfigure(outputWidth, outputHeight);
    }
    
    // For OpenGL, do full reinit (window context may need recreation)
    Dimensions savedInput = mInputDimensions;
    SDL_Window* savedWindow = mWindow;

    Shutdown();
    return Init(savedInput.width, savedInput.height, outputWidth, outputHeight, savedWindow);
}

//-----------------------------------------------------------------------------
// Pass Management
//-----------------------------------------------------------------------------

void RenderPipeline::SetupPasses() {
    CleanupPasses();

    RenderSurface inputSurface{nullptr, mInputDimensions.width, mInputDimensions.height, TextureFormat::RGBA8};
    RenderSurface outputSurface{nullptr, mRenderDimensions.width, mRenderDimensions.height, TextureFormat::RGBA16F};

    // Vulkan backend currently uses direct presentation without shader passes
    // Anime4K shaders need to be ported to Vulkan GLSL format first
    if (mUsingVulkan) {
        LogDiagnostic("[SCALER] Vulkan mode: Using direct presentation (HDR passthrough)");
        // No shader passes needed - VulkanContext::Present() handles scaling
        return;
    }

    if (mConfiguredMode == RenderMode::ANIME4K) {
        SetupAnime4KPipeline(inputSurface, outputSurface);
    } else {
        SetupSimplePipeline(inputSurface, outputSurface);
    }
}

void RenderPipeline::SetupSimplePipeline(const RenderSurface& inputSurface, 
                                          const RenderSurface& outputSurface) {
    LogDiagnostic("[SCALER] Using Default Scaler");
    auto scalerPass = std::make_unique<ScalerPass>();
    if (scalerPass->Init(*mContext, inputSurface, outputSurface)) {
        mScalerPass = std::move(scalerPass);
    }
}

void RenderPipeline::SetupAnime4KPipeline(const RenderSurface& inputSurface, 
                                           const RenderSurface& outputSurface) {
    LogDiagnostic("[SCALER] Setting up Anime4K Pipeline");
    
    int currentW = mInputDimensions.width;
    int currentH = mInputDimensions.height;
    RenderSurface currentInput = inputSurface;

    // Collect enabled steps
    std::vector<PipelineStepConfig*> steps = {
        &mAnime4KConfig.prePass,
        &mAnime4KConfig.clean1,
        &mAnime4KConfig.clean2,
        &mAnime4KConfig.scale1,
        &mAnime4KConfig.optimize,
        &mAnime4KConfig.scale2,
        &mAnime4KConfig.polish,
        &mAnime4KConfig.postPass
    };

    for (auto* step : steps) {
        if (!step->enabled || step->shaderPath.empty()) continue;

        std::string path = "data/shaders/" + step->shaderPath;
        int nextW = currentW * step->scaleFactor;
        int nextH = currentH * step->scaleFactor;
        
        TextureDesc desc{nextW, nextH, TextureFormat::RGBA16F};
        void* handle = mContext->CreateTexture(desc);
        if (!handle) {
            LogDiagnostic("Failed to create buffer for %s", step->shaderPath.c_str());
            continue;
        }
        
        RenderSurface output{handle, nextW, nextH, TextureFormat::RGBA16F};
        mScalingBuffers.push_back(output);

        auto pass = std::make_unique<GenericShaderPass>(path);
        if (pass->Init(*mContext, currentInput, output)) {
            mAnime4KPasses.push_back(std::move(pass));
            LogDiagnostic("Added Anime4K Pass: %s (%dx%d -> %dx%d)", 
                          step->shaderPath.c_str(), currentW, currentH, nextW, nextH);
            currentW = nextW;
            currentH = nextH;
            currentInput = output;
        } else {
            LogDiagnostic("Failed to init Anime4K Pass: %s", step->shaderPath.c_str());
        }
    }

    // Final scaler to target dimensions
    LogDiagnostic("[SCALER] Final scale from %dx%d to %dx%d", 
                  currentW, currentH, mRenderDimensions.width, mRenderDimensions.height);
    
    auto scalerPass = std::make_unique<ScalerPass>();
    if (scalerPass->Init(*mContext, currentInput, outputSurface)) {
        mScalerPass = std::move(scalerPass);
    } else {
        LogDiagnostic("Failed to init final ScalerPass");
    }
}

void RenderPipeline::CleanupPasses() {
    if (!mContext) return;
    
    for (auto& pass : mAnime4KPasses) {
        pass->Shutdown(*mContext);
    }
    mAnime4KPasses.clear();

    if (mScalerPass) {
        mScalerPass->Shutdown(*mContext);
        mScalerPass.reset();
    }

    for (auto& buf : mScalingBuffers) {
        if (buf.handle) {
            mContext->DestroyTexture(buf.handle);
        }
    }
    mScalingBuffers.clear();
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
    
    // Debug: Check if palette has non-black colors
    int nonBlackPalette = 0;
    for (int i = 0; i < 256; i++) {
        if ((paletteRGBA[i] & 0x00FFFFFF) != 0) {
            nonBlackPalette++;
        }
    }
    
    // Debug: Check if indexed pixels have non-zero indices
    uint8_t* pixels = static_cast<uint8_t*>(surface->pixels);
    int nonZeroIndex = 0;
    const int sampleSize = std::min(1024, surface->w * surface->h);
    for (int i = 0; i < sampleSize; i++) {
        if (pixels[i] != 0) {
            nonZeroIndex++;
        }
    }
    
    // Detailed logging every 60 frames
    static int frameCount = 0;
    frameCount++;
    if (frameCount % 60 == 0) {
        LogDiagnostic("[STAGE1-RAW] SetIndexedInput: dims=%dx%d, pitch=%d", surface->w, surface->h, surface->pitch);
        LogDiagnostic("[STAGE1-RAW] First 4 indexed pixels: %d, %d, %d, %d", 
                      pixels[0], pixels[1], pixels[2], pixels[3]);
        
        // Sample from middle of screen
        int midY = surface->h / 2;
        int midX = surface->w / 2;
        int midIdx = midY * surface->pitch + midX;
        LogDiagnostic("[STAGE1-RAW] Mid-screen[%d,%d] index=%d", midX, midY, pixels[midIdx]);
        
        // Show what palette colors these indices map to
        LogDiagnostic("[STAGE1-RAW] Palette lookup: idx[0]=%d -> 0x%08X, idx[mid]=%d -> 0x%08X",
                      pixels[0], paletteRGBA[pixels[0]], 
                      pixels[midIdx], paletteRGBA[pixels[midIdx]]);
        
        LogDiagnostic("[STAGE1-RAW] Stats: paletteNonBlack=%d/256, nonZeroIndices=%d/%d", 
                      nonBlackPalette, nonZeroIndex, sampleSize);
    }
    
    mPhantomDisplay->SetData(pixels, paletteRGBA, surface->pitch);
    return true;
}

void RenderPipeline::ConvertPaletteToRGBA(SDL_Surface* surface, uint32_t* paletteRGBA) {
    if (surface->format && surface->format->palette) {
        SDL_Color* colors = surface->format->palette->colors;
        LogDiagnostic("[DEBUG] ConvertPalette: ncolors=%d, first4colors=[(R%d,G%d,B%d) (R%d,G%d,B%d) (R%d,G%d,B%d) (R%d,G%d,B%d)]", 
                      surface->format->palette->ncolors,
                      colors[0].r, colors[0].g, colors[0].b,
                      colors[1].r, colors[1].g, colors[1].b,
                      colors[2].r, colors[2].g, colors[2].b,
                      colors[3].r, colors[3].g, colors[3].b);
        for (int i = 0; i < 256; ++i) {
            // Pack as 0xAABBGGRR for OpenGL (Little Endian)
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
    bool capture = mScreenshotManager.IsCaptureRequested();

    // Swap to next frame's buffers
    mBuffers.SwapBuffers();
    mContext->BeginFrame();

    // Upload input data
    if (!mBuffers.UploadInput(*mContext, mPhantomDisplay->GetPixels(), 
                               mPhantomDisplay->GetWidth() * mPhantomDisplay->GetHeight() * 4)) {
        LogDiagnostic("Failed to upload input");
    }

    // For Vulkan HDR mode, skip shader passes and present input directly
    // (Vulkan shaders will be implemented separately)
    if (mUsingVulkan) {
        LogDiagnostic("[DEBUG] Vulkan Dispatch: Presenting input surface %p (%dx%d) to window %dx%d", mBuffers.GetInputSurface().handle, mInputDimensions.width, mInputDimensions.height, mWindowDimensions.width, mWindowDimensions.height);
        
        // For Vulkan mode, save CPU-side screenshot since GPU readback isn't implemented
        if (capture) {
            SaveCpuScreenshot("00_VulkanInput");
            mScreenshotManager.EndCapture();
        }
        
        mContext->Present(mBuffers.GetInputSurface().handle, 
                          mInputDimensions.width, mInputDimensions.height,
                          mWindowDimensions.width, mWindowDimensions.height);
        mContext->EndFrame();
        return;
    }

    // Execute filter chain (OpenGL path)
    RenderSurface currentInput = mBuffers.GetInputSurface();
    
    if (capture) {
        mScreenshotManager.Capture(*mContext, currentInput, "00_Input");
    }

    int passIndex = 1;
    RenderSurface scalerOutput = mBuffers.GetOutputSurface();
    
    ExecuteAnime4KChain(currentInput, capture, passIndex);
    ExecuteScalerPass(currentInput, scalerOutput, capture, passIndex);

    if (capture) {
        mScreenshotManager.EndCapture();
    }

    mContext->EndFrame();

    // Present to screen
    mContext->Present(mBuffers.GetOutputSurface().handle, 
                      mRenderDimensions.width, mRenderDimensions.height,
                      mWindowDimensions.width, mWindowDimensions.height);
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

void RenderPipeline::ExecuteAnime4KChain(RenderSurface& currentInput, bool capture, int& passIndex) {
    if (mAnime4KPasses.empty()) return;

    RenderSurface chainInput = currentInput;
    for (size_t i = 0; i < mAnime4KPasses.size(); ++i) {
        RenderSurface chainOutput = mScalingBuffers[i];
        mAnime4KPasses[i]->Execute(*mContext, chainInput, chainOutput);
        
        if (capture) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%02d_Anime4K_%d_%s", 
                     passIndex++, static_cast<int>(i), 
                     mAnime4KPasses[i]->GetName().c_str());
            mScreenshotManager.Capture(*mContext, chainOutput, buf);
        }
        
        chainInput = chainOutput;
    }
    currentInput = chainInput;
}

void RenderPipeline::ExecuteScalerPass(const RenderSurface& input, const RenderSurface& output,
                                        bool capture, int passIndex) {
    if (!mScalerPass) {
        LogDiagnostic("Dispatch: No Scaler Pass!");
        return;
    }

    mScalerPass->Execute(*mContext, input, output);
    
    if (capture) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%02d_%s", passIndex, mScalerPass->GetName().c_str());
        mScreenshotManager.Capture(*mContext, output, buf);
    }
}

const void* RenderPipeline::GetOutput() {
    if (!mInitialized) return nullptr;
    return mBuffers.ReadbackOutput(*mContext);
}

//-----------------------------------------------------------------------------
// Configuration
//-----------------------------------------------------------------------------

void RenderPipeline::LoadConfiguration() {
    auto& config = RendererConfig::GetInstance();
    
    if (!config.Load("renderer_config.ini")) {
        LogDiagnostic("Failed to load renderer_config.ini from current directory");
    } else {
        LogDiagnostic("Loaded renderer_config.ini");
    }

    int mode = config.GetInt("General", "Mode", 0);
    mConfiguredMode = (mode == 1) ? RenderMode::ANIME4K : RenderMode::SIMPLE;
    mVerboseLogging = config.GetBool("General", "VerboseLogging", false);

    // Load Anime4K configuration
    auto loadStep = [&](PipelineStepConfig& step, const std::string& enableKey, 
                        const std::string& shaderKey, const std::string& defaultShader,
                        bool defaultEnabled, int scale) {
        step.enabled = config.GetBool("Anime4K", enableKey, defaultEnabled);
        step.shaderPath = config.GetString("Anime4K", shaderKey, defaultShader);
        step.scaleFactor = scale;
    };

    loadStep(mAnime4KConfig.prePass, "EnablePrePass", "PrePassShader", "", false, 1);
    loadStep(mAnime4KConfig.clean1, "EnableClean1", "Clean1Shader", "Anime4K_Clamp_Highlights.glsl", true, 1);
    loadStep(mAnime4KConfig.clean2, "EnableClean2", "Clean2Shader", "Anime4K_Restore_CNN_M.glsl", true, 1);
    loadStep(mAnime4KConfig.scale1, "EnableScale1", "Scale1Shader", "Anime4K_Upscale_CNN_x2_L.glsl", true, 2);
    loadStep(mAnime4KConfig.optimize, "EnableOptimize", "OptimizeShader", "Anime4K_AutoDownscalePre_x4.glsl", true, 1);
    loadStep(mAnime4KConfig.scale2, "EnableScale2", "Scale2Shader", "Anime4K_Upscale_CNN_x2_M.glsl", true, 2);
    loadStep(mAnime4KConfig.polish, "EnablePolish", "PolishShader", "Anime4K_Thin_HQ.glsl", true, 1);
    loadStep(mAnime4KConfig.postPass, "EnablePostPass", "PostPassShader", "", false, 1);

    LogDiagnostic("Configuration Loaded: Mode=%d (Configured=%d)", mode, static_cast<int>(mConfiguredMode));
}

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
    
    // Create SDL surface - our data is in ABGR format (0xAABBGGRR in little-endian memory)
    // which appears as RGBA when read byte-by-byte
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
        if (SDL_SaveBMP(surf, filename.c_str()) != 0) {
            Logger::Log(LogLevel::Error, "SaveCpuScreenshot: Failed to save %s: %s", filename.c_str(), SDL_GetError());
        } else {
            Logger::Log(LogLevel::Info, "SaveCpuScreenshot: Saved %s (%dx%d)", filename.c_str(), width, height);
            
            // Also log some pixel samples from the saved data
            const uint32_t* px = static_cast<const uint32_t*>(pixels);
            Logger::Log(LogLevel::Info, "SaveCpuScreenshot: first4pixels=0x%08X 0x%08X 0x%08X 0x%08X",
                        px[0], px[1], px[2], px[3]);
            
            // Sample from middle
            int midIdx = (height / 2) * width + (width / 2);
            Logger::Log(LogLevel::Info, "SaveCpuScreenshot: midPixel=0x%08X", px[midIdx]);
        }
        SDL_FreeSurface(surf);
    } else {
        Logger::Log(LogLevel::Error, "SaveCpuScreenshot: Failed to create surface: %s", SDL_GetError());
    }
}

} // namespace renderer
} // namespace fallout