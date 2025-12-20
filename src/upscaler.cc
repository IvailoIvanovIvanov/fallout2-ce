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
#include "upscaler_filters.h"
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

    const uint32_t* getOutputBuffer() const { return mOutputBuffer; }
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
    bool dispatchIntegerScale(int scaleFactor);
    bool initAnime4k();
    bool shutdownAnime4k();
    bool dispatchAnime4k();

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

    // State variables
    UpscalerState mState = UpscalerState::STATE_UNINITIALIZED;
    UpscalerMode mMode = UpscalerMode::NONE;
    UpscalerMode mConfiguredMode = UpscalerMode::INTEGER_3X;
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
    
    if (mUpscaleLog == nullptr && !mLogFilePath.empty()) {
        mUpscaleLog = fopen(mLogFilePath.c_str(), "a");
    }
    
    if (mUpscaleLog != nullptr) {
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
    int modeValue = 2; // Default to INTEGER_3X
    if (configGetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_MODE_KEY, &modeValue)) {
        // Clamp to valid range [0-4]
        if (modeValue < 0 || modeValue > 4) {
            logDiagnostic("Invalid mode %d in config, defaulting to INTEGER_3X", modeValue);
            modeValue = 2; // INTEGER_3X
        }
        mConfiguredMode = static_cast<UpscalerMode>(modeValue);
    } else {
        mConfiguredMode = UpscalerMode::INTEGER_3X;
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
        mVerboseLogging = false; // Force disabled for performance
    }
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
            logDiagnostic("Anime4K init failed, falling back to INTEGER_3X");
            mMode = UpscalerMode::INTEGER_3X;
            // Re-allocate buffers if needed (though they should be fine)
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
    } else {
        if (!allocateOutputBuffer(outputWidth, outputHeight)) return false;
    }
    return true;
}

void UpscalerImpl::shutdown() {
    if (mMode == UpscalerMode::ANIME4K) {
        shutdownAnime4k();
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
        case UpscalerMode::INTEGER_2X:
            logDiagnostic("Dispatching INTEGER_2X");
            return dispatchIntegerScale(2);
        case UpscalerMode::INTEGER_3X:
            logDiagnostic("Dispatching INTEGER_3X");
            return dispatchIntegerScale(3);
        case UpscalerMode::INTEGER_4X:
            logDiagnostic("Dispatching INTEGER_4X");
            return dispatchIntegerScale(4);
        default:
            logDiagnostic("Mode NONE/Unknown - passthrough");
            return true; // Pass-through or None
    }
}

// ============================================================================
// Integer Scaling
// ============================================================================

bool UpscalerImpl::dispatchIntegerScale(int scaleFactor) {
    logDiagnostic("INTEGER_SCALE: factor=%d, input=%dx%d, output=%dx%d", 
                  scaleFactor, mInputWidth, mInputHeight, mOutputWidth, mOutputHeight);
    
    if (!mInputBuffer || !mOutputBuffer) {
        logDiagnostic("ERROR: Buffers null - input=%p, output=%p", mInputBuffer, mOutputBuffer);
        return false;
    }

    // Kuwahara Filter (Pre-scaling)
    uint32_t* sourceBuffer = mInputBuffer;
    if (mEnableKuwahara) {
        if (!mTempBuffer) {
            mTempBuffer = static_cast<uint32_t*>(internal_malloc(mInputWidth * mInputHeight * sizeof(uint32_t)));
        }
        if (mTempBuffer && filterApplyKuwahara(mInputBuffer, mTempBuffer, mInputWidth, mInputHeight, mKuwaharaRadius)) {
            sourceBuffer = mTempBuffer;
        }
    }

    // Scaling & Letterboxing
    int scaledWidth = mInputWidth * scaleFactor;
    int scaledHeight = mInputHeight * scaleFactor;
    int offsetX = (mOutputWidth - scaledWidth) / 2;
    int offsetY = (mOutputHeight - scaledHeight) / 2;

    memset(mOutputBuffer, 0, mOutputWidth * mOutputHeight * sizeof(uint32_t));

    for (int y = 0; y < mInputHeight; y++) {
        for (int x = 0; x < mInputWidth; x++) {
            uint32_t pixel = sourceBuffer[y * mInputWidth + x];
            for (int dy = 0; dy < scaleFactor; dy++) {
                int outY = offsetY + (y * scaleFactor + dy);
                if (outY < 0 || outY >= mOutputHeight) continue;
                for (int dx = 0; dx < scaleFactor; dx++) {
                    int outX = offsetX + (x * scaleFactor + dx);
                    if (outX < 0 || outX >= mOutputWidth) continue;
                    mOutputBuffer[outY * mOutputWidth + outX] = pixel;
                }
            }
        }
    }

    // Post-processing
    FilterConfig filterConfig;
    filterConfig.type = FilterType::MINIMAL;
    filterConfig.enableDebanding = mEnableDebanding;
    filterConfig.debandingStrength = mDebandingStrength;
    filterConfig.frameIndex = mFrameIndex++;
    filterConfig.enableEdgeSmoothing = mEnableEdgeSmoothing;
    filterConfig.smoothingStrength = mSmoothingStrength;
    filterConfig.enableSoftHDR = mEnableSoftHDR;
    filterConfig.hdrStrength = mHdrStrength;
    filterConfig.hdrSaturation = mHdrSaturation;
    filterConfig.hdrContrast = mHdrContrast;
    
    filterApplyPostProcessing(mOutputBuffer, mOutputWidth, mOutputHeight, mOutputWidth * sizeof(uint32_t), filterConfig);
    
    return true;
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
    heapDesc.NumDescriptors = 2;
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
    UINT handleSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();

    // SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(gpuTextureGetResource(mAnime4kInputTexture), &srvDesc, handle);

    // UAV
    handle.ptr += handleSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(gpuTextureGetResource(mAnime4kOutputTexture), nullptr, &uavDesc, handle);
    
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

    mAnime4kInitialized = true;
    return true;
}

bool UpscalerImpl::shutdownAnime4k() {
    if (!mAnime4kInitialized) return true;
    
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

bool UpscalerImpl::dispatchAnime4k() {
    logDiagnostic("ANIME4K: initialized=%d, inputBuf=%p, outputBuf=%p", 
                  mAnime4kInitialized, mInputBuffer, mOutputBuffer);
    
    if (!mAnime4kInitialized || !mInputBuffer || !mOutputBuffer) {
        logDiagnostic("ANIME4K: Falling back to INTEGER_3X");
        return dispatchIntegerScale(3);
    }

    ID3D12GraphicsCommandList* cmdList = static_cast<ID3D12GraphicsCommandList*>(mAnime4kCommandList);
    ID3D12CommandAllocator* allocator = static_cast<ID3D12CommandAllocator*>(mAnime4kCommandAllocator);
    
    allocator->Reset();
    cmdList->Reset(allocator, static_cast<ID3D12PipelineState*>(mAnime4kPipelineState));
    
    if (!gpuTextureUpload(mAnime4kInputTexture, mInputBuffer, mInputWidth * mInputHeight * 4)) return dispatchIntegerScale(3);

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
    if (!gpuUploadConstantBuffer(&params, sizeof(params), &cbAddr)) return dispatchIntegerScale(3);
    cmdList->SetComputeRootConstantBufferView(0, cbAddr);

    ID3D12DescriptorHeap* heap = static_cast<ID3D12DescriptorHeap*>(mAnime4kDescriptorHeap);
    cmdList->SetDescriptorHeaps(1, &heap);
    cmdList->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());

    cmdList->Dispatch((mOutputWidth + 7) / 8, (mOutputHeight + 7) / 8, 1);
    
    // Transition resources back to COMMON
    // Input: SRV -> COMMON
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    
    // Output: UAV -> COMMON
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    
    cmdList->ResourceBarrier(2, barriers);

    if (!gpuDeviceExecuteCommandList(cmdList)) return dispatchIntegerScale(3);
    gpuDeviceWaitForGpu();
    
    if (!gpuTextureDownload(mAnime4kOutputTexture, mOutputBuffer, mOutputWidth * mOutputHeight * 4)) return dispatchIntegerScale(3);
    
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
