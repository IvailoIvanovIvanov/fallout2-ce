#include "upscaler.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>
#include <chrono>

#include <SDL.h>

#include "render_pipeline.h"
#include "ml_upscale_pass.h"
#include "blur_filter.h"
#include "hdr_filter.h"
#include "phantom_display.h"
#include "real_display.h"

#include "../diagnostics.h"
#include "../game_config.h"
#include "../memory.h"

namespace fallout {

constexpr int ERROR_MSG_SIZE = 256;
constexpr int MAX_RESOLUTION = 8192;

class UpscalerImpl {
public:
    static UpscalerImpl* getInstance() {
        static UpscalerImpl instance;
        return &instance;
    }

    UpscalerImpl(const UpscalerImpl&) = delete;
    UpscalerImpl& operator=(const UpscalerImpl&) = delete;

    UpscalerState getState() const { return mState; }

    bool init(int inputWidth, int inputHeight, int outputWidth, int outputHeight, UpscalerMode mode);
    bool reconfigureOutput(int outputWidth, int outputHeight);
    void shutdown();

    bool setIndexedInput(const unsigned char* indexedBuffer, const uint32_t* palette);
    bool setRgbaInput(const uint32_t* rgbaBuffer);
    bool dispatch();

    const uint32_t* getOutputBuffer() const {
        if (mPipeline) {
            return static_cast<const uint32_t*>(mPipeline->GetOutput());
        }
        return nullptr;
    }

    int getOutputPitch() const { return mRealDisplay ? mRealDisplay->GetWidth() * 4 : 0; }
    void getOutputDimensions(int& width, int& height) const {
        if (mRealDisplay) {
            width = mRealDisplay->GetWidth();
            height = mRealDisplay->GetHeight();
        } else {
            width = 0;
            height = 0;
        }
    }

    bool setQuality(UpscalerQuality quality) { mQuality = quality; return true; }
    bool setSharpness(float sharpness) { mSharpness = sharpness; return true; }
    
    // Deprecated stubs
    void setMotionVectorsEnabled(bool) {}
    void setFilterParams(float, float, float, float) {}
    
    void setVerboseLogging(bool enabled);
    void reloadConfig();

    bool isAvailable() const { return mIsAvailable; }
    const char* getLastError() const { return mLastError; }
    UpscalerMode getConfiguredMode() const { return mConfiguredMode; }

private:
    UpscalerImpl();

    void logDiagnostic(const char* format, ...);
    void setError(const char* format, ...);
    
    void loadConfiguration();
    void loadUpscalerConfig();
    void loadFilterConfig();
    void saveConfiguration();

    bool allocateInputBuffer(int width, int height);
    void deallocateBuffers();

    UpscalerState mState = UpscalerState::STATE_UNINITIALIZED;
    UpscalerMode mMode = UpscalerMode::NONE;
    UpscalerMode mConfiguredMode = UpscalerMode::NONE;
    UpscalerQuality mQuality = UpscalerQuality::BALANCED;
    bool mIsAvailable = false;

    // New Renderer Pipeline
    std::unique_ptr<renderer::RenderPipeline> mPipeline;

    // Phantom Display
    std::unique_ptr<renderer::PhantomDisplay> mPhantomDisplay;
    // Real Display
    std::unique_ptr<renderer::RealDisplay> mRealDisplay;

    // Config
    float mSharpness = 0.5f;
    bool mEnableDebanding = true;
    float mDebandingStrength = 0.5f;
    bool mEnableEdgeSmoothing = true;
    float mSmoothingStrength = 0.6f;
    bool mEnableKuwahara = false;
    int mKuwaharaRadius = 2;
    bool mEnableSoftHDR = true;  // ENABLED BY DEFAULT
    float mHdrStrength = 0.5f;
    float mHdrSaturation = 1.5f;  // Increased saturation
    float mHdrContrast = 1.3f;    // Increased contrast
    float mBlackCrushThreshold = 0.05f;  // Higher threshold
    float mBlackCrushStrength = 1.5f;    // Stronger crush
    
    bool mVerboseLogging = false;
    char mLastError[ERROR_MSG_SIZE] = {};
    FILE* mUpscaleLog = nullptr;
    std::string mLogFilePath;
    
    // ML Config
    std::string mMlModelFile;
    int mMlFrameSkip = 3;
};

UpscalerImpl* upscalerGetImpl() {
    return UpscalerImpl::getInstance();
}

UpscalerImpl::UpscalerImpl() {
    char* basePath = SDL_GetBasePath();
    if (basePath != nullptr) {
        mLogFilePath = std::string(basePath) + "upscale.log";
        SDL_free(basePath);
    } else {
        mLogFilePath = "upscale.log";
    }
    
    mUpscaleLog = fopen(mLogFilePath.c_str(), "w");
    if (mUpscaleLog != nullptr) {
        fprintf(mUpscaleLog, "====================================================\n");
        fprintf(mUpscaleLog, "   FALLOUT 2 CE - UPSCALER DIAGNOSTICS LOG (NEW PIPELINE)\n");
        fprintf(mUpscaleLog, "====================================================\n");
        fflush(mUpscaleLog);
    }
}

void UpscalerImpl::logDiagnostic(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    diagnosticsLog(DiagnosticsLevel::Info, "UPSCALER", "%s", buffer);
    
    if (mUpscaleLog) {
        time_t now = time(nullptr);
        struct tm timeinfo;
#if _WIN32
        localtime_s(&timeinfo, &now);
#else
        localtime_r(&now, &timeinfo);
#endif
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeinfo);
        
        fprintf(mUpscaleLog, "[%s] [UPSCALER] %s\n", timestamp, buffer);
        fflush(mUpscaleLog);
    }
}

void UpscalerImpl::setError(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(mLastError, ERROR_MSG_SIZE, format, args);
    va_end(args);
    diagnosticsLog(static_cast<DiagnosticsLevel>(1), "UPSCALER", "%s", mLastError);
}

void UpscalerImpl::loadConfiguration() {
    if (!gGameConfigInitialized) return;
    loadUpscalerConfig();
    loadFilterConfig();
}

void UpscalerImpl::loadUpscalerConfig() {
    int modeValue = 0;
    if (configGetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_MODE_KEY, &modeValue)) {
        if (modeValue < 0 || modeValue > 5) modeValue = 0;
        mConfiguredMode = static_cast<UpscalerMode>(modeValue);
    } else {
        mConfiguredMode = UpscalerMode::REAL_ESRGAN;  // Default to ML upscaler
    }
    
    int qualityValue = 1;
    if (configGetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_QUALITY_KEY, &qualityValue)) {
        mQuality = static_cast<UpscalerQuality>(qualityValue);
    }
    
    double sharpnessValue = 0.5;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_SHARPNESS_KEY, &sharpnessValue)) {
        mSharpness = static_cast<float>(sharpnessValue);
    }
    
    bool verboseValue = false;
    if (configGetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_VERBOSE_LOG_KEY, &verboseValue)) {
        mVerboseLogging = verboseValue;
    }
    
    int frameSkip = 3;
    if (configGetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_ml_frame_skip", &frameSkip)) {
        mMlFrameSkip = std::clamp(frameSkip, 1, 10);
    }

    char* modelFile = nullptr;
    if (configGetString(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_model_file", &modelFile)) {
        mMlModelFile = std::string(modelFile);
    } else {
        mMlModelFile = "RealESRGAN_x4plus_anime_6B.onnx";
    }
}

void UpscalerImpl::loadFilterConfig() {
    configGetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_DEBANDING_KEY, &mEnableDebanding);
    double debandingStr = 0.5;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_DEBANDING_STRENGTH_KEY, &debandingStr)) {
        mDebandingStrength = static_cast<float>(debandingStr);
    }
    
    configGetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_EDGE_SMOOTHING_KEY, &mEnableEdgeSmoothing);
    double smoothingStr = 0.6;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_SMOOTHING_STRENGTH_KEY, &smoothingStr)) {
        mSmoothingStrength = static_cast<float>(smoothingStr);
    }
    
    configGetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_kuwahara_enable", &mEnableKuwahara);
    int radius = 2;
    if (configGetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_kuwahara_radius", &radius)) {
        mKuwaharaRadius = std::clamp(radius, 1, 5);
    }
    
    configGetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_hdr_enable", &mEnableSoftHDR);
    double hdrStr = 0.5;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_hdr_strength", &hdrStr)) {
        mHdrStrength = static_cast<float>(std::clamp(hdrStr, 0.0, 1.0));
    }
    double hdrSat = 1.2;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_hdr_saturation", &hdrSat)) {
        mHdrSaturation = static_cast<float>(std::clamp(hdrSat, 0.0, 2.0));
    }
    double hdrCon = 1.1;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_hdr_contrast", &hdrCon)) {
        mHdrContrast = static_cast<float>(std::clamp(hdrCon, 0.0, 2.0));
    }
    double blackThresh = 0.03;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_black_crush_threshold", &blackThresh)) {
        mBlackCrushThreshold = static_cast<float>(std::clamp(blackThresh, 0.0, 0.15));
    }
    double blackStr = 1.0;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_black_crush_strength", &blackStr)) {
        mBlackCrushStrength = static_cast<float>(std::clamp(blackStr, 0.0, 2.0));
    }
}

void UpscalerImpl::saveConfiguration() {
    if (!gGameConfigInitialized) return;
    
    configSetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_MODE_KEY, static_cast<int>(mMode));
    configSetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_QUALITY_KEY, static_cast<int>(mQuality));
    configSetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_SHARPNESS_KEY, mSharpness);
    configSetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_DEBANDING_KEY, mEnableDebanding);
    configSetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_DEBANDING_STRENGTH_KEY, mDebandingStrength);
    configSetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_EDGE_SMOOTHING_KEY, mEnableEdgeSmoothing);
    configSetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_SMOOTHING_STRENGTH_KEY, mSmoothingStrength);
    configSetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_VERBOSE_LOG_KEY, mVerboseLogging);
    
    gameConfigSave();
}

void UpscalerImpl::setVerboseLogging(bool enabled) {
    mVerboseLogging = enabled;
    saveConfiguration();
}

void UpscalerImpl::reloadConfig() {
    loadConfiguration();
}

bool UpscalerImpl::allocateInputBuffer(int width, int height) {
    // Deprecated: Handled by PhantomDisplay
    return true;
}

void UpscalerImpl::deallocateBuffers() {
    // Deprecated: Handled by PhantomDisplay
}

bool UpscalerImpl::init(int inputWidth, int inputHeight, int outputWidth, int outputHeight, UpscalerMode mode) {
    logDiagnostic("=== INIT: %dx%d -> %dx%d, requested_mode=%d ===", 
                  inputWidth, inputHeight, outputWidth, outputHeight, static_cast<int>(mode));
    
    loadConfiguration();
    mode = mConfiguredMode; // Override with config
    
    if (mState != UpscalerState::STATE_UNINITIALIZED) {
        setError("Upscaler already initialized");
        return false;
    }

    mState = UpscalerState::STATE_INITIALIZING;
    mMode = mode;
    
    mPhantomDisplay = std::make_unique<renderer::PhantomDisplay>(inputWidth, inputHeight);
    mRealDisplay = std::make_unique<renderer::RealDisplay>(outputWidth, outputHeight);

    if (mode == UpscalerMode::NONE) {
        mState = UpscalerState::STATE_READY;
        mIsAvailable = false;
        return true;
    }

    // Initialize RenderPipeline
    mPipeline = std::make_unique<renderer::RenderPipeline>();
    if (!mPipeline->Init(inputWidth, inputHeight, *mRealDisplay)) {
        setError("Failed to initialize RenderPipeline");
        mState = UpscalerState::STATE_ERROR;
        return false;
    }

    logDiagnostic("RenderPipeline initialized: %dx%d -> %dx%d", inputWidth, inputHeight, outputWidth, outputHeight);

    // Add Passes in order: Blur (pre-process) -> HDR (color enhance) -> ML Upscale
    
    // 1. Add Blur Pass if needed (Pre-processing)
    if (mEnableEdgeSmoothing) {
        logDiagnostic("[PASS 1] Adding BlurFilter (strength=%.2f)", mSmoothingStrength);
        auto blurPass = std::make_unique<renderer::BlurFilter>();
        blurPass->SetStrength(mSmoothingStrength);
        mPipeline->AddPass(std::move(blurPass));
    }

    // 2. Add HDR Pass if needed (Color enhancement)
    logDiagnostic("HDR Filter Check: mEnableSoftHDR=%d, sat=%.2f, contrast=%.2f, blackThresh=%.3f, blackStr=%.2f",
                  mEnableSoftHDR, mHdrSaturation, mHdrContrast, mBlackCrushThreshold, mBlackCrushStrength);
    if (mEnableSoftHDR) {
        logDiagnostic("[PASS 2] Adding HdrFilter (sat=%.2f, contrast=%.2f, blackThresh=%.3f, blackStr=%.2f)",
                      mHdrSaturation, mHdrContrast, mBlackCrushThreshold, mBlackCrushStrength);
        auto hdrPass = std::make_unique<renderer::HdrFilter>();
        hdrPass->SetParams(mHdrSaturation, mHdrContrast, mBlackCrushThreshold, mBlackCrushStrength);
        mPipeline->AddPass(std::move(hdrPass));
    } else {
        logDiagnostic("HDR Filter DISABLED (config: upscaler_hdr_enable=0)");
    }

    // 3. Add ML Upscaler if mode is enabled (Final upscaling)
    if (mode == UpscalerMode::REAL_ESRGAN) {
        logDiagnostic("[PASS 3] Adding MlUpscalePass (model=%s)", mMlModelFile.c_str());
        auto mlPass = std::make_unique<renderer::MlUpscalePass>();
        mlPass->SetModelFile(mMlModelFile);
        mPipeline->AddPass(std::move(mlPass));
    } else if (mode == UpscalerMode::ANIME4K) {
        logDiagnostic("Anime4K not yet ported to new pipeline, using Preprocessing only");
        // Fallback or placeholder
    }

    // Ensure at least one pass exists for scaling if input != output
    // If no passes are added, we use BlurFilter with 0 strength as a scaler
    if (inputWidth != outputWidth || inputHeight != outputHeight) {
        // Check if pipeline has passes (we can't check mPipeline->mPasses directly as it's private)
        // But we know if we added any above.
        bool hasPasses = (mode == UpscalerMode::REAL_ESRGAN) || mEnableEdgeSmoothing || mEnableSoftHDR;
        
        if (!hasPasses) {
            logDiagnostic("Adding BlurFilter (Strength 0) as default Scaler");
            auto scalerPass = std::make_unique<renderer::BlurFilter>();
            scalerPass->SetStrength(0.0f);
            mPipeline->AddPass(std::move(scalerPass));
        }
    }

    mState = UpscalerState::STATE_READY;
    mIsAvailable = true;
    logDiagnostic("Initialized upscaler mode: %d", (int)mMode);
    return true;
}

bool UpscalerImpl::reconfigureOutput(int outputWidth, int outputHeight) {
    if (mRealDisplay && mRealDisplay->GetWidth() == outputWidth && mRealDisplay->GetHeight() == outputHeight) return true;
    
    logDiagnostic("Reconfiguring output: %dx%d -> %dx%d", 
        mRealDisplay ? mRealDisplay->GetWidth() : 0, 
        mRealDisplay ? mRealDisplay->GetHeight() : 0, 
        outputWidth, outputHeight);
    
    // Shutdown and re-init
    UpscalerMode currentMode = mMode;
    int inputWidth = mPhantomDisplay ? mPhantomDisplay->GetWidth() : 0;
    int inputHeight = mPhantomDisplay ? mPhantomDisplay->GetHeight() : 0;

    if (inputWidth == 0 || inputHeight == 0) {
        setError("Cannot reconfigure: Input dimensions unknown");
        return false;
    }

    shutdown();
    return init(inputWidth, inputHeight, outputWidth, outputHeight, currentMode);
}

void UpscalerImpl::shutdown() {
    logDiagnostic("Shutting down upscaler");
    if (mPipeline) {
        mPipeline->Shutdown();
        mPipeline.reset();
    }
    mPhantomDisplay.reset();
    mRealDisplay.reset();
    mState = UpscalerState::STATE_UNINITIALIZED;
    mIsAvailable = false;
    if (mUpscaleLog) {
        fclose(mUpscaleLog);
        mUpscaleLog = nullptr;
    }
}

bool UpscalerImpl::setIndexedInput(const unsigned char* indexedBuffer, const uint32_t* palette) {
    if (mState != UpscalerState::STATE_READY || !mPhantomDisplay || !indexedBuffer || !palette) return false;
    mPhantomDisplay->SetData(indexedBuffer, palette);
    return true;
}

bool UpscalerImpl::setRgbaInput(const uint32_t* rgbaBuffer) {
    if (mState != UpscalerState::STATE_READY || !mPhantomDisplay || !rgbaBuffer) return false;
    mPhantomDisplay->SetData(rgbaBuffer);
    return true;
}

bool UpscalerImpl::dispatch() {
    if (mState != UpscalerState::STATE_READY) return false;
    
    static int frameCounter = 0;
    static double totalTime = 0;
    
    auto start = std::chrono::high_resolution_clock::now();

    if (mPipeline && mPhantomDisplay) {
        mPipeline->Dispatch(*mPhantomDisplay);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;
    
    totalTime += elapsed.count();
    frameCounter++;
    
    if (frameCounter % 30 == 0) {
        double avgTime = totalTime / 30.0;
        int inputW = mPhantomDisplay ? mPhantomDisplay->GetWidth() : 0;
        int inputH = mPhantomDisplay ? mPhantomDisplay->GetHeight() : 0;
        int outputW = mRealDisplay ? mRealDisplay->GetWidth() : 0;
        int outputH = mRealDisplay ? mRealDisplay->GetHeight() : 0;
        logDiagnostic("Frame %d: Render Time: %.2f ms (Avg: %.2f ms) | Input: %dx%d | Output: %dx%d", 
            frameCounter, elapsed.count(), avgTime, inputW, inputH, outputW, outputH);
        totalTime = 0;
    }

    return true;
}

// C-style wrappers
int upscalerInit(int inputWidth, int inputHeight, int outputWidth, int outputHeight, UpscalerMode mode) {
    return UpscalerImpl::getInstance()->init(inputWidth, inputHeight, outputWidth, outputHeight, mode) ? 0 : -1;
}

int upscalerReconfigureOutput(int outputWidth, int outputHeight) {
    return UpscalerImpl::getInstance()->reconfigureOutput(outputWidth, outputHeight) ? 0 : -1;
}

void upscalerShutdown() {
    UpscalerImpl::getInstance()->shutdown();
}

int upscalerSetIndexedInput(const unsigned char* indexedBuffer, const uint32_t* palette) {
    return UpscalerImpl::getInstance()->setIndexedInput(indexedBuffer, palette) ? 0 : -1;
}

int upscalerSetRgbaInput(const uint32_t* rgbaBuffer) {
    return UpscalerImpl::getInstance()->setRgbaInput(rgbaBuffer) ? 0 : -1;
}

int upscalerDispatch() {
    return UpscalerImpl::getInstance()->dispatch() ? 0 : -1;
}

const uint32_t* upscalerGetOutputBuffer() {
    return UpscalerImpl::getInstance()->getOutputBuffer();
}

int upscalerGetOutputPitch() {
    return UpscalerImpl::getInstance()->getOutputPitch();
}

void upscalerGetOutputDimensions(int& width, int& height) {
    UpscalerImpl::getInstance()->getOutputDimensions(width, height);
}

bool upscalerIsAvailable() {
    return UpscalerImpl::getInstance()->isAvailable();
}

UpscalerState upscalerGetState() {
    return UpscalerImpl::getInstance()->getState();
}

int upscalerSetQuality(UpscalerQuality quality) {
    return UpscalerImpl::getInstance()->setQuality(quality) ? 0 : -1;
}

UpscalerMode upscalerGetConfiguredMode() {
    return UpscalerImpl::getInstance()->getConfiguredMode();
}

int upscalerSetSharpness(float sharpness) {
    return UpscalerImpl::getInstance()->setSharpness(sharpness) ? 0 : -1;
}

void upscalerSetMotionVectorsEnabled(bool enabled) {
    UpscalerImpl::getInstance()->setMotionVectorsEnabled(enabled);
}

int upscalerSetFilterParams(float edgeStrength, float colorStrength, float saturation, float contrast) {
    UpscalerImpl::getInstance()->setFilterParams(edgeStrength, colorStrength, saturation, contrast);
    return 0;
}

void upscalerSetVerboseLogging(bool enabled) {
    UpscalerImpl::getInstance()->setVerboseLogging(enabled);
}

void upscalerReloadConfig() {
    UpscalerImpl::getInstance()->reloadConfig();
}

const char* upscalerGetLastError() {
    return UpscalerImpl::getInstance()->getLastError();
}

} // namespace fallout
