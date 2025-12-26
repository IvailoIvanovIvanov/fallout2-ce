#include "render_pipeline.h"
#include "opengl_context.h"
#include "display_scaler.h"
#include "logger.h"
#include "renderer_config.h"
#include "generic_shader_pass.h"

#include <SDL.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>

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
    mContext = std::make_unique<OpenGLContext>(mWindow);
    if (!mContext->Init()) {
        LogDiagnostic("Failed to initialize GpuContext");
        return false;
    }
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
    
    mPhantomDisplay->SetData(static_cast<const unsigned char*>(surface->pixels), paletteRGBA);
    return true;
}

void RenderPipeline::ConvertPaletteToRGBA(SDL_Surface* surface, uint32_t* paletteRGBA) {
    if (surface->format && surface->format->palette) {
        SDL_Color* colors = surface->format->palette->colors;
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

    // Execute filter chain
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

} // namespace renderer
} // namespace fallout