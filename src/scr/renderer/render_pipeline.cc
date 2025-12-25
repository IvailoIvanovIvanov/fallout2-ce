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

RenderPipeline::RenderPipeline() {
}

RenderPipeline::~RenderPipeline() {
    Shutdown();
}

bool RenderPipeline::Init(int inputWidth, int inputHeight, int outputWidth, int outputHeight, SDL_Window* window) {
    if (mInitialized) {
        return false;
    }

    // Initialize Logger
    Logger::Init("renderer.log");
    LogDiagnostic("Initializing RenderPipeline: %dx%d -> %dx%d", inputWidth, inputHeight, outputWidth, outputHeight);

    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;
    mWindow = window;

    mWindowWidth = outputWidth;
    mWindowHeight = outputHeight;

    // Calculate Render Resolution (Aspect Correct)
    float inputAspect = (float)inputWidth / inputHeight;
    float windowAspect = (float)outputWidth / outputHeight;

    if (windowAspect > inputAspect) {
        // Window is wider (Pillarbox)
        mRenderHeight = outputHeight;
        mRenderWidth = (int)(outputHeight * inputAspect);
    } else {
        // Window is taller (Letterbox)
        mRenderWidth = outputWidth;
        mRenderHeight = (int)(outputWidth / inputAspect);
    }
    
    // Ensure even dimensions
    mRenderWidth &= ~1;
    mRenderHeight &= ~1;

    LogDiagnostic("Render Resolution: %dx%d (Window: %dx%d)", mRenderWidth, mRenderHeight, mWindowWidth, mWindowHeight);

    // Initialize Display Scaler
    displayScalerInit(inputWidth, inputHeight);
    displayScalerUpdatePhysicalSize(outputWidth, outputHeight);

    // Create Displays
    mPhantomDisplay = std::make_unique<PhantomDisplay>(inputWidth, inputHeight);
    mRealDisplay = std::make_unique<RealDisplay>(outputWidth, outputHeight);

    // Load Configuration
    LoadConfiguration();
    
    // Force verbose logging for debugging
    mVerboseLogging = true;

    // Instantiate OpenGL Context
    mContext = std::make_unique<OpenGLContext>(window);

    if (!mContext->Init()) {
        LogDiagnostic("Failed to initialize GpuContext");
        return false;
    }

    if (!mBuffers.Init(*mContext, inputWidth, inputHeight, mRenderWidth, mRenderHeight)) {
        LogDiagnostic("Failed to initialize BufferManager");
        return false;
    }

    // Setup Passes based on configuration
    SetupPasses();

    mInitialized = true;
    LogDiagnostic("RenderPipeline initialized successfully");
    return true;
}

void RenderPipeline::SetupPasses() {
    mScalerPass.reset();
    mAnime4KPasses.clear();
    mPrePass.reset();
    mPostPass.reset();

    // Clear scaling buffers
    for (auto& buf : mScalingBuffers) {
        if (buf.handle) mContext->DestroyTexture(buf.handle);
    }
    mScalingBuffers.clear();

    RenderSurface inputSurface = { nullptr, mInputWidth, mInputHeight, TextureFormat::RGBA8 };
    RenderSurface renderSurface = { nullptr, mRenderWidth, mRenderHeight, TextureFormat::RGBA16F };

    // 3. Scaler Pass / Anime4K Pipeline
    if (mConfiguredMode == RenderMode::ANIME4K) {
        LogDiagnostic("[SCALER] Setting up Anime4K Pipeline");
        
        int currentW = mInputWidth;
        int currentH = mInputHeight;
        RenderSurface currentInput = inputSurface;

        // Helper to add pass
        auto AddPass = [&](const std::string& shaderName, int scaleFactor) {
            if (shaderName.empty()) return;

            std::string path = "data/shaders/" + shaderName;
            int nextW = currentW * scaleFactor;
            int nextH = currentH * scaleFactor;
            
            // Create output buffer
            TextureDesc desc = { nextW, nextH, TextureFormat::RGBA16F };
            void* handle = mContext->CreateTexture(desc);
            if (!handle) {
                LogDiagnostic("Failed to create buffer for %s", shaderName.c_str());
                return;
            }
            RenderSurface output = { handle, nextW, nextH, TextureFormat::RGBA16F };
            mScalingBuffers.push_back(output);

            auto pass = std::make_unique<GenericShaderPass>(path);
            if (pass->Init(*mContext, currentInput, output)) {
                mAnime4KPasses.push_back(std::move(pass));
                LogDiagnostic("Added Anime4K Pass: %s (%dx%d -> %dx%d)", shaderName.c_str(), currentW, currentH, nextW, nextH);
                currentW = nextW;
                currentH = nextH;
                currentInput = output;
            } else {
                LogDiagnostic("Failed to init Anime4K Pass: %s", shaderName.c_str());
                // Cleanup buffer?
            }
        };

        // Define Pipeline Steps
        struct PipelineStep {
            bool enabled;
            std::string shader;
            int scale;
        };

        std::vector<PipelineStep> steps = {
            { mEnablePrePass, mPrePassShader, 1 },
            { mEnableClean1, mClean1Shader, 1 },
            { mEnableClean2, mClean2Shader, 1 },
            { mEnableScale1, mScale1Shader, 2 },
            { mEnableOptimize, mOptimizeShader, 1 },
            { mEnableScale2, mScale2Shader, 2 },
            { mEnablePolish, mPolishShader, 1 },
            { mEnablePostPass, mPostPassShader, 1 }
        };

        for (const auto& step : steps) {
            if (step.enabled) {
                AddPass(step.shader, step.scale);
            }
        }

        // Final Scaler to Target
        LogDiagnostic("[SCALER] Final scale from %dx%d to %dx%d", currentW, currentH, mRenderWidth, mRenderHeight);
        auto scalerPass = std::make_unique<ScalerPass>();
        if (scalerPass->Init(*mContext, currentInput, renderSurface)) {
            mScalerPass = std::move(scalerPass);
        } else {
            LogDiagnostic("Failed to init final ScalerPass");
        }

    } else {
        // Mode 0: Simple Scaler Only
        LogDiagnostic("[SCALER] Using Default Scaler");
        auto scalerPass = std::make_unique<ScalerPass>();
        if (scalerPass->Init(*mContext, inputSurface, renderSurface)) {
            mScalerPass = std::move(scalerPass);
        }
    }
}

void RenderPipeline::Shutdown() {
    if (!mContext) return;
    
    for (auto& pass : mAnime4KPasses) pass->Shutdown(*mContext);
    mAnime4KPasses.clear();

    if (mScalerPass) mScalerPass->Shutdown(*mContext);

    for (auto& buf : mScalingBuffers) {
        if (buf.handle) mContext->DestroyTexture(buf.handle);
    }
    mScalingBuffers.clear();

    mBuffers.Shutdown(*mContext);
    mContext->Shutdown();
    mPhantomDisplay.reset();
    mRealDisplay.reset();
    mInitialized = false;
}

bool RenderPipeline::Reconfigure(int outputWidth, int outputHeight) {
    if (mWindowWidth == outputWidth && mWindowHeight == outputHeight) return true;

    LogDiagnostic("Reconfiguring output: %dx%d -> %dx%d", mWindowWidth, mWindowHeight, outputWidth, outputHeight);
    
    // Shutdown and re-init
    // We keep input dimensions and window
    int inputW = mInputWidth;
    int inputH = mInputHeight;
    SDL_Window* win = mWindow;

    Shutdown();
    return Init(inputW, inputH, outputWidth, outputHeight, win);
}

bool RenderPipeline::SetIndexedInput(SDL_Surface* surface) {
    if (!mInitialized || !mPhantomDisplay) {
        LogDiagnostic("SetIndexedInput failed: Not initialized");
        return false;
    }

    if (!surface || !surface->pixels) {
        LogDiagnostic("SetIndexedInput failed: Null surface or pixels");
        return false;
    }

    // Convert RGB palette to RGBA
    // OpenGL expects RGBA, so we need to pack it as 0xAABBGGRR on Little Endian
    uint32_t paletteRGBA[256];
    
    if (surface->format && surface->format->palette) {
        SDL_Color* colors = surface->format->palette->colors;
        for (int i = 0; i < 256; ++i) {
            uint8_t r = colors[i].r;
            uint8_t g = colors[i].g;
            uint8_t b = colors[i].b;
            // Ensure alpha is 0xFF
            paletteRGBA[i] = (0xFF000000) | (b << 16) | (g << 8) | (r);
        }
    } else {
        LogDiagnostic("SetIndexedInput: Surface has no palette!");
        memset(paletteRGBA, 0, sizeof(paletteRGBA));
    }

    mPhantomDisplay->SetData((const unsigned char*)surface->pixels, paletteRGBA);
    return true;
}

bool RenderPipeline::SetRgbaInput(const uint32_t* rgbaBuffer) {
    if (!mInitialized || !mPhantomDisplay) return false;
    mPhantomDisplay->SetData(rgbaBuffer);
    return true;
}

void RenderPipeline::Dispatch() {
    if (!mInitialized || !mPhantomDisplay) return;

    // Check for F8 key press
    const Uint8* state = SDL_GetKeyboardState(NULL);
    if (state[SDL_SCANCODE_F8]) {
        if (!mF8Pressed) {
            mScreenshotManager.RequestCapture();
            mF8Pressed = true;
        }
    } else {
        mF8Pressed = false;
    }

    bool capture = mScreenshotManager.IsCaptureRequested();

    // 1. Swap Buffers (Move to next frame)
    mBuffers.SwapBuffers();

    // 2. Begin Frame
    mContext->BeginFrame();

    // 3. Upload Input
    if (!mBuffers.UploadInput(*mContext, mPhantomDisplay->GetPixels(), mPhantomDisplay->GetWidth() * mPhantomDisplay->GetHeight() * 4)) {
        LogDiagnostic("Failed to upload input");
    }

    // 4. Execute Filter Chain
    RenderSurface currentInput = mBuffers.GetInputSurface();
    
    if (capture) {
        mScreenshotManager.Capture(*mContext, currentInput, "00_Input");
    }

    int passIndex = 1;

    // 5. Execute Scaler Pass / Anime4K Chain
    RenderSurface scalerOutput = mBuffers.GetOutputSurface();
    
    if (!mAnime4KPasses.empty()) {
        // Execute Anime4K Chain
        RenderSurface chainInput = currentInput;
        for (size_t i = 0; i < mAnime4KPasses.size(); ++i) {
            RenderSurface chainOutput = mScalingBuffers[i];
            mAnime4KPasses[i]->Execute(*mContext, chainInput, chainOutput);
            
            if (capture) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%02d_Anime4K_%d_%s", passIndex++, (int)i, mAnime4KPasses[i]->GetName().c_str());
                mScreenshotManager.Capture(*mContext, chainOutput, buf);
            }
            
            chainInput = chainOutput;
        }
        currentInput = chainInput;
    }

    if (mScalerPass) {
        mScalerPass->Execute(*mContext, currentInput, scalerOutput);
        
        if (capture) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%02d_%s", passIndex++, mScalerPass->GetName().c_str());
            mScreenshotManager.Capture(*mContext, scalerOutput, buf);
        }
    } else {
        LogDiagnostic("Dispatch: No Scaler Pass!");
        // If no scaler pass, we must copy input to output manually or handle it
        // For now, just copy if possible, but sizes differ so we need a scaler.
        // mScalerPass should always be present.
    }

    if (capture) {
        mScreenshotManager.EndCapture();
    }

    // 7. End Frame
    mContext->EndFrame();

    // 8. Present to Screen
    // Use the final output buffer
    mContext->Present(mBuffers.GetOutputSurface().handle, mRenderWidth, mRenderHeight, mWindowWidth, mWindowHeight);
}

const void* RenderPipeline::GetOutput() {
    if (!mInitialized) return nullptr;
    return mBuffers.ReadbackOutput(*mContext);
}

void RenderPipeline::LoadConfiguration() {
    auto& config = RendererConfig::GetInstance();
    
    // Try to load from current directory, then from executable directory if needed
    if (!config.Load("renderer_config.ini")) {
        LogDiagnostic("Failed to load renderer_config.ini from current directory");
        // Fallback logic could go here
    } else {
        LogDiagnostic("Loaded renderer_config.ini");
    }

    int mode = config.GetInt("General", "Mode", 0);
    mConfiguredMode = (mode == 1) ? RenderMode::ANIME4K : RenderMode::SIMPLE;

    mVerboseLogging = config.GetBool("General", "VerboseLogging", false);

    mSharpness = config.GetFloat("Scaler", "Sharpness", 0.5f);

    // Anime4K Pipeline
    mEnablePrePass = config.GetBool("Anime4K", "EnablePrePass", false);
    mPrePassShader = config.GetString("Anime4K", "PrePassShader", "");

    mEnableClean1 = config.GetBool("Anime4K", "EnableClean1", true);
    mClean1Shader = config.GetString("Anime4K", "Clean1Shader", "Anime4K_Clamp_Highlights.glsl");

    mEnableClean2 = config.GetBool("Anime4K", "EnableClean2", true);
    mClean2Shader = config.GetString("Anime4K", "Clean2Shader", "Anime4K_Restore_CNN_M.glsl");

    mEnableScale1 = config.GetBool("Anime4K", "EnableScale1", true);
    mScale1Shader = config.GetString("Anime4K", "Scale1Shader", "Anime4K_Upscale_CNN_x2_L.glsl");

    mEnableOptimize = config.GetBool("Anime4K", "EnableOptimize", true);
    mOptimizeShader = config.GetString("Anime4K", "OptimizeShader", "Anime4K_AutoDownscalePre_x4.glsl");

    mEnableScale2 = config.GetBool("Anime4K", "EnableScale2", true);
    mScale2Shader = config.GetString("Anime4K", "Scale2Shader", "Anime4K_Upscale_CNN_x2_M.glsl");

    mEnablePolish = config.GetBool("Anime4K", "EnablePolish", true);
    mPolishShader = config.GetString("Anime4K", "PolishShader", "Anime4K_Thin_HQ.glsl");

    mEnablePostPass = config.GetBool("Anime4K", "EnablePostPass", false);
    mPostPassShader = config.GetString("Anime4K", "PostPassShader", "");

    LogDiagnostic("Configuration Loaded: Mode=%d (Configured=%d)", mode, (int)mConfiguredMode);
}

void RenderPipeline::LogDiagnostic(const char* format, ...) {
    // Check if it's an error message
    bool isError = (strstr(format, "Failed") != nullptr || strstr(format, "Error") != nullptr);
    
    // Always log errors, otherwise check verbose flag
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