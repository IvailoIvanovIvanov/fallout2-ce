#include "upscaler.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#include <SDL.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "diagnostics.h"
#include "gpu_device.h"
#include "gpu_texture.h"
#include "memory.h"
#include "upscaler_postprocess_shader.h"
#include "upscaler_ml_postprocess_shader.h"
#include "upscaler_ml.h"
#include "upscaler_preprocess_shader.h"
#include "game_config.h"

namespace fallout {

// Constants
constexpr int MAX_RESOLUTION = 8192;
constexpr int MIN_RESOLUTION = 320;
constexpr int ERROR_MSG_SIZE = 256;

/**
 * Upscaler implementation
 * Handles scaling of the 640x480 game output to modern resolutions.
 * Supports Integer Scaling (2x, 3x, 4x) and Anime4K (experimental).
 */
class UpscalerImpl {
public:
    static UpscalerImpl* getInstance() {
        static UpscalerImpl instance;
        return &instance;
    }

    UpscalerImpl(const UpscalerImpl&) = delete;
    UpscalerImpl& operator=(const UpscalerImpl&) = delete;

    // State management
    UpscalerState getState() const { return mState; }

    bool init(int inputWidth, int inputHeight, int outputWidth, int outputHeight, UpscalerMode mode);
    bool reconfigureOutput(int outputWidth, int outputHeight);
    void shutdown();

    bool setIndexedInput(const unsigned char* indexedBuffer, const uint32_t* palette);
    bool setRgbaInput(const uint32_t* rgbaBuffer);
    bool dispatch();

    const uint32_t* getOutputBuffer() const { 
        // Always return mOutputBuffer. For REAL_ESRGAN, we memcpy the readback data 
        // to mOutputBuffer in dispatchRealEsrgan to ensure a stable, cached buffer 
        // for the game engine to read.
        return mOutputBuffer; 
    }
    int getOutputPitch() const { return mOutputPitch; }
    void getOutputDimensions(int& width, int& height) const {
        width = mOutputWidth;
        height = mOutputHeight;
    }

    bool setQuality(UpscalerQuality quality);
    bool setSharpness(float sharpness);
    
    // Deprecated but kept for API compatibility
    void setMotionVectorsEnabled(bool enabled) { /* No-op */ }
    void setFilterParams(float edge, float color, float sat, float con) { /* No-op */ }
    
    void setVerboseLogging(bool enabled);
    void reloadConfig();

    bool isAvailable() const { return mIsAvailable; }
    const char* getLastError() const { return mLastError; }
    UpscalerMode getConfiguredMode() const { return mConfiguredMode; }

private:
    UpscalerImpl();

    // Logging and Diagnostics
    void logDiagnostic(const char* format, ...);
    void setError(const char* format, ...);
    
    // Configuration
    void loadConfiguration();
    void loadUpscalerConfig();
    void loadFilterConfig();
    void saveConfiguration();
    
    // Implementation methods
    bool initAnime4k();
    bool shutdownAnime4k();
    bool dispatchAnime4k();

    // Real-ESRGAN Helpers
    bool initRealEsrgan();
    bool shutdownRealEsrgan();
    bool dispatchRealEsrgan();
    bool initMlGpuPipeline();
    bool shutdownMlGpuPipeline();

    // Buffer management
    bool allocateInputBuffer(int width, int height);
    bool allocateOutputBuffer(int width, int height);
    void deallocateBuffers();

    // Anime4K Helpers
    bool checkGpuReadiness();
    bool compileAnime4kShader(Microsoft::WRL::ComPtr<ID3DBlob>& shaderBlob);
    bool createAnime4kRootSignature();
    bool createAnime4kPipelineState(ID3DBlob* shaderBlob);
    bool createAnime4kDescriptorHeap();
    bool createAnime4kTextures();
    bool createAnime4kDescriptors();
    bool createAnime4kCommandList();
    
    // GPU Post-Processing Helpers
    bool initGpuPostProcess();
    bool shutdownGpuPostProcess();

    // State variables
    UpscalerState mState = UpscalerState::STATE_UNINITIALIZED;
    UpscalerMode mMode = UpscalerMode::NONE;
    UpscalerMode mConfiguredMode = UpscalerMode::NONE;
    UpscalerQuality mQuality = UpscalerQuality::BALANCED;
    bool mIsAvailable = false;
    bool mAnime4kInitialized = false;
    
    // Anime4K D3D12 resources
    void* mAnime4kPipelineState = nullptr;      // ID3D12PipelineState*
    void* mAnime4kRootSignature = nullptr;      // ID3D12RootSignature*
    void* mAnime4kDescriptorHeap = nullptr;     // ID3D12DescriptorHeap*
    GpuTextureHandle mAnime4kInputTexture = {}; 
    GpuTextureHandle mAnime4kOutputTexture = {};
    void* mAnime4kCommandAllocator = nullptr;   // ID3D12CommandAllocator*
    void* mAnime4kCommandList = nullptr;        // ID3D12GraphicsCommandList*
    
    // GPU Post-Processing (runs after Anime4K on GPU)
    bool mGpuPostProcessInitialized = false;
    void* mPostProcessRootSignature = nullptr;  // ID3D12RootSignature*
    void* mPostProcessPipelineState = nullptr;  // ID3D12PipelineState*

    // ML Upscaler
    std::unique_ptr<UpscalerML> mUpscalerML;
    uint32_t mMlFrameSkip = 3;  // Only run ML every N frames (1=every frame, 3=every 3rd frame)
    std::string mMlModelFile;
    uint32_t mMlFrameCounter = 0;
    uint32_t* mMlCachedOutput = nullptr;  // Cache the last ML output
    
    // ML GPU Pipeline Resources
    void* mMlPreprocessPipeline = nullptr;   // ID3D12PipelineState*
    void* mMlPreprocessRootSig = nullptr;    // ID3D12RootSignature*
    void* mMlPostprocessPipeline = nullptr;  // ID3D12PipelineState*
    void* mMlPostprocessRootSig = nullptr;   // ID3D12RootSignature*
    GpuTextureHandle mMlPlanarR = {};        // Float32 R plane
    GpuTextureHandle mMlPlanarG = {};        // Float32 G plane
    GpuTextureHandle mMlPlanarB = {};        // Float32 B plane
    GpuTextureHandle mMlOutputPlanarR = {};  // Output R plane
    GpuTextureHandle mMlOutputPlanarG = {};  // Output G plane
    GpuTextureHandle mMlOutputPlanarB = {};  // Output B plane
    void* mMlPlanarInputBuffer = nullptr;    // ID3D12Resource* (Planar RGB float buffer)
    void* mMlPlanarOutputBuffer = nullptr;   // ID3D12Resource* (Planar RGB float buffer for ML output)
    void* mMlFinalOutputBuffer = nullptr;    // ID3D12Resource* (Interleaved RGBA8 buffer for final output)
    void* mMlReadbackBuffer = nullptr;       // ID3D12Resource* (Readback buffer for CPU access)
    void* mMlReadbackMappedPtr = nullptr;    // Mapped pointer to readback buffer

    // Dimensions
    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;

    // Buffers (ARGB8888 format)
    uint32_t* mInputBuffer = nullptr;
    uint32_t* mOutputBuffer = nullptr;
    uint32_t* mTempBuffer = nullptr;
    int mInputPitch = 0;
    int mOutputPitch = 0;

    // Configuration
    float mSharpness = 0.5f;
    uint32_t mFrameIndex = 0;
    
    // Filter configuration
    bool mEnableDebanding = true;
    float mDebandingStrength = 0.5f;
    bool mEnableEdgeSmoothing = true;
    float mSmoothingStrength = 0.6f;
    
    // Kuwahara filter
    bool mEnableKuwahara = false;
    int mKuwaharaRadius = 2;
    
    // Soft HDR
    bool mEnableSoftHDR = false;
    float mHdrStrength = 0.5f;
    float mHdrSaturation = 1.2f;
    float mHdrContrast = 1.1f;
    float mBlackCrushThreshold = 0.03f;  // Luminance below which to crush to black
    float mBlackCrushStrength = 1.0f;    // How aggressively to darken near-blacks
    
    bool mVerboseLogging = false;

    // Error tracking
    char mLastError[ERROR_MSG_SIZE] = {};
    
    // Logging
    FILE* mUpscaleLog = nullptr;
    std::string mLogFilePath;
};

// Global singleton accessor
UpscalerImpl* upscalerGetImpl() {
    return UpscalerImpl::getInstance();
}

// ============================================================================
// Constructor & Logging
// ============================================================================

UpscalerImpl::UpscalerImpl() {
    // Initialize log file on singleton creation
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
        fprintf(mUpscaleLog, "   FALLOUT 2 CE - UPSCALER DIAGNOSTICS LOG\n");
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
    
    FILE* f = fopen("C:/Temp/upscale_debug.log", "a");
    if (f) {
        time_t now = time(nullptr);
        struct tm timeinfo;
#if _WIN32
        localtime_s(&timeinfo, &now);
#else
        localtime_r(&now, &timeinfo);
#endif
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeinfo);
        
        fprintf(f, "[%s] [UPSCALER] %s\n", timestamp, buffer);
        fclose(f);
    }
}

void UpscalerImpl::setError(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(mLastError, ERROR_MSG_SIZE, format, args);
    va_end(args);
    diagnosticsLog(static_cast<DiagnosticsLevel>(1), "UPSCALER", "%s", mLastError);
}

// ============================================================================
// Configuration
// ============================================================================

void UpscalerImpl::loadConfiguration() {
    if (!gGameConfigInitialized) return;
    
    logDiagnostic("Loading configuration...");
    loadUpscalerConfig();
    loadFilterConfig();
    logDiagnostic("Configuration loaded.");
}

void UpscalerImpl::loadUpscalerConfig() {
    // Mode
    int modeValue = 0; // Default to NONE
    if (configGetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_MODE_KEY, &modeValue)) {
        // Clamp to valid range [0-5]
        if (modeValue < 0 || modeValue > 5) {
            logDiagnostic("Invalid mode %d in config, defaulting to NONE", modeValue);
            modeValue = 0; // NONE
        }
        mConfiguredMode = static_cast<UpscalerMode>(modeValue);
    } else {
        mConfiguredMode = UpscalerMode::NONE;
    }
    
    // Quality
    int qualityValue = 1;
    if (configGetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_QUALITY_KEY, &qualityValue)) {
        mQuality = static_cast<UpscalerQuality>(qualityValue);
    }
    
    // Sharpness
    double sharpnessValue = 0.5;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_SHARPNESS_KEY, &sharpnessValue)) {
        mSharpness = static_cast<float>(sharpnessValue);
    }
    
    // Verbose Logging
    bool verboseValue = false;
    if (configGetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_VERBOSE_LOG_KEY, &verboseValue)) {
        mVerboseLogging = verboseValue;
    }
    
    // ML Frame Skip
    int frameSkip = 3;
    if (configGetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_ml_frame_skip", &frameSkip)) {
        mMlFrameSkip = std::clamp(frameSkip, 1, 10);
    }
    logDiagnostic("ML Frame Skip: %d", mMlFrameSkip);

    // ML Model File
    char* modelFile = nullptr;
    if (configGetString(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_model_file", &modelFile)) {
        mMlModelFile = std::string(modelFile);
    } else {
        mMlModelFile = "RealESRGAN_x4plus_anime_6B.onnx"; // Default to the requested model
    }
    logDiagnostic("ML Model File: %s", mMlModelFile.c_str());
}

void UpscalerImpl::loadFilterConfig() {
    // Debanding
    configGetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_DEBANDING_KEY, &mEnableDebanding);
    double debandingStr = 0.5;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_DEBANDING_STRENGTH_KEY, &debandingStr)) {
        mDebandingStrength = static_cast<float>(debandingStr);
    }
    
    // Edge Smoothing
    configGetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_EDGE_SMOOTHING_KEY, &mEnableEdgeSmoothing);
    double smoothingStr = 0.6;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_SMOOTHING_STRENGTH_KEY, &smoothingStr)) {
        mSmoothingStrength = static_cast<float>(smoothingStr);
    }
    
    // Kuwahara
    configGetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_kuwahara_enable", &mEnableKuwahara);
    int radius = 2;
    if (configGetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_kuwahara_radius", &radius)) {
        mKuwaharaRadius = std::clamp(radius, 1, 5);
    }
    
    // Soft HDR
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

// ============================================================================
// Buffer Management
// ============================================================================

bool UpscalerImpl::allocateInputBuffer(int width, int height) {
    if (width <= 0 || height <= 0 || width > MAX_RESOLUTION || height > MAX_RESOLUTION) {
        setError("Invalid input dimensions: %dx%d", width, height);
        return false;
    }

    deallocateBuffers();
    mInputWidth = width;
    mInputHeight = height;
    mInputPitch = width * static_cast<int>(sizeof(uint32_t));

    size_t bufferSize = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint32_t);
    mInputBuffer = static_cast<uint32_t*>(internal_malloc(bufferSize));

    if (mInputBuffer == nullptr) {
        setError("Failed to allocate input buffer");
        return false;
    }
    return true;
}

bool UpscalerImpl::allocateOutputBuffer(int width, int height) {
    if (width <= 0 || height <= 0 || width > MAX_RESOLUTION || height > MAX_RESOLUTION) {
        setError("Invalid output dimensions: %dx%d", width, height);
        return false;
    }

    if (mOutputBuffer != nullptr) {
        internal_free(mOutputBuffer);
        mOutputBuffer = nullptr;
    }

    mOutputWidth = width;
    mOutputHeight = height;
    mOutputPitch = width * static_cast<int>(sizeof(uint32_t));

    size_t bufferSize = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint32_t);
    mOutputBuffer = static_cast<uint32_t*>(internal_malloc(bufferSize));

    if (mOutputBuffer == nullptr) {
        setError("Failed to allocate output buffer");
        return false;
    }
    return true;
}

void UpscalerImpl::deallocateBuffers() {
    if (mInputBuffer) { internal_free(mInputBuffer); mInputBuffer = nullptr; }
    if (mOutputBuffer) { internal_free(mOutputBuffer); mOutputBuffer = nullptr; }
    if (mTempBuffer) { internal_free(mTempBuffer); mTempBuffer = nullptr; }
    if (mMlCachedOutput) { internal_free(mMlCachedOutput); mMlCachedOutput = nullptr; }
    mInputWidth = 0; mInputHeight = 0;
    mOutputWidth = 0; mOutputHeight = 0;
}

// ============================================================================
// Initialization & Shutdown
// ============================================================================

bool UpscalerImpl::init(int inputWidth, int inputHeight, int outputWidth, int outputHeight, UpscalerMode mode) {
    logDiagnostic("=== INIT: %dx%d -> %dx%d, requested_mode=%d ===", 
                  inputWidth, inputHeight, outputWidth, outputHeight, static_cast<int>(mode));
    
    loadConfiguration();
    mode = mConfiguredMode; // Override with config
    
    logDiagnostic("Using configured mode: %d", static_cast<int>(mode));
    
    if (mState != UpscalerState::STATE_UNINITIALIZED) {
        setError("Upscaler already initialized");
        return false;
    }

    mState = UpscalerState::STATE_INITIALIZING;
    mMode = mode;

    if (mode == UpscalerMode::NONE) {
        mState = UpscalerState::STATE_READY;
        mIsAvailable = false;
        return true;
    }

    // Allocate buffers
    if (!allocateInputBuffer(inputWidth, inputHeight) || !allocateOutputBuffer(outputWidth, outputHeight)) {
        mState = UpscalerState::STATE_ERROR;
        return false;
    }

    // Initialize specific mode
    if (mode == UpscalerMode::ANIME4K) {
        if (!initAnime4k()) {
            logDiagnostic("Anime4K init failed, falling back to NONE");
            mMode = UpscalerMode::NONE;
            mIsAvailable = false;
        }
    } else if (mode == UpscalerMode::REAL_ESRGAN) {
        logDiagnostic("Mode is REAL_ESRGAN, attempting initialization...");
        if (!initRealEsrgan()) {
            logDiagnostic("!!! Real-ESRGAN init failed, falling back to NONE !!!");
            logDiagnostic("Last error: %s", mLastError);
            mMode = UpscalerMode::NONE;
            mIsAvailable = false;
        }
    }

    mIsAvailable = true;
    mState = UpscalerState::STATE_READY;
    logDiagnostic("Initialized upscaler mode: %d", (int)mMode);
    return true;
}

bool UpscalerImpl::reconfigureOutput(int outputWidth, int outputHeight) {
    if (mState != UpscalerState::STATE_READY) return false;
    if (outputWidth == mOutputWidth && outputHeight == mOutputHeight) return true;

    logDiagnostic("Reconfiguring output: %dx%d -> %dx%d", mOutputWidth, mOutputHeight, outputWidth, outputHeight);

    if (mMode == UpscalerMode::ANIME4K) {
        shutdownAnime4k();
        if (!allocateOutputBuffer(outputWidth, outputHeight)) return false;
        if (!initAnime4k()) return false;
    } else if (mMode == UpscalerMode::REAL_ESRGAN) {
        shutdownRealEsrgan();
        if (!allocateOutputBuffer(outputWidth, outputHeight)) return false;
        if (!initRealEsrgan()) return false;
    } else {
        if (!allocateOutputBuffer(outputWidth, outputHeight)) return false;
    }
    return true;
}

void UpscalerImpl::shutdown() {
    if (mMode == UpscalerMode::ANIME4K) {
        shutdownAnime4k();
    } else if (mMode == UpscalerMode::REAL_ESRGAN) {
        shutdownRealEsrgan();
    }
    deallocateBuffers();
    mState = UpscalerState::STATE_UNINITIALIZED;
    mIsAvailable = false;
    if (mUpscaleLog) {
        fclose(mUpscaleLog);
        mUpscaleLog = nullptr;
    }
}

// ============================================================================
// Input Handling
// ============================================================================

bool UpscalerImpl::setIndexedInput(const unsigned char* indexedBuffer, const uint32_t* palette) {
    if (mState != UpscalerState::STATE_READY || !mInputBuffer || !indexedBuffer || !palette) return false;

    for (int i = 0; i < mInputWidth * mInputHeight; ++i) {
        uint32_t paletteEntry = palette[indexedBuffer[i]];
        mInputBuffer[i] = (paletteEntry & 0xFFFFFF) | 0xFF000000;
    }
    return true;
}

bool UpscalerImpl::setRgbaInput(const uint32_t* rgbaBuffer) {
    if (mState != UpscalerState::STATE_READY || !mInputBuffer || !rgbaBuffer) return false;
    std::memcpy(mInputBuffer, rgbaBuffer, mInputWidth * mInputHeight * sizeof(uint32_t));
    return true;
}

// ============================================================================
// Dispatch
// ============================================================================

bool UpscalerImpl::dispatch() {
    logDiagnostic("=== DISPATCH: mode=%d, state=%d ===", static_cast<int>(mMode), static_cast<int>(mState));
    
    if (mState != UpscalerState::STATE_READY) {
        logDiagnostic("ERROR: Not ready, state=%d", static_cast<int>(mState));
        return false;
    }

    switch (mMode) {
        case UpscalerMode::ANIME4K:
            logDiagnostic("Dispatching ANIME4K");
            return dispatchAnime4k();
        case UpscalerMode::REAL_ESRGAN:
            logDiagnostic("Dispatching REAL_ESRGAN");
            return dispatchRealEsrgan();
        default:
            logDiagnostic("Mode NONE/Unknown - passthrough");
            return true; // Pass-through or None
    }
}

// ============================================================================
// Anime4K Implementation
// ============================================================================

bool UpscalerImpl::checkGpuReadiness() {
    if (!gpuDeviceIsReady()) {
        setError("GPU device not initialized");
        return false;
    }
    if (gpuDeviceGetDevice() == nullptr) {
        setError("GPU device is null");
        return false;
    }
    return true;
}

bool UpscalerImpl::compileAnime4kShader(Microsoft::WRL::ComPtr<ID3DBlob>& shaderBlob) {
    // Embedded shader code (Anime4K v3.2 Upscale Original x2)
    const char* shaderCode = R"(
#define REFINE_STRENGTH 0.5
#define REFINE_BIAS 0.0
#define P5 ( 11.68129591)
#define P4 (-42.46906057)
#define P3 ( 60.28286266)
#define P2 (-41.84451327)
#define P1 ( 14.05517353)
#define P0 (-1.081521930)

cbuffer UpscaleParams : register(b0) {
    uint2 inputSize; uint2 outputSize; uint2 effectiveSize; uint2 offset;
    float2 rcpInput; float2 rcpEffectiveOutput; float strength; float3 padding;
};

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);
SamplerState LinearSampler : register(s0);

float get_luma(float4 c) { return dot(c.rgb, float3(0.299, 0.587, 0.114)); }
float power_function(float x) { float x2=x*x; float x3=x2*x; return P5*x2*x3 + P4*x2*x2 + P3*x3 + P2*x2 + P1*x + P0; }

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= outputSize.x || DTid.y >= outputSize.y) return;
    if (DTid.x < offset.x || DTid.x >= offset.x + effectiveSize.x ||
        DTid.y < offset.y || DTid.y >= offset.y + effectiveSize.y) {
        OutputTexture[DTid.xy] = float4(0, 0, 0, 1); return;
    }

    float2 pixelPos = float2(DTid.xy) - float2(offset);
    float2 uv = (pixelPos + 0.5f) * rcpEffectiveOutput;
    float2 d = rcpEffectiveOutput;

    float4 cc = InputTexture.SampleLevel(LinearSampler, uv, 0);
    float t = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(0, -d.y), 0));
    float b = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(0, d.y), 0));
    float l = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(-d.x, 0), 0));
    float r = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(d.x, 0), 0));
    float tl = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(-d.x, -d.y), 0));
    float tr = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(d.x, -d.y), 0));
    float bl = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(-d.x, d.y), 0));
    float br = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(d.x, d.y), 0));

    float gx = (tr + 2.0 * r + br) - (tl + 2.0 * l + bl);
    float gy = (bl + 2.0 * b + br) - (tl + 2.0 * t + tr);
    float sobel_norm = sqrt(gx * gx + gy * gy) / 4.0;
    float dval = saturate(power_function(saturate(sobel_norm)) * REFINE_STRENGTH + REFINE_BIAS);

    float xpos = (gx > 0.0) ? 1.0 : -1.0;
    float ypos = (gy > 0.0) ? 1.0 : -1.0;
    float4 xval = InputTexture.SampleLevel(LinearSampler, uv + float2(d.x * xpos, 0), 0);
    float4 yval = InputTexture.SampleLevel(LinearSampler, uv + float2(0, d.y * ypos), 0);
    float xy_ratio = abs(gx) / (abs(gx) + abs(gy) + 0.0001);
    float4 avg = xval * xy_ratio + yval * (1.0 - xy_ratio);

    OutputTexture[DTid.xy] = avg * dval + cc * (1.0 - dval);
}
)";

    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(shaderCode, strlen(shaderCode), "Anime4K", nullptr, nullptr, "main", "cs_5_0", 
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shaderBlob.GetAddressOf(), errorBlob.GetAddressOf());
    
    if (FAILED(hr)) {
        if (errorBlob) setError("Shader compile failed: %s", (char*)errorBlob->GetBufferPointer());
        return false;
    }
    return true;
}

bool UpscalerImpl::createAnime4kRootSignature() {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 2;
    rootParams[1].DescriptorTable.pDescriptorRanges = ranges;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = { 2, rootParams, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    
    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob, errorBlob;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob))) return false;

    ID3D12Device* device = gpuDeviceGetDevice();
    ID3D12RootSignature* rootSig = nullptr;
    if (FAILED(device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig)))) return false;
    
    mAnime4kRootSignature = rootSig;
    return true;
}

bool UpscalerImpl::createAnime4kPipelineState(ID3DBlob* shaderBlob) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = static_cast<ID3D12RootSignature*>(mAnime4kRootSignature);
    psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };
    
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(gpuDeviceGetDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)))) return false;
    
    mAnime4kPipelineState = pso;
    return true;
}

bool UpscalerImpl::createAnime4kDescriptorHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 4; // Increased to 4 for ML pipeline (Input SRV, Planar UAV, Planar SRV, Final UAV)
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    
    ID3D12DescriptorHeap* heap = nullptr;
    if (FAILED(gpuDeviceGetDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)))) return false;
    
    mAnime4kDescriptorHeap = heap;
    return true;
}

bool UpscalerImpl::createAnime4kTextures() {
    mAnime4kInputTexture = gpuTextureCreate(mInputWidth, mInputHeight, GpuTextureFormat::ARGB8888, (int)GpuTextureUsage::SHADER_RESOURCE);
    if (!mAnime4kInputTexture.resource) return false;
    
    mAnime4kOutputTexture = gpuTextureCreate(mOutputWidth, mOutputHeight, GpuTextureFormat::ARGB8888, (int)GpuTextureUsage::UNORDERED_ACCESS);
    if (!mAnime4kOutputTexture.resource) return false;
    
    return true;
}

bool UpscalerImpl::createAnime4kDescriptors() {
    ID3D12Device* device = gpuDeviceGetDevice();
    ID3D12DescriptorHeap* heap = static_cast<ID3D12DescriptorHeap*>(mAnime4kDescriptorHeap);
    if (!heap) {
        logDiagnostic("ERROR: Descriptor heap is null in createAnime4kDescriptors!");
        return false;
    }
    UINT handleSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();

    // SRV (Input texture for Anime4K)
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(gpuTextureGetResource(mAnime4kInputTexture), &srvDesc, handle);
    logDiagnostic("Created SRV descriptor at offset 0");

    // UAV (Output buffer for ML preprocessing)
    // Note: We'll create this as a structured buffer UAV for the planar RGB float buffer
    handle.ptr += handleSize;
    if (mMlPlanarInputBuffer) {
        ID3D12Resource* planarRes = static_cast<ID3D12Resource*>(mMlPlanarInputBuffer);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = mInputWidth * mInputHeight * 3;  // Total float count
        uavDesc.Buffer.StructureByteStride = sizeof(float);
        device->CreateUnorderedAccessView(planarRes, nullptr, &uavDesc, handle);
        logDiagnostic("Created UAV descriptor at offset 1 for planar buffer");
    } else {
        logDiagnostic("WARNING: mMlPlanarInputBuffer is null, skipping UAV creation");
    }

    // SRV (Planar Output Buffer for ML postprocessing)
    handle.ptr += handleSize;
    if (mMlPlanarOutputBuffer) {
        ID3D12Resource* planarRes = static_cast<ID3D12Resource*>(mMlPlanarOutputBuffer);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc2 = {};
        srvDesc2.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc2.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc2.Buffer.FirstElement = 0;
        srvDesc2.Buffer.NumElements = (mInputWidth * 4) * (mInputHeight * 4) * 3;
        srvDesc2.Buffer.StructureByteStride = sizeof(float);
        device->CreateShaderResourceView(planarRes, &srvDesc2, handle);
        logDiagnostic("Created SRV descriptor at offset 2 for planar output buffer");
    }

    // UAV (Final Interleaved Output Buffer for ML postprocessing)
    handle.ptr += handleSize;
    if (mMlFinalOutputBuffer) {
        ID3D12Resource* finalRes = static_cast<ID3D12Resource*>(mMlFinalOutputBuffer);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc2 = {};
        uavDesc2.Format = DXGI_FORMAT_R32_UINT; // Typed Buffer
        uavDesc2.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc2.Buffer.FirstElement = 0;
        uavDesc2.Buffer.NumElements = mOutputWidth * mOutputHeight;
        uavDesc2.Buffer.StructureByteStride = 0; // Must be 0 for Typed Buffer
        device->CreateUnorderedAccessView(finalRes, nullptr, &uavDesc2, handle);
        logDiagnostic("Created UAV descriptor at offset 3 for final output buffer (Typed R32_UINT)");
    }
    
    return true;
}

bool UpscalerImpl::createAnime4kCommandList() {
    mAnime4kCommandAllocator = gpuDeviceCreateCommandAllocator();
    if (!mAnime4kCommandAllocator) return false;
    
    mAnime4kCommandList = gpuDeviceCreateCommandList(static_cast<ID3D12CommandAllocator*>(mAnime4kCommandAllocator));
    if (!mAnime4kCommandList) return false;
    
    static_cast<ID3D12GraphicsCommandList*>(mAnime4kCommandList)->Close();
    return true;
}

bool UpscalerImpl::initAnime4k() {
    if (!checkGpuReadiness()) return false;

    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    if (!compileAnime4kShader(shaderBlob)) return false;
    if (!createAnime4kRootSignature()) return false;
    if (!createAnime4kPipelineState(shaderBlob.Get())) return false;
    if (!createAnime4kDescriptorHeap()) return false;
    if (!createAnime4kTextures()) return false;
    if (!createAnime4kDescriptors()) return false;
    
    gpuDeviceWaitForGpu(); // Sync before command list creation
    
    if (!createAnime4kCommandList()) return false;
    
    // Initialize GPU post-processing
    if (!initGpuPostProcess()) {
        logDiagnostic("WARNING: GPU post-processing init failed, will fallback to CPU");
    }

    mAnime4kInitialized = true;
    return true;
}

bool UpscalerImpl::shutdownAnime4k() {
    if (!mAnime4kInitialized) return true;
    
    // Shutdown GPU post-processing
    shutdownGpuPostProcess();
    
    if (mAnime4kCommandList) { static_cast<ID3D12GraphicsCommandList*>(mAnime4kCommandList)->Release(); mAnime4kCommandList = nullptr; }
    if (mAnime4kCommandAllocator) { static_cast<ID3D12CommandAllocator*>(mAnime4kCommandAllocator)->Release(); mAnime4kCommandAllocator = nullptr; }
    if (mAnime4kOutputTexture.resource) { gpuTextureRelease(mAnime4kOutputTexture); mAnime4kOutputTexture = {}; }
    if (mAnime4kInputTexture.resource) { gpuTextureRelease(mAnime4kInputTexture); mAnime4kInputTexture = {}; }
    if (mAnime4kDescriptorHeap) { static_cast<ID3D12DescriptorHeap*>(mAnime4kDescriptorHeap)->Release(); mAnime4kDescriptorHeap = nullptr; }
    if (mAnime4kPipelineState) { static_cast<ID3D12PipelineState*>(mAnime4kPipelineState)->Release(); mAnime4kPipelineState = nullptr; }
    if (mAnime4kRootSignature) { static_cast<ID3D12RootSignature*>(mAnime4kRootSignature)->Release(); mAnime4kRootSignature = nullptr; }
    
    mAnime4kInitialized = false;
    return true;
}

// ============================================================================
// GPU Post-Processing (runs after Anime4K)
// ============================================================================

bool UpscalerImpl::initGpuPostProcess() {
    if (mGpuPostProcessInitialized) {
        logDiagnostic("GPU PostProcess already initialized");
        return true;
    }
    if (!checkGpuReadiness()) {
        logDiagnostic("GPU PostProcess init failed: GPU not ready");
        return false;
    }
    
    logDiagnostic("Initializing GPU PostProcess...");
    ID3D12Device* device = gpuDeviceGetDevice();
    
    // Compile post-process shader
    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    
    logDiagnostic("Compiling post-process shader...");
    HRESULT hr = D3DCompile(
        UPSCALER_POSTPROCESS_SHADER,
        strlen(UPSCALER_POSTPROCESS_SHADER),
        "PostProcessShader",
        nullptr, nullptr,
        "main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &shaderBlob, &errorBlob
    );
    
    if (FAILED(hr)) {
        logDiagnostic("PostProcess shader compile FAILED: HRESULT=0x%08X", hr);
        if (errorBlob) {
            logDiagnostic("PostProcess shader compile error: %s", (char*)errorBlob->GetBufferPointer());
        }
        return false;
    }
    
    logDiagnostic("PostProcess shader compiled successfully");
    
    // Create root signature
    D3D12_ROOT_PARAMETER rootParams[2] = {};
    
    // CBV for parameters
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    // UAV descriptor table
    D3D12_DESCRIPTOR_RANGE descRange = {};
    descRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descRange.NumDescriptors = 1;
    descRange.BaseShaderRegister = 0;
    descRange.RegisterSpace = 0;
    descRange.OffsetInDescriptorsFromTableStart = 0;
    
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &descRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 2;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    
    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
    logDiagnostic("Serializing root signature...");
    hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
    if (FAILED(hr)) {
        logDiagnostic("Root signature serialization FAILED: HRESULT=0x%08X", hr);
        return false;
    }
    
    ID3D12RootSignature* rootSig = nullptr;
    logDiagnostic("Creating root signature...");
    hr = device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        logDiagnostic("Root signature creation FAILED: HRESULT=0x%08X", hr);
        return false;
    }
    mPostProcessRootSignature = rootSig;
    
    // Create pipeline state
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSig;
    psoDesc.CS.pShaderBytecode = shaderBlob->GetBufferPointer();
    psoDesc.CS.BytecodeLength = shaderBlob->GetBufferSize();
    
    ID3D12PipelineState* pso = nullptr;
    logDiagnostic("Creating compute pipeline state...");
    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        logDiagnostic("Compute PSO creation FAILED: HRESULT=0x%08X", hr);
        rootSig->Release();
        mPostProcessRootSignature = nullptr;
        return false;
    }
    mPostProcessPipelineState = pso;
    
    mGpuPostProcessInitialized = true;
    logDiagnostic("GPU PostProcess initialized successfully - ready to process");
    return true;
}

bool UpscalerImpl::shutdownGpuPostProcess() {
    if (!mGpuPostProcessInitialized) return true;
    
    if (mPostProcessPipelineState) {
        static_cast<ID3D12PipelineState*>(mPostProcessPipelineState)->Release();
        mPostProcessPipelineState = nullptr;
    }
    if (mPostProcessRootSignature) {
        static_cast<ID3D12RootSignature*>(mPostProcessRootSignature)->Release();
        mPostProcessRootSignature = nullptr;
    }
    
    mGpuPostProcessInitialized = false;
    return true;
}

bool UpscalerImpl::dispatchAnime4k() {
    logDiagnostic("ANIME4K: initialized=%d, inputBuf=%p, outputBuf=%p", 
                  mAnime4kInitialized, mInputBuffer, mOutputBuffer);
    
    if (!mAnime4kInitialized || !mInputBuffer || !mOutputBuffer) {
        logDiagnostic("ANIME4K: Falling back to NONE");
        return false;
    }

    ID3D12GraphicsCommandList* cmdList = static_cast<ID3D12GraphicsCommandList*>(mAnime4kCommandList);
    ID3D12CommandAllocator* allocator = static_cast<ID3D12CommandAllocator*>(mAnime4kCommandAllocator);
    
    allocator->Reset();
    cmdList->Reset(allocator, static_cast<ID3D12PipelineState*>(mAnime4kPipelineState));
    
    if (!gpuTextureUpload(mAnime4kInputTexture, mInputBuffer, mInputWidth * mInputHeight * 4)) return false;

    // Transition resources for compute shader
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    
    // Input: COMMON -> SRV
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[0].Transition.pResource = gpuTextureGetResource(mAnime4kInputTexture);
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Output: COMMON -> UAV
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[1].Transition.pResource = gpuTextureGetResource(mAnime4kOutputTexture);
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    
    cmdList->ResourceBarrier(2, barriers);

    cmdList->SetComputeRootSignature(static_cast<ID3D12RootSignature*>(mAnime4kRootSignature));
    cmdList->SetPipelineState(static_cast<ID3D12PipelineState*>(mAnime4kPipelineState));
    
    // Constant Buffer
    struct {
        uint32_t w, h, ow, oh, ew, eh, ox, oy;
        float riX, riY, reX, reY, str, pad[3];
    } params;
    
    float scale = std::min((float)mOutputWidth / mInputWidth, (float)mOutputHeight / mInputHeight);
    params.w = mInputWidth; params.h = mInputHeight;
    params.ow = mOutputWidth; params.oh = mOutputHeight;
    params.ew = (uint32_t)(mInputWidth * scale); params.eh = (uint32_t)(mInputHeight * scale);
    params.ox = (mOutputWidth - params.ew) / 2; params.oy = (mOutputHeight - params.eh) / 2;
    params.riX = 1.0f / mInputWidth; params.riY = 1.0f / mInputHeight;
    params.reX = 1.0f / params.ew; params.reY = 1.0f / params.eh;
    params.str = 1.0f;

    uint64_t cbAddr;
    if (!gpuUploadConstantBuffer(&params, sizeof(params), &cbAddr)) return false;
    cmdList->SetComputeRootConstantBufferView(0, cbAddr);

    ID3D12DescriptorHeap* heap = static_cast<ID3D12DescriptorHeap*>(mAnime4kDescriptorHeap);
    cmdList->SetDescriptorHeaps(1, &heap);
    cmdList->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());

    cmdList->Dispatch((mOutputWidth + 7) / 8, (mOutputHeight + 7) / 8, 1);
    
    // ========================================================================
    // GPU POST-PROCESSING (Palette Normalization, HDR, Black Crush, etc.)
    // ========================================================================
    
    // Always apply if GPU post-processing is initialized (palette normalization needs it!)
    if (mGpuPostProcessInitialized) {
        logDiagnostic("ANIME4K: Applying GPU post-processing (palette norm, HDR, filters)");
        logDiagnostic("  - HDR enabled: %d, Saturation: %.2f, Contrast: %.2f", 
                      mEnableSoftHDR, mHdrSaturation, mHdrContrast);
        logDiagnostic("  - Black crush: %.3f / %.2f", mBlackCrushThreshold, mBlackCrushStrength);
        logDiagnostic("  - Debanding: %d (%.2f), Edge: %d (%.2f)", 
                      mEnableDebanding, mDebandingStrength, mEnableEdgeSmoothing, mSmoothingStrength);
        
        // Keep output texture in UAV state for post-processing
        // No barrier needed - already in UAV state
        
        // Set up post-process pipeline
        cmdList->SetComputeRootSignature(static_cast<ID3D12RootSignature*>(mPostProcessRootSignature));
        cmdList->SetPipelineState(static_cast<ID3D12PipelineState*>(mPostProcessPipelineState));
        
        // Prepare comprehensive post-processing parameters
        struct {
            uint32_t resX, resY;
            float hdrSaturation;
            float hdrContrast;
            float blackCrushThreshold;
            float blackCrushStrength;
            uint32_t frameIndex;
            float debandingStrength;
            float edgeSmoothingStrength;
            float paletteNormalization;
            float colorGrading;
            float padding;
        } postParams;
        
        postParams.resX = mOutputWidth;
        postParams.resY = mOutputHeight;
        // If HDR is disabled, use neutral values (but still run shader for palette/debanding/edge)
        postParams.hdrSaturation = mEnableSoftHDR ? mHdrSaturation : 1.0f;
        postParams.hdrContrast = mEnableSoftHDR ? mHdrContrast : 1.0f;
        postParams.blackCrushThreshold = mEnableSoftHDR ? mBlackCrushThreshold : 0.0f;
        postParams.blackCrushStrength = mEnableSoftHDR ? mBlackCrushStrength : 0.0f;
        postParams.frameIndex = mFrameIndex++;
        postParams.debandingStrength = mEnableDebanding ? mDebandingStrength : 0.0f;
        postParams.edgeSmoothingStrength = mEnableEdgeSmoothing ? mSmoothingStrength : 0.0f;
        postParams.paletteNormalization = 0.8f; // Always enable 8-bit palette expansion
        postParams.colorGrading = mEnableSoftHDR ? 0.3f : 0.0f; // Only with HDR
        postParams.padding = 0;
        
        uint64_t cbAddr;
        if (gpuUploadConstantBuffer(&postParams, sizeof(postParams), &cbAddr)) {
            cmdList->SetComputeRootConstantBufferView(0, cbAddr);
            
            ID3D12DescriptorHeap* heap = static_cast<ID3D12DescriptorHeap*>(mAnime4kDescriptorHeap);
            cmdList->SetDescriptorHeaps(1, &heap);
            
            // Use UAV descriptor (second entry in heap, offset by handle size)
            ID3D12Device* device = gpuDeviceGetDevice();
            UINT handleSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = heap->GetGPUDescriptorHandleForHeapStart();
            uavHandle.ptr += handleSize; // Skip SRV, go to UAV
            
            cmdList->SetComputeRootDescriptorTable(1, uavHandle);
            
            // Dispatch post-process shader
            cmdList->Dispatch((mOutputWidth + 7) / 8, (mOutputHeight + 7) / 8, 1);
            
            logDiagnostic("ANIME4K: GPU post-processing dispatched");
        } else {
            logDiagnostic("ANIME4K: WARNING - Failed to upload post-process params");
        }
    }
    
    // Transition resources back to COMMON
    // Input: SRV -> COMMON
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    
    // Output: UAV -> COMMON
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    
    cmdList->ResourceBarrier(2, barriers);

    if (!gpuDeviceExecuteCommandList(cmdList)) return false;
    gpuDeviceWaitForGpu();
    
    // Download final processed result (no more CPU processing needed!)
    if (!gpuTextureDownload(mAnime4kOutputTexture, mOutputBuffer, mOutputWidth * mOutputHeight * 4)) return false;
    
    logDiagnostic("ANIME4K: Dispatch complete (GPU post-processing applied)");
    return true;
}

// ============================================================================
// Real-ESRGAN Implementation
// ============================================================================

bool UpscalerImpl::initMlGpuPipeline() {
    logDiagnostic("=== Initializing ML GPU pipeline ===");
    
    ID3D12Device* device = gpuDeviceGetDevice();
    if (!device) {
        logDiagnostic("ERROR: GPU device is null!");
        return false;
    }

    // 1. Create Planar RGB float buffer for ML input
    // Size: 640 * 480 * 3 channels * 4 bytes (float32)
    size_t bufferSize = (size_t)mInputWidth * mInputHeight * 3 * sizeof(float);
    logDiagnostic("Creating planar input buffer: %zu bytes (w=%d, h=%d)", bufferSize, mInputWidth, mInputHeight);
    
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    
    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Width = bufferSize;
    resDesc.Height = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc.Count = 1;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    
    ID3D12Resource* planarBuffer = nullptr;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&planarBuffer));
    
    if (FAILED(hr)) {
        logDiagnostic("Failed to create planar input buffer: 0x%08X", hr);
        return false;
    }
    logDiagnostic("Planar input buffer created successfully (addr=%p)", planarBuffer);
    mMlPlanarInputBuffer = planarBuffer;

    // 1b. Create Planar RGB float buffer for ML output
    // Size: (mInputWidth * 4) * (mInputHeight * 4) * 3 channels * 4 bytes (float32)
    int mlWidth = mInputWidth * 4;
    int mlHeight = mInputHeight * 4;
    size_t outputBufferSize = (size_t)mlWidth * mlHeight * 3 * sizeof(float);
    logDiagnostic("Creating planar output buffer: %zu bytes (w=%d, h=%d)", outputBufferSize, mlWidth, mlHeight);
    
    resDesc.Width = outputBufferSize;
    ID3D12Resource* planarOutputBuffer = nullptr;
    hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&planarOutputBuffer));
    
    if (FAILED(hr)) {
        logDiagnostic("Failed to create planar output buffer: 0x%08X", hr);
        return false;
    }
    mMlPlanarOutputBuffer = planarOutputBuffer;

    // 1c. Create Interleaved RGBA8 buffer for final output (GPU)
    // Size: 2560 * 1920 * 4 bytes
    size_t finalBufferSize = (size_t)mOutputWidth * mOutputHeight * 4;
    logDiagnostic("Creating final output buffer: %zu bytes", finalBufferSize);
    
    resDesc.Width = finalBufferSize;
    ID3D12Resource* finalOutputBuffer = nullptr;
    hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&finalOutputBuffer));
    
    if (FAILED(hr)) {
        logDiagnostic("Failed to create final output buffer: 0x%08X", hr);
        return false;
    }
    mMlFinalOutputBuffer = finalOutputBuffer;

    // 1d. Create Readback buffer for CPU access
    D3D12_HEAP_PROPERTIES readbackHeapProps = {};
    readbackHeapProps.Type = D3D12_HEAP_TYPE_READBACK;
    
    D3D12_RESOURCE_DESC readbackResDesc = {};
    readbackResDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackResDesc.Width = finalBufferSize;
    readbackResDesc.Height = 1;
    readbackResDesc.DepthOrArraySize = 1;
    readbackResDesc.MipLevels = 1;
    readbackResDesc.Format = DXGI_FORMAT_UNKNOWN;
    readbackResDesc.SampleDesc.Count = 1;
    readbackResDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    readbackResDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    
    ID3D12Resource* readbackBuffer = nullptr;
    hr = device->CreateCommittedResource(
        &readbackHeapProps, D3D12_HEAP_FLAG_NONE, &readbackResDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&readbackBuffer));
    
    if (FAILED(hr)) {
        logDiagnostic("Failed to create readback buffer: 0x%08X", hr);
        return false;
    }
    mMlReadbackBuffer = readbackBuffer;
    logDiagnostic("Readback buffer created (will be mapped after each frame)");

    // 2. Compile Preprocessing Shader
    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    
    logDiagnostic("Compiling preprocessing shader...");
    hr = D3DCompile(
        UPSCALER_PREPROCESS_SHADER,
        strlen(UPSCALER_PREPROCESS_SHADER),
        "PreprocessShader",
        nullptr, nullptr,
        "main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &shaderBlob, &errorBlob
    );
    
    if (FAILED(hr)) {
        logDiagnostic("Preprocess shader compile FAILED: 0x%08X", hr);
        if (errorBlob) logDiagnostic("Shader error: %s", (char*)errorBlob->GetBufferPointer());
        return false;
    }
    logDiagnostic("Preprocessing shader compiled successfully");

    // 3. Create Root Signature
    D3D12_ROOT_PARAMETER rootParams[3] = {};
    
    // CBV (Params)
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    // SRV (Input Texture)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    // UAV (Output Buffer) - use descriptor table, NOT direct virtual address
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 3;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    
    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
    logDiagnostic("Serializing root signature...");
    hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
    if (FAILED(hr)) {
        logDiagnostic("Root signature serialization failed: 0x%08X", hr);
        return false;
    }
    
    ID3D12RootSignature* rootSig = nullptr;
    logDiagnostic("Creating root signature...");
    hr = device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        logDiagnostic("Root signature creation failed: 0x%08X", hr);
        return false;
    }
    logDiagnostic("Root signature created (addr=%p)", rootSig);
    mMlPreprocessRootSig = rootSig;

    // 4. Create PSO
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSig;
    psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };
    
    ID3D12PipelineState* pso = nullptr;
    logDiagnostic("Creating compute PSO...");
    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        logDiagnostic("PSO creation failed: 0x%08X", hr);
        return false;
    }
    logDiagnostic("PSO created (addr=%p)", pso);
    mMlPreprocessPipeline = pso;

    // 5. Create Post-processing Pipeline
    logDiagnostic("Compiling ML post-processing shader...");
    hr = D3DCompile(
        UPSCALER_ML_POSTPROCESS_SHADER,
        strlen(UPSCALER_ML_POSTPROCESS_SHADER),
        "PostprocessShader",
        nullptr, nullptr,
        "main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &shaderBlob, &errorBlob
    );
    
    if (FAILED(hr)) {
        logDiagnostic("Postprocess shader compile FAILED: 0x%08X", hr);
        if (errorBlob) logDiagnostic("Shader error: %s", (char*)errorBlob->GetBufferPointer());
        return false;
    }

    // Post-process Root Signature
    D3D12_ROOT_PARAMETER postRootParams[3] = {};
    
    // CBV (Params) - Use 32-bit constants for performance and simplicity
    postRootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    postRootParams[0].Constants.ShaderRegister = 0;
    postRootParams[0].Constants.RegisterSpace = 0;
    postRootParams[0].Constants.Num32BitValues = 4; // outputWidth, outputHeight, mlWidth, mlHeight
    postRootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    // SRV (Input Planar Buffer) -> t0 (Descriptor Table)
    D3D12_DESCRIPTOR_RANGE postSrvRange = {};
    postSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    postSrvRange.NumDescriptors = 1;
    postSrvRange.BaseShaderRegister = 0;
    postSrvRange.RegisterSpace = 0;
    postSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    postRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    postRootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    postRootParams[1].DescriptorTable.pDescriptorRanges = &postSrvRange;
    postRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // UAV (Output Final Buffer) -> u0 (Descriptor Table)
    D3D12_DESCRIPTOR_RANGE postUavRange = {};
    postUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    postUavRange.NumDescriptors = 1;
    postUavRange.BaseShaderRegister = 0;
    postUavRange.RegisterSpace = 0;
    postUavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    postRootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    postRootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    postRootParams[2].DescriptorTable.pDescriptorRanges = &postUavRange;
    postRootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    D3D12_ROOT_SIGNATURE_DESC postRootSigDesc = {};
    postRootSigDesc.NumParameters = 3;
    postRootSigDesc.pParameters = postRootParams;
    postRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    
    hr = D3D12SerializeRootSignature(&postRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
    if (FAILED(hr)) return false;
    
    ID3D12RootSignature* postRootSig = nullptr;
    hr = device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&postRootSig));
    if (FAILED(hr)) return false;
    mMlPostprocessRootSig = postRootSig;

    D3D12_COMPUTE_PIPELINE_STATE_DESC postPsoDesc = {};
    postPsoDesc.pRootSignature = postRootSig;
    postPsoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };
    
    ID3D12PipelineState* postPso = nullptr;
    hr = device->CreateComputePipelineState(&postPsoDesc, IID_PPV_ARGS(&postPso));
    if (FAILED(hr)) return false;
    mMlPostprocessPipeline = postPso;

    logDiagnostic("=== ML GPU pipeline initialized successfully ===");
    return true;
}

bool UpscalerImpl::shutdownMlGpuPipeline() {
    logDiagnostic("Shutting down ML GPU pipeline...");
    if (mMlPlanarInputBuffer) {
        static_cast<ID3D12Resource*>(mMlPlanarInputBuffer)->Release();
        mMlPlanarInputBuffer = nullptr;
    }
    if (mMlPlanarOutputBuffer) {
        static_cast<ID3D12Resource*>(mMlPlanarOutputBuffer)->Release();
        mMlPlanarOutputBuffer = nullptr;
    }
    if (mMlFinalOutputBuffer) {
        static_cast<ID3D12Resource*>(mMlFinalOutputBuffer)->Release();
        mMlFinalOutputBuffer = nullptr;
    }
    if (mMlReadbackBuffer) {
        static_cast<ID3D12Resource*>(mMlReadbackBuffer)->Unmap(0, nullptr);
        static_cast<ID3D12Resource*>(mMlReadbackBuffer)->Release();
        mMlReadbackBuffer = nullptr;
        mMlReadbackMappedPtr = nullptr;
    }
    
    if (mMlPreprocessPipeline) {
        static_cast<ID3D12PipelineState*>(mMlPreprocessPipeline)->Release();
        mMlPreprocessPipeline = nullptr;
    }
    if (mMlPreprocessRootSig) {
        static_cast<ID3D12RootSignature*>(mMlPreprocessRootSig)->Release();
        mMlPreprocessRootSig = nullptr;
    }
    if (mMlPostprocessPipeline) {
        static_cast<ID3D12PipelineState*>(mMlPostprocessPipeline)->Release();
        mMlPostprocessPipeline = nullptr;
    }
    if (mMlPostprocessRootSig) {
        static_cast<ID3D12RootSignature*>(mMlPostprocessRootSig)->Release();
        mMlPostprocessRootSig = nullptr;
    }
    return true;
}

bool UpscalerImpl::initRealEsrgan() {
    logDiagnostic("=== REAL-ESRGAN INIT START ===");
    
    if (!checkGpuReadiness()) {
        logDiagnostic("GPU not ready, cannot initialize ML upscaler");
        return false;
    }
    
    if (!mUpscalerML) {
        logDiagnostic("Creating UpscalerML instance");
        mUpscalerML = std::make_unique<UpscalerML>();
    }
    
    // Initialize ML engine with model path
    char* basePath = SDL_GetBasePath();
    std::string basePathStr = basePath ? std::string(basePath) : "";
    if (basePath) {
        SDL_free(basePath);
    }
    
    // Use configured model file
    std::string modelPath = basePathStr + mMlModelFile;
    logDiagnostic("Attempting to load ML model from: %s", modelPath.c_str());
    
    // Create logger callback to bridge ML logs to our log file
    auto logger = [this](const char* msg) {
        this->logDiagnostic("[ML] %s", msg);
    };
    
    // Try primary model
    bool initSuccess = mUpscalerML->init(gpuDeviceGetDevice(), modelPath, logger);
    
    if (!initSuccess) {
        setError("Failed to initialize ML model. Checked paths: %s", modelPath.c_str());
        logDiagnostic("ML model initialization FAILED");
        return false;
    }
    
    logDiagnostic("ML model loaded successfully!");
    
    // Allocate cache buffer for 4x output (Real-ESRGAN is 4x)
    if (mMlCachedOutput) internal_free(mMlCachedOutput);
    mMlCachedOutput = static_cast<uint32_t*>(internal_malloc(mInputWidth * 4 * mInputHeight * 4 * sizeof(uint32_t)));
    
    // Create GPU textures for ML upscaler
    logDiagnostic("Creating GPU textures for ML upscaler");
    if (!createAnime4kTextures()) {
        logDiagnostic("Failed to create textures");
        return false;
    }
    
    logDiagnostic("Creating descriptor heap");
    if (!createAnime4kDescriptorHeap()) {
        logDiagnostic("Failed to create descriptor heap");
        return false;
    }
    
    logDiagnostic("Creating descriptors");
    if (!createAnime4kDescriptors()) {
        logDiagnostic("Failed to create descriptors");
        return false;
    }
    
    logDiagnostic("Creating command list");
    if (!createAnime4kCommandList()) {
        logDiagnostic("Failed to create command list");
        return false;
    }
    
    // Initialize GPU pipeline for preprocessing
    logDiagnostic("Initializing ML GPU pipeline (Preprocessing)");
    if (!initMlGpuPipeline()) {
        logDiagnostic("Failed to initialize ML GPU pipeline");
        return false;
    }
    
    // Now that the planar buffer is created, create its UAV descriptor
    logDiagnostic("Creating UAV descriptor for planar buffer");
    ID3D12Device* device = gpuDeviceGetDevice();
    ID3D12DescriptorHeap* heap = static_cast<ID3D12DescriptorHeap*>(mAnime4kDescriptorHeap);
    if (heap && mMlPlanarInputBuffer && device) {
        UINT handleSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
        
        // Offset 1: UAV for Planar Input (Preprocess)
        handle.ptr += handleSize;
        ID3D12Resource* planarRes = static_cast<ID3D12Resource*>(mMlPlanarInputBuffer);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = mInputWidth * mInputHeight * 3;  // Total float count
        uavDesc.Buffer.StructureByteStride = sizeof(float);
        device->CreateUnorderedAccessView(planarRes, nullptr, &uavDesc, handle);
        logDiagnostic("Created UAV descriptor for planar buffer at offset 1");

        // Offset 2: SRV for Planar Output (Postprocess)
        handle.ptr += handleSize;
        ID3D12Resource* planarOutRes = static_cast<ID3D12Resource*>(mMlPlanarOutputBuffer);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = mInputWidth * 4 * mInputHeight * 4 * 3; // ML Output size
        srvDesc.Buffer.StructureByteStride = sizeof(float);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(planarOutRes, &srvDesc, handle);
        logDiagnostic("Created SRV descriptor for planar output buffer at offset 2");

        // Offset 3: UAV for Final Output (Postprocess)
        handle.ptr += handleSize;
        ID3D12Resource* finalOutRes = static_cast<ID3D12Resource*>(mMlFinalOutputBuffer);
        D3D12_UNORDERED_ACCESS_VIEW_DESC finalUavDesc = {};
        finalUavDesc.Format = DXGI_FORMAT_R32_UINT; // Typed Buffer
        finalUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        finalUavDesc.Buffer.FirstElement = 0;
        finalUavDesc.Buffer.NumElements = mOutputWidth * mOutputHeight;
        finalUavDesc.Buffer.StructureByteStride = 0; // Must be 0 for Typed Buffer
        finalUavDesc.Buffer.CounterOffsetInBytes = 0;
        finalUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        device->CreateUnorderedAccessView(finalOutRes, nullptr, &finalUavDesc, handle);
        logDiagnostic("Created UAV descriptor for final output buffer at offset 3");

    } else {
        logDiagnostic("WARNING: Could not create planar buffer UAV (heap=%p, buffer=%p, device=%p)", 
                      heap, mMlPlanarInputBuffer, device);
    }
    
    logDiagnostic("=== REAL-ESRGAN INIT SUCCESS ===");
    return true;
}

bool UpscalerImpl::shutdownRealEsrgan() {
    mUpscalerML.reset();
    
    // Clean up ML GPU pipeline
    shutdownMlGpuPipeline();
    
    // Clean up ML cache
    if (mMlCachedOutput) {
        internal_free(mMlCachedOutput);
        mMlCachedOutput = nullptr;
    }
    
    // Clean up shared resources
    if (mAnime4kOutputTexture.resource) { gpuTextureRelease(mAnime4kOutputTexture); mAnime4kOutputTexture = {}; }
    if (mAnime4kInputTexture.resource) { gpuTextureRelease(mAnime4kInputTexture); mAnime4kInputTexture = {}; }
    if (mAnime4kCommandList) { static_cast<ID3D12GraphicsCommandList*>(mAnime4kCommandList)->Release(); mAnime4kCommandList = nullptr; }
    if (mAnime4kCommandAllocator) { static_cast<ID3D12CommandAllocator*>(mAnime4kCommandAllocator)->Release(); mAnime4kCommandAllocator = nullptr; }
    
    return true;
}

bool UpscalerImpl::dispatchRealEsrgan() {
    logDiagnostic("=== REAL-ESRGAN DISPATCH START ===");
    uint64_t dispatchStart = SDL_GetPerformanceCounter();
    
    if (!mUpscalerML) {
        logDiagnostic("ERROR: mUpscalerML is null!");
        return false;
    }
    
    if (!mMlPlanarInputBuffer) {
        logDiagnostic("ERROR: Planar input buffer not created during init!");
        return false;
    }
    
    // 1. Upload input to GPU
    if (!mAnime4kInputTexture.resource) {
        logDiagnostic("ERROR: Input texture not created!");
        return false;
    }
    logDiagnostic("Uploading input texture (%dx%d)", mInputWidth, mInputHeight);
    gpuTextureUpload(mAnime4kInputTexture, mInputBuffer, mInputWidth * mInputHeight * 4);

    // 2. Run Preprocessing Shader (Blur + HDR)
    logDiagnostic("Starting preprocessing shader dispatch");
    ID3D12GraphicsCommandList* cmdList = static_cast<ID3D12GraphicsCommandList*>(mAnime4kCommandList);
    ID3D12CommandAllocator* allocator = static_cast<ID3D12CommandAllocator*>(mAnime4kCommandAllocator);
    
    if (!cmdList || !allocator) {
        logDiagnostic("ERROR: Command list or allocator is null!");
        return false;
    }
    
    allocator->Reset();
    if (!mMlPreprocessPipeline) {
        logDiagnostic("ERROR: Preprocessing PSO not created!");
        return false;
    }
    cmdList->Reset(allocator, static_cast<ID3D12PipelineState*>(mMlPreprocessPipeline));
    
    cmdList->SetComputeRootSignature(static_cast<ID3D12RootSignature*>(mMlPreprocessRootSig));
    
    // Set Constants
    struct {
        uint32_t width, height;
        float blurStrength;
        float hdrSaturation;
        float hdrContrast;
        float padding[2];
    } params;
    params.width = mInputWidth;
    params.height = mInputHeight;
    params.blurStrength = 0.5f; // Default blur
    params.hdrSaturation = mHdrSaturation;
    params.hdrContrast = mHdrContrast;
    
    uint32_t paramCount = sizeof(params) / 4;
    logDiagnostic("Setting compute root constants (count=%u, size=%zu)", paramCount, sizeof(params));
    cmdList->SetComputeRoot32BitConstants(0, paramCount, &params, 0);
    
    // Set SRV (Input)
    ID3D12DescriptorHeap* heap = static_cast<ID3D12DescriptorHeap*>(mAnime4kDescriptorHeap);
    if (!heap) {
        logDiagnostic("ERROR: Descriptor heap is null!");
        return false;
    }
    cmdList->SetDescriptorHeaps(1, &heap);
    
    // SRV descriptor is at offset 0 in the heap
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = heap->GetGPUDescriptorHandleForHeapStart();
    logDiagnostic("Setting SRV descriptor table");
    cmdList->SetComputeRootDescriptorTable(1, srvHandle);
    
    // Set UAV (Output Planar Buffer) - need to create a UAV for the buffer
    // For now, we'll use the second handle in the heap (offset by one)
    ID3D12Device* device = gpuDeviceGetDevice();
    UINT handleSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = srvHandle;
    uavHandle.ptr += handleSize;
    logDiagnostic("Setting UAV descriptor table");
    cmdList->SetComputeRootDescriptorTable(2, uavHandle);
    
    uint32_t groupX = (mInputWidth + 15) / 16;
    uint32_t groupY = (mInputHeight + 15) / 16;
    logDiagnostic("Dispatching %ux%u thread groups", groupX, groupY);
    cmdList->Dispatch(groupX, groupY, 1);
    
    // Barrier to ensure preprocessing is done before ML
    // Also transition Output Buffer to UAV for ML
    D3D12_RESOURCE_BARRIER preMlBarriers[2] = {};
    
    // 1. UAV Barrier for Input Buffer (ensure writes are visible)
    preMlBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    preMlBarriers[0].UAV.pResource = static_cast<ID3D12Resource*>(mMlPlanarInputBuffer);
    
    // 2. Transition Output Buffer to UAV (from COMMON)
    preMlBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preMlBarriers[1].Transition.pResource = static_cast<ID3D12Resource*>(mMlPlanarOutputBuffer);
    preMlBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    preMlBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    preMlBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    
    logDiagnostic("Adding Pre-ML barriers");
    cmdList->ResourceBarrier(2, preMlBarriers);
    
    logDiagnostic("Closing command list and executing");
    HRESULT closeHr = cmdList->Close();
    if (FAILED(closeHr)) {
        logDiagnostic("ERROR: Failed to close command list (HRESULT=0x%08X)", closeHr);
        return false;
    }
    
    ID3D12CommandQueue* queue = gpuDeviceGetCommandQueue();
    if (!queue) {
        logDiagnostic("ERROR: Command queue is null!");
        return false;
    }
    ID3D12CommandList* lists[] = { cmdList };
    queue->ExecuteCommandLists(1, lists);
    logDiagnostic("Preprocessing command list executed, waiting for GPU");
    
    // Wait for GPU to ensure preprocessing is done before ML starts
    gpuDeviceWaitForGpu();
    logDiagnostic("GPU preprocessing complete");

    // 3. Run ML Inference on GPU (GPU-to-GPU)
    logDiagnostic("Running ML inference on GPU (GPU-to-GPU pipeline)");
    bool mlSuccess = mUpscalerML->dispatchGpuToGpu(
        static_cast<ID3D12Resource*>(mMlPlanarInputBuffer), 
        static_cast<ID3D12Resource*>(mMlPlanarOutputBuffer), 
        mInputWidth, mInputHeight);
    
    if (!mlSuccess) {
        logDiagnostic("!!! ML DISPATCH FAILED !!!");
        return false;
    }

    // 4. Run Post-processing Shader (Planar to Interleaved)
    logDiagnostic("Starting post-processing shader dispatch");
    allocator->Reset();
    if (!mMlPostprocessPipeline) {
        logDiagnostic("ERROR: Postprocessing PSO not created!");
        return false;
    }
    cmdList->Reset(allocator, static_cast<ID3D12PipelineState*>(mMlPostprocessPipeline));
    
    // Transition Output Buffer from UAV (ML) to SRV (PostProcess)
    D3D12_RESOURCE_BARRIER postMlBarrier = {};
    postMlBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postMlBarrier.Transition.pResource = static_cast<ID3D12Resource*>(mMlPlanarOutputBuffer);
    postMlBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    postMlBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    postMlBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &postMlBarrier);

    cmdList->SetComputeRootSignature(static_cast<ID3D12RootSignature*>(mMlPostprocessRootSig));
    
    // Set Constants
    struct {
        uint32_t outputWidth, outputHeight;
        uint32_t mlWidth, mlHeight;
    } postParams;
    postParams.outputWidth = mOutputWidth;
    postParams.outputHeight = mOutputHeight;
    postParams.mlWidth = mInputWidth * 4;
    postParams.mlHeight = mInputHeight * 4;
    cmdList->SetComputeRoot32BitConstants(0, 4, &postParams, 0);
    
    cmdList->SetDescriptorHeaps(1, &heap);
    
    // Get handle size again
    device = gpuDeviceGetDevice();
    handleSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE baseHandle = heap->GetGPUDescriptorHandleForHeapStart();
    
    // Set SRV (Planar Output) -> t0 (Offset 2)
    D3D12_GPU_DESCRIPTOR_HANDLE postSrvHandle = baseHandle;
    postSrvHandle.ptr += handleSize * 2;
    cmdList->SetComputeRootDescriptorTable(1, postSrvHandle);

    // Set UAV (Final Output) -> u0 (Offset 3)
    D3D12_GPU_DESCRIPTOR_HANDLE postUavHandle = baseHandle;
    postUavHandle.ptr += handleSize * 3;
    cmdList->SetComputeRootDescriptorTable(2, postUavHandle);
    
    uint32_t postGroupX = (mOutputWidth + 7) / 8;
    uint32_t postGroupY = (mOutputHeight + 7) / 8;
    
    // Transition Final Output Buffer from COMMON to UAV for writing
    D3D12_RESOURCE_BARRIER finalOutputTransBarrier = {};
    finalOutputTransBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    finalOutputTransBarrier.Transition.pResource = static_cast<ID3D12Resource*>(mMlFinalOutputBuffer);
    finalOutputTransBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    finalOutputTransBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    finalOutputTransBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &finalOutputTransBarrier);

    logDiagnostic("Dispatching PostProcess: %dx%d groups (Out: %dx%d)", postGroupX, postGroupY, mOutputWidth, mOutputHeight);
    cmdList->Dispatch(postGroupX, postGroupY, 1);
    
    // UAV Barrier to ensure writes are complete before state transition
    D3D12_RESOURCE_BARRIER postBarrier = {};
    postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    postBarrier.UAV.pResource = static_cast<ID3D12Resource*>(mMlFinalOutputBuffer);
    cmdList->ResourceBarrier(1, &postBarrier);
    
    // Copy to Readback buffer
    D3D12_RESOURCE_BARRIER copyBarriers[2] = {};
    copyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copyBarriers[0].Transition.pResource = static_cast<ID3D12Resource*>(mMlFinalOutputBuffer);
    copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    copyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    
    copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copyBarriers[1].Transition.pResource = static_cast<ID3D12Resource*>(mMlReadbackBuffer);
    copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST; // No change
    copyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    
    cmdList->ResourceBarrier(1, &copyBarriers[0]);
    
    // Use CopyResource for full buffer copy
    cmdList->CopyResource(static_cast<ID3D12Resource*>(mMlReadbackBuffer), static_cast<ID3D12Resource*>(mMlFinalOutputBuffer));
    
    // Transition back
    copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    
    // Also transition Planar Output Buffer back to COMMON (from SRV)
    copyBarriers[1].Transition.pResource = static_cast<ID3D12Resource*>(mMlPlanarOutputBuffer);
    copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    
    cmdList->ResourceBarrier(2, copyBarriers);
    
    cmdList->Close();
    queue->ExecuteCommandLists(1, lists);
    gpuDeviceWaitForGpu();
    
    // Now map the readback buffer to read the results
    ID3D12Resource* readbackRes = static_cast<ID3D12Resource*>(mMlReadbackBuffer);
    if (mMlReadbackMappedPtr) {
        readbackRes->Unmap(0, nullptr);
        mMlReadbackMappedPtr = nullptr;
    }
    
    D3D12_RANGE readRange = { 0, (SIZE_T)(mOutputWidth * mOutputHeight * 4) };
    HRESULT mapHr = readbackRes->Map(0, &readRange, &mMlReadbackMappedPtr);
    if (FAILED(mapHr)) {
        logDiagnostic("ERROR: Failed to map readback buffer for reading: 0x%08X", mapHr);
        return false;
    }
    
    // DEBUG: Check first pixel value
    if (mMlReadbackMappedPtr) {
        uint32_t* pixels = static_cast<uint32_t*>(mMlReadbackMappedPtr);
        uint32_t centerIdx = (mOutputHeight / 2) * mOutputWidth + (mOutputWidth / 2);
        logDiagnostic("DEBUG: Readback pixel[0]: 0x%08X, pixel[center]: 0x%08X", pixels[0], pixels[centerIdx]);
        
        // Copy to output buffer
        if (mOutputBuffer) {
            memcpy(mOutputBuffer, mMlReadbackMappedPtr, mOutputWidth * mOutputHeight * 4);
            logDiagnostic("DEBUG: OutputBuffer pixel[0]: 0x%08X", mOutputBuffer[0]);
        } else {
            logDiagnostic("ERROR: mOutputBuffer is NULL!");
        }
    }
    
    uint64_t dispatchEnd = SDL_GetPerformanceCounter();
    double totalMs = (dispatchEnd - dispatchStart) * 1000.0 / SDL_GetPerformanceFrequency();
    logDiagnostic("=== REAL-ESRGAN ZERO-COPY DISPATCH COMPLETE (%.2f ms) ===", totalMs);
    return true;
}

// ============================================================================
// Public API
// ============================================================================

bool UpscalerImpl::setQuality(UpscalerQuality quality) { mQuality = quality; return true; }
bool UpscalerImpl::setSharpness(float sharpness) { mSharpness = std::clamp(sharpness, 0.0f, 1.0f); return true; }

// Public API Implementation
int upscalerInit(int iw, int ih, int ow, int oh, UpscalerMode m) { return upscalerGetImpl()->init(iw, ih, ow, oh, m) ? 0 : -1; }
int upscalerReconfigureOutput(int ow, int oh) { return upscalerGetImpl()->reconfigureOutput(ow, oh) ? 0 : -1; }
void upscalerShutdown() { upscalerGetImpl()->shutdown(); }
int upscalerSetIndexedInput(const unsigned char* buf, const uint32_t* pal) { return upscalerGetImpl()->setIndexedInput(buf, pal) ? 0 : -1; }
int upscalerSetRgbaInput(const uint32_t* buf) { return upscalerGetImpl()->setRgbaInput(buf) ? 0 : -1; }
int upscalerDispatch() { return upscalerGetImpl()->dispatch() ? 0 : -1; }
const uint32_t* upscalerGetOutputBuffer() { return upscalerGetImpl()->getOutputBuffer(); }
int upscalerGetOutputPitch() { return upscalerGetImpl()->getOutputPitch(); }
void upscalerGetOutputDimensions(int& w, int& h) { upscalerGetImpl()->getOutputDimensions(w, h); }
UpscalerState upscalerGetState() { return upscalerGetImpl()->getState(); }
int upscalerSetQuality(UpscalerQuality q) { return upscalerGetImpl()->setQuality(q) ? 0 : -1; }
UpscalerMode upscalerGetConfiguredMode() { return upscalerGetImpl()->getConfiguredMode(); }
int upscalerSetSharpness(float s) { return upscalerGetImpl()->setSharpness(s) ? 0 : -1; }
void upscalerSetMotionVectorsEnabled(bool e) { upscalerGetImpl()->setMotionVectorsEnabled(e); }
int upscalerSetFilterParams(float e, float c, float s, float k) { upscalerGetImpl()->setFilterParams(e, c, s, k); return 0; }
void upscalerSetVerboseLogging(bool e) { upscalerGetImpl()->setVerboseLogging(e); }
void upscalerReloadConfig() { upscalerGetImpl()->reloadConfig(); }
bool upscalerIsAvailable() { return upscalerGetImpl()->isAvailable(); }
const char* upscalerGetLastError() { return upscalerGetImpl()->getLastError(); }

} // namespace fallout
