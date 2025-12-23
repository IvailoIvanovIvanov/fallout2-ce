#include "render_pipeline.h"
#include "opengl_context.h"
#include "display_scaler.h"
#include "logger.h"
#include "renderer_config.h"

#include "anime4k_pass.h"
#include "blur_filter.h"
#include "hdr_filter.h"
#include "generic_shader_pass.h"

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

    // Initialize Display Scaler
    displayScalerInit(inputWidth, inputHeight);
    displayScalerUpdatePhysicalSize(outputWidth, outputHeight);

    // Create Displays
    mPhantomDisplay = std::make_unique<PhantomDisplay>(inputWidth, inputHeight);
    mRealDisplay = std::make_unique<RealDisplay>(outputWidth, outputHeight);

    // Load Configuration
    LoadConfiguration();

    // Instantiate OpenGL Context
    mContext = std::make_unique<OpenGLContext>(window);

    if (!mContext->Init()) {
        LogDiagnostic("Failed to initialize GpuContext");
        return false;
    }

    if (!mBuffers.Init(*mContext, inputWidth, inputHeight, mOutputWidth, mOutputHeight)) {
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

    if (mConfiguredMode == RenderMode::ANIME4K) {
        // 1. Add Blur Pass if needed (Pre-processing)
        if (mEnableEdgeSmoothing) {
            LogDiagnostic("[PASS 1] Adding BlurFilter (strength=%.2f)", mSmoothingStrength);
            auto blurPass = std::make_unique<BlurFilter>();
            blurPass->SetStrength(mSmoothingStrength);
            if (blurPass->Init(*mContext, mInputWidth, mInputHeight, mInputWidth, mInputHeight)) {
                mPasses.push_back(std::move(blurPass));
            }
        }

        // 2. Add HDR Pass if needed (Color enhancement)
        if (mEnableSoftHDR) {
            LogDiagnostic("[PASS 2] Adding HdrFilter (sat=%.2f, contrast=%.2f)", mHdrSaturation, mHdrContrast);
            auto hdrPass = std::make_unique<HDRFilter>();
            hdrPass->SetParams(mHdrSaturation, mHdrContrast, mBlackCrushThreshold, mBlackCrushStrength);
            if (hdrPass->Init(*mContext, mInputWidth, mInputHeight, mInputWidth, mInputHeight)) {
                mPasses.push_back(std::move(hdrPass));
            }
        }

        // 3. Add Anime4K Scaler
        int anime4kVersion = RendererConfig::GetInstance().GetInt("Scaler", "Anime4KVersion", 0);
        LogDiagnostic("[SCALER] Using Anime4K Scaler (strength=%.2f, version=%d)", mSharpness, anime4kVersion);
        auto anime4k = std::make_unique<Anime4kPass>();
        anime4k->SetStrength(mSharpness);
        anime4k->SetVersion(static_cast<Anime4kPass::Version>(anime4kVersion));
        mScalerPass = std::move(anime4k);

        // 4. Add Post-Processing Passes
        if (mEnablePostBlur) {
            LogDiagnostic("[POST] Adding Blur Pass");
            auto pass = std::make_unique<GenericShaderPass>("data/shaders/pp_blur_h.glsl");
            if (pass->Init(*mContext, mOutputWidth, mOutputHeight, mOutputWidth, mOutputHeight)) mPostPasses.push_back(std::move(pass));
            
            pass = std::make_unique<GenericShaderPass>("data/shaders/pp_blur_v.glsl");
            if (pass->Init(*mContext, mOutputWidth, mOutputHeight, mOutputWidth, mOutputHeight)) mPostPasses.push_back(std::move(pass));
        }
        if (mEnablePostBloom) {
            LogDiagnostic("[POST] Adding Tone Map Pass");
            auto pass = std::make_unique<GenericShaderPass>("data/shaders/pp_tonemap.glsl");
            if (pass->Init(*mContext, mOutputWidth, mOutputHeight, mOutputWidth, mOutputHeight)) mPostPasses.push_back(std::move(pass));
        }
        if (mEnablePostSharpen) {
            LogDiagnostic("[POST] Adding Sharpen Pass");
            auto pass = std::make_unique<GenericShaderPass>("data/shaders/pp_sharpen.glsl");
            if (pass->Init(*mContext, mOutputWidth, mOutputHeight, mOutputWidth, mOutputHeight)) mPostPasses.push_back(std::move(pass));
        }
        if (mEnablePostDenoise) {
            LogDiagnostic("[POST] Adding Denoise Pass");
            auto pass = std::make_unique<GenericShaderPass>("data/shaders/pp_denoise.glsl");
            if (pass->Init(*mContext, mOutputWidth, mOutputHeight, mOutputWidth, mOutputHeight)) mPostPasses.push_back(std::move(pass));
        }

    } else {
        // Mode 0: Simple Scaler Only
        LogDiagnostic("[SCALER] Using Default Scaler");
        auto scalerPass = std::make_unique<ScalerPass>();
        mScalerPass = std::move(scalerPass);
    }

    if (mScalerPass) {
        if (!mScalerPass->Init(*mContext, mInputWidth, mInputHeight, mOutputWidth, mOutputHeight)) {
            LogDiagnostic("Failed to initialize ScalerPass");
        }
    }
}

bool RenderPipeline::CreateIntermediateBuffers() {
    TextureDesc desc = { mInputWidth, mInputHeight, TextureFormat::RGBA8 };

    for (int i = 0; i < 2; ++i) {
        mIntermediateBuffers[i] = mContext->CreateTexture(desc);
        if (!mIntermediateBuffers[i]) return false;
    }

    TextureDesc postDesc = { mOutputWidth, mOutputHeight, TextureFormat::RGBA8 };
    for (int i = 0; i < 2; ++i) {
        mPostIntermediateBuffers[i] = mContext->CreateTexture(postDesc);
        if (!mPostIntermediateBuffers[i]) return false;
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
        if (mIntermediateBuffers[i]) {
            mContext->DestroyTexture(mIntermediateBuffers[i]);
            mIntermediateBuffers[i] = nullptr;
        }
        if (mPostIntermediateBuffers[i]) {
            mContext->DestroyTexture(mPostIntermediateBuffers[i]);
            mPostIntermediateBuffers[i] = nullptr;
        }
    }

    mBuffers.Shutdown(*mContext);
    mContext->Shutdown();
    mPhantomDisplay.reset();
    mRealDisplay.reset();
    mInitialized = false;
}

bool RenderPipeline::Reconfigure(int outputWidth, int outputHeight) {
    if (mOutputWidth == outputWidth && mOutputHeight == outputHeight) return true;

    LogDiagnostic("Reconfiguring output: %dx%d -> %dx%d", mOutputWidth, mOutputHeight, outputWidth, outputHeight);
    
    // Shutdown and re-init
    // We keep input dimensions and window
    int inputW = mInputWidth;
    int inputH = mInputHeight;
    SDL_Window* win = mWindow;

    Shutdown();
    return Init(inputW, inputH, outputWidth, outputHeight, win);
}

bool RenderPipeline::SetIndexedInput(const unsigned char* indexedBuffer, const unsigned char* palette) {
    if (!mInitialized || !mPhantomDisplay) {
        LogDiagnostic("SetIndexedInput failed: Not initialized");
        return false;
    }

    if (!indexedBuffer || !palette) {
        LogDiagnostic("SetIndexedInput failed: Null buffer or palette");
        return false;
    }

    // Convert RGB palette to RGBA
    uint32_t paletteRGBA[256];
    for (int i = 0; i < 256; ++i) {
        paletteRGBA[i] = (0xFF000000) | (palette[i*3] << 16) | (palette[i*3+1] << 8) | (palette[i*3+2]);
    }

    // Log first few palette entries for debugging
    static int logCounter = 0;
    if (logCounter++ < 5) {
        LogDiagnostic("Palette[0]: %08X, Palette[1]: %08X", paletteRGBA[0], paletteRGBA[1]);
    }

    mPhantomDisplay->SetData(indexedBuffer, paletteRGBA);
    return true;
}

bool RenderPipeline::SetRgbaInput(const uint32_t* rgbaBuffer) {
    if (!mInitialized || !mPhantomDisplay) return false;
    mPhantomDisplay->SetData(rgbaBuffer);
    return true;
}

void RenderPipeline::Dispatch() {
    if (!mInitialized || !mPhantomDisplay) return;

    // 1. Swap Buffers (Move to next frame)
    mBuffers.SwapBuffers();

    // 2. Begin Frame
    mContext->BeginFrame();

    // 3. Upload Input
    if (!mBuffers.UploadInput(*mContext, mPhantomDisplay->GetPixels(), mPhantomDisplay->GetWidth() * mPhantomDisplay->GetHeight() * 4)) {
        LogDiagnostic("Failed to upload input");
    }

    // 4. Execute Filter Chain
    void* currentInput = mBuffers.GetInputBuffer();
    int targetIndex = 0;

    for (auto& pass : mPasses) {
        void* currentOutput = mIntermediateBuffers[targetIndex];
        pass->Execute(*mContext, currentInput, currentOutput);
        currentInput = currentOutput;
        targetIndex = 1 - targetIndex;
    }

    // 5. Execute Scaler Pass
    void* scalerOutput = (mPostPasses.empty()) ? mBuffers.GetOutputBuffer() : mPostIntermediateBuffers[0];
    if (mScalerPass) {
        mScalerPass->Execute(*mContext, currentInput, scalerOutput);
    } else {
        LogDiagnostic("Dispatch: No Scaler Pass!");
    }

    // 6. Execute Post Passes
    if (!mPostPasses.empty()) {
        currentInput = scalerOutput;
        targetIndex = 1;

        for (size_t i = 0; i < mPostPasses.size(); ++i) {
            auto& pass = mPostPasses[i];
            bool isLast = (i == mPostPasses.size() - 1);
            void* currentOutput = isLast ? mBuffers.GetOutputBuffer() : mPostIntermediateBuffers[targetIndex];

            pass->Execute(*mContext, currentInput, currentOutput);

            currentInput = currentOutput;
            targetIndex = 1 - targetIndex;
        }
    }

    // 7. End Frame
    mContext->EndFrame();
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
    mEnablePostBlur = config.GetBool("PostProcessing", "Blur", false);
    mEnablePostBloom = config.GetBool("PostProcessing", "Bloom", false);
    mEnablePostSharpen = config.GetBool("PostProcessing", "Sharpen", false);
    mEnablePostDenoise = config.GetBool("PostProcessing", "Denoise", false);

    mSharpness = config.GetFloat("Scaler", "Sharpness", 0.5f);

    LogDiagnostic("Configuration Loaded: Mode=%d", (int)mConfiguredMode);
}

void RenderPipeline::LogDiagnostic(const char* format, ...) {
    if (!mVerboseLogging) return;
    
    va_list args;
    va_start(args, format);
    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    Logger::Log(LogLevel::Info, "%s", buffer);
}

} // namespace renderer
} // namespace fallout