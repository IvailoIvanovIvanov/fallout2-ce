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

    if (!CreateIntermediateBuffers()) {
        LogDiagnostic("Failed to create intermediate buffers");
        return false;
    }

    // Setup Passes based on configuration
    SetupPasses();

    mInitialized = true;
    LogDiagnostic("RenderPipeline initialized successfully");
    return true;
}

void RenderPipeline::SetupPasses() {
    mPasses.clear();
    mPostPasses.clear();
    mScalerPass.reset();

    RenderSurface inputSurface = { nullptr, mInputWidth, mInputHeight };
    RenderSurface renderSurface = { nullptr, mRenderWidth, mRenderHeight };

    if (mConfiguredMode == RenderMode::ANIME4K) {
        // 1. Add Blur Pass (Pre-processing) - 2 Passes (Horizontal + Vertical)
        if (mEnableEdgeSmoothing) {
            LogDiagnostic("[PASS 1] Adding Blur Pass (H+V)");
            
            auto blurH = std::make_unique<GenericShaderPass>("data/shaders/pp_blur_h.glsl");
            if (blurH->Init(*mContext, inputSurface, inputSurface)) {
                mPasses.push_back(std::move(blurH));
            }

            auto blurV = std::make_unique<GenericShaderPass>("data/shaders/pp_blur_v.glsl");
            if (blurV->Init(*mContext, inputSurface, inputSurface)) {
                mPasses.push_back(std::move(blurV));
            }
        }

        // 2. Add HDR/Tonemap Pass
        if (mEnableSoftHDR) {
            LogDiagnostic("[PASS 2] Adding HDR/Tonemap Pass");
            auto hdrPass = std::make_unique<GenericShaderPass>("data/shaders/pp_tonemap.glsl");
            if (hdrPass->Init(*mContext, inputSurface, inputSurface)) {
                mPasses.push_back(std::move(hdrPass));
            }
        }

        // 3. Add Anime4K Scaler
        std::string anime4kShader = RendererConfig::GetInstance().GetString("Scaler", "Anime4KVersion", "Anime4K_Upscale_GAN_x4_UUL.glsl");
        LogDiagnostic("[SCALER] Using Anime4K Scaler (shader=%s)", anime4kShader.c_str());
        
        // If user provides just a name, assume it's in data/shaders/
        std::string shaderPath = anime4kShader;
        if (shaderPath.find("/") == std::string::npos && shaderPath.find("\\") == std::string::npos) {
            shaderPath = "data/shaders/" + shaderPath;
        }

        auto anime4k = std::make_unique<GenericShaderPass>(shaderPath);
        mScalerPass = std::move(anime4k);

        // 4. Add Post-Processing Passes
        if (mEnablePostSharpen) {
            LogDiagnostic("[POST] Adding Sharpen Pass");
            auto pass = std::make_unique<GenericShaderPass>("data/shaders/pp_sharpen.glsl");
            if (pass->Init(*mContext, renderSurface, renderSurface)) mPostPasses.push_back(std::move(pass));
        }
        if (mEnablePostDenoise) {
            LogDiagnostic("[POST] Adding Denoise Pass");
            auto pass = std::make_unique<GenericShaderPass>("data/shaders/pp_denoise.glsl");
            if (pass->Init(*mContext, renderSurface, renderSurface)) mPostPasses.push_back(std::move(pass));
        }

    } else {
        // Mode 0: Simple Scaler Only
        LogDiagnostic("[SCALER] Using Default Scaler");
        auto scalerPass = std::make_unique<ScalerPass>();
        mScalerPass = std::move(scalerPass);
    }

    if (mScalerPass) {
        if (!mScalerPass->Init(*mContext, inputSurface, renderSurface)) {
            LogDiagnostic("Failed to initialize ScalerPass");
        }
    }
}

bool RenderPipeline::CreateIntermediateBuffers() {
    TextureDesc desc = { mInputWidth, mInputHeight, TextureFormat::RGBA8 };

    for (int i = 0; i < 2; ++i) {
        mIntermediateBuffers[i].handle = mContext->CreateTexture(desc);
        mIntermediateBuffers[i].width = mInputWidth;
        mIntermediateBuffers[i].height = mInputHeight;
        if (!mIntermediateBuffers[i].handle) return false;
    }

    TextureDesc postDesc = { mRenderWidth, mRenderHeight, TextureFormat::RGBA8 };
    for (int i = 0; i < 2; ++i) {
        mPostIntermediateBuffers[i].handle = mContext->CreateTexture(postDesc);
        mPostIntermediateBuffers[i].width = mRenderWidth;
        mPostIntermediateBuffers[i].height = mRenderHeight;
        if (!mPostIntermediateBuffers[i].handle) return false;
    }

    return true;
}

void RenderPipeline::Shutdown() {
    if (!mContext) return;

    for (auto& pass : mPasses) pass->Shutdown(*mContext);
    mPasses.clear();

    for (auto& pass : mPostPasses) pass->Shutdown(*mContext);
    mPostPasses.clear();
    
    if (mScalerPass) mScalerPass->Shutdown(*mContext);

    for (int i = 0; i < 2; ++i) {
        if (mIntermediateBuffers[i].handle) {
            mContext->DestroyTexture(mIntermediateBuffers[i].handle);
            mIntermediateBuffers[i].handle = nullptr;
        }
        if (mPostIntermediateBuffers[i].handle) {
            mContext->DestroyTexture(mPostIntermediateBuffers[i].handle);
            mPostIntermediateBuffers[i].handle = nullptr;
        }
    }

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

    int targetIndex = 0;
    int passIndex = 1;

    for (auto& pass : mPasses) {
        RenderSurface currentOutput = mIntermediateBuffers[targetIndex];
        pass->Execute(*mContext, currentInput, currentOutput);
        
        if (capture) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%02d_%s", passIndex++, pass->GetName().c_str());
            mScreenshotManager.Capture(*mContext, currentOutput, buf);
        }

        currentInput = currentOutput;
        targetIndex = 1 - targetIndex;
    }

    // 5. Execute Scaler Pass
    RenderSurface scalerOutput = (mPostPasses.empty()) ? mBuffers.GetOutputSurface() : mPostIntermediateBuffers[0];
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

    // 6. Execute Post Passes
    if (!mPostPasses.empty()) {
        currentInput = scalerOutput;
        targetIndex = 1;

        for (size_t i = 0; i < mPostPasses.size(); ++i) {
            auto& pass = mPostPasses[i];
            bool isLast = (i == mPostPasses.size() - 1);
            RenderSurface currentOutput = isLast ? mBuffers.GetOutputSurface() : mPostIntermediateBuffers[targetIndex];

            pass->Execute(*mContext, currentInput, currentOutput);

            if (capture) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%02d_%s", passIndex++, pass->GetName().c_str());
                mScreenshotManager.Capture(*mContext, currentOutput, buf);
            }

            currentInput = currentOutput;
            targetIndex = 1 - targetIndex;
        }
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
    config.Load("renderer_config.ini");

    int mode = config.GetInt("General", "Mode", 0);
    mConfiguredMode = (mode == 1) ? RenderMode::ANIME4K : RenderMode::SIMPLE;

    mVerboseLogging = config.GetBool("General", "VerboseLogging", false);

    // Filters
    mEnableEdgeSmoothing = config.GetBool("Filters", "EdgeSmoothing", true);
    mSmoothingStrength = config.GetFloat("Filters", "SmoothingStrength", 0.6f);
    
    mEnableSoftHDR = config.GetBool("Filters", "HDR", true);
    mHdrSaturation = config.GetFloat("Filters", "HDRSaturation", 1.2f);
    mHdrContrast = config.GetFloat("Filters", "HDRContrast", 1.1f);
    mBlackCrushThreshold = config.GetFloat("Filters", "BlackCrushThreshold", 0.03f);
    mBlackCrushStrength = config.GetFloat("Filters", "BlackCrushStrength", 1.0f);

    // Post Processing
    mEnablePostSharpen = config.GetBool("PostProcessing", "Sharpen", false);
    mEnablePostDenoise = config.GetBool("PostProcessing", "Denoise", false);

    mSharpness = config.GetFloat("Scaler", "Sharpness", 0.5f);

    LogDiagnostic("Configuration Loaded: Mode=%d", (int)mConfiguredMode);
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