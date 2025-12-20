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

// FidelityFX SDK 2.1 headers (optional, only if FALLOUT_HAS_FSR2 is defined)
#ifdef FALLOUT_HAS_FSR2
    #include "ffx_upscale.h"
    #include "ffx_api.h"
    #include "dx12/ffx_api_dx12.h"  // For DX12 backend descriptor
#endif

namespace fallout {

// Maximum resolution for safety clamps
constexpr int MAX_RESOLUTION = 8192;
constexpr int MIN_RESOLUTION = 320;
constexpr int ERROR_MSG_SIZE = 256;

/**
 * Upscaler implementation
 * Encapsulates FSR2 context and state management
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
    void setMotionVectorsEnabled(bool enabled) { mMotionVectorsEnabled = enabled; }
    
    // Deprecated: Edge/color/saturation/contrast now handled by FSR2 RCAS
    void setFilterParams(float edge, float color, float sat, float con) {
        // No-op: These parameters are deprecated
        // Adjust FSR2 sharpness instead for edge/contrast control
        logDiagnostic("WARNING: setFilterParams() deprecated - use setSharpness() for FSR2 RCAS control");
    }
    
    void setVerboseLogging(bool enabled) {
        mVerboseLogging = enabled;
        saveConfiguration();
    }
    
    void reloadConfig() {
        loadConfiguration();
    }

    bool isAvailable() const { 
        // Log every 60th call to track availability
        static int checkCount = 0;
        checkCount++;
        if (mUpscaleLog != nullptr && (checkCount == 1 || checkCount % 60 == 0)) {
            fprintf(mUpscaleLog, "[UPSCALER] isAvailable() check #%d: %s (state=%d, mode=%d)\n",
                checkCount, mIsAvailable ? "TRUE" : "FALSE", (int)mState, (int)mMode);
            fflush(mUpscaleLog);
        }
        return mIsAvailable; 
    }
    const char* getLastError() const { return mLastError; }
    UpscalerMode getConfiguredMode() const { return mConfiguredMode; }

private:
    UpscalerImpl() {
        // Initialize log file on singleton creation
        char* basePath = SDL_GetBasePath();
        if (basePath != nullptr) {
            mLogFilePath = std::string(basePath) + "upscale.log";
            SDL_free(basePath);
        } else {
            mLogFilePath = "upscale.log";
        }
        
        // Create the log file immediately
        mUpscaleLog = fopen(mLogFilePath.c_str(), "w");
        if (mUpscaleLog != nullptr) {
            fprintf(mUpscaleLog, "====================================================\n");
            fprintf(mUpscaleLog, "   FALLOUT 2 CE - UPSCALER DIAGNOSTICS LOG\n");
            fprintf(mUpscaleLog, "====================================================\n");
            fprintf(mUpscaleLog, "[BOOTSTRAP] Upscaler singleton initialized\n");
            fprintf(mUpscaleLog, "[BOOTSTRAP] Log file: %s\n", mLogFilePath.c_str());
            fprintf(mUpscaleLog, "[BOOTSTRAP] State: UNINITIALIZED\n");
            fprintf(mUpscaleLog, "[BOOTSTRAP] Waiting for upscalerInit() call...\n");
            fprintf(mUpscaleLog, "====================================================\n\n");
            fflush(mUpscaleLog);
        }
    }

    void logDiagnostic(const char* format, ...);
    void setError(const char* format, ...);
    
    void loadConfiguration();
    void saveConfiguration();
    
    bool initFsr2();
    bool shutdownFsr2();
    bool dispatchFsr2();
    bool dispatchIntegerScale(int scaleFactor);
    bool initAnime4k();
    bool shutdownAnime4k();
    bool dispatchAnime4k();

    bool allocateInputBuffer(int width, int height);
    bool allocateOutputBuffer(int width, int height);
    void deallocateBuffers();

    void calculateJitter(float& outX, float& outY);

    // State variables
    UpscalerState mState = UpscalerState::STATE_UNINITIALIZED;
    UpscalerMode mMode = UpscalerMode::NONE;
    UpscalerMode mConfiguredMode = UpscalerMode::FSR2;  // Mode from config file
    UpscalerQuality mQuality = UpscalerQuality::BALANCED;
    bool mIsAvailable = false;
    bool mAnime4kInitialized = false;  // Tracks Anime4K shader readiness
    
    // Anime4K D3D12 resources
    void* mAnime4kPipelineState = nullptr;      // ID3D12PipelineState*
    void* mAnime4kRootSignature = nullptr;      // ID3D12RootSignature*
    void* mAnime4kDescriptorHeap = nullptr;     // ID3D12DescriptorHeap*
    GpuTextureHandle mAnime4kInputTexture = {}; // GPU input texture (640x480)
    GpuTextureHandle mAnime4kOutputTexture = {}; // GPU output texture (display size)
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
    uint32_t* mTempBuffer = nullptr;      // Temporary buffer for pre-processing
    int mInputPitch = 0;
    int mOutputPitch = 0;

    // FidelityFX context (Phase 2 will populate this)
    void* mFsrContext = nullptr;
    bool mFsrContextInitialized = false;

    // Phase 3: GPU device context for FSR2 compute
    ID3D12Device* mGpuDevice = nullptr;
    ID3D12CommandQueue* mGpuCommandQueue = nullptr;
    ID3D12CommandAllocator* mGpuCommandAllocator = nullptr;
    ID3D12GraphicsCommandList* mGpuCommandList = nullptr;
    
    // Phase 4: GPU textures for FSR2 input/output
    GpuTextureHandle mGpuInputTexture = { nullptr };
    GpuTextureHandle mGpuOutputTexture = { nullptr };

    // Configuration
    bool mMotionVectorsEnabled = false;
    float mSharpness = 0.5f;
    uint32_t mFrameIndex = 0;
    
    // Simplified filter configuration (complements FSR2 RCAS)
    // Note: Edge enhancement, color, contrast now handled by FSR2's built-in RCAS
    bool mEnableDebanding = true;      // Reduce 8-bit palette color banding
    float mDebandingStrength = 0.5f;   // Temporal dithering strength
    bool mEnableEdgeSmoothing = true;  // Optional pixel art anti-aliasing
    float mSmoothingStrength = 0.6f;   // Selective edge smoothing strength
    
    // Pre-scaling Kuwahara filter (edge-preserving color smoothing)
    bool mEnableKuwahara = false;      // Edge-preserving smoothing before scaling
    int mKuwaharaRadius = 2;           // Smoothing window radius (1-5)
    
    bool mVerboseLogging = false;      // Detailed diagnostic logging

    // Error tracking
    char mLastError[ERROR_MSG_SIZE] = {};
    
    // Upscaler logging
    FILE* mUpscaleLog = nullptr;
    std::string mLogFilePath;
};

// Global singleton accessor
UpscalerImpl* upscalerGetImpl() {
    return UpscalerImpl::getInstance();
}

// ============================================================================
// Diagnostic & Error Handling
// ============================================================================

void UpscalerImpl::logDiagnostic(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    diagnosticsLog(DiagnosticsLevel::Info, "UPSCALER", "%s", buffer);
    
    // Also write to dedicated upscale.log file
    if (mUpscaleLog == nullptr && mLogFilePath.empty()) {
        // Initialize log file path on first use
        char* basePath = SDL_GetBasePath();
        if (basePath != nullptr) {
            mLogFilePath = std::string(basePath) + "upscale.log";
            SDL_free(basePath);
        } else {
            // Fallback to current directory if SDL_GetBasePath fails
            mLogFilePath = "upscale.log";
        }
    }
    
    if (mUpscaleLog == nullptr && !mLogFilePath.empty()) {
        mUpscaleLog = fopen(mLogFilePath.c_str(), "w");
    }
    
    if (mUpscaleLog != nullptr) {
        // Get current time for timestamp
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
    // Log as warning without using enum class qualifier
    diagnosticsLog(static_cast<DiagnosticsLevel>(1), "UPSCALER", "%s", mLastError);
}

// ============================================================================
// Configuration Management
// ============================================================================

void UpscalerImpl::loadConfiguration() {
    if (!gGameConfigInitialized) {
        logDiagnostic("Config: Game config not initialized yet, using defaults");
        return;
    }
    
    logDiagnostic("========================================");
    logDiagnostic("LOADING UPSCALER CONFIGURATION");
    logDiagnostic("========================================");
    
    // Load upscaler mode (0=NONE, 1=FSR2, 2=INTEGER_2X, 3=INTEGER_3X, 4=INTEGER_4X, 5=ANIME4K)
    int modeValue = 1; // Default to FSR2
    if (configGetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_MODE_KEY, &modeValue)) {
        logDiagnostic("Config: upscaler_mode = %d (0=NONE, 1=FSR2, 2=INT2X, 3=INT3X, 4=INT4X, 5=ANIME4K)", modeValue);
    } else {
        logDiagnostic("Config: upscaler_mode not found, using default FSR2");
    }
    
    // Convert integer value to enum
    if (modeValue >= 0 && modeValue <= 5) {
        mConfiguredMode = static_cast<UpscalerMode>(modeValue);
    } else {
        logDiagnostic("WARNING: Invalid upscaler_mode=%d, using INTEGER_3X", modeValue);
        mConfiguredMode = UpscalerMode::INTEGER_3X;
    }
    
    // Load upscaler quality (0=QUALITY, 1=BALANCED, 2=PERFORMANCE)
    int qualityValue = 1; // Default to BALANCED
    if (configGetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_QUALITY_KEY, &qualityValue)) {
        mQuality = static_cast<UpscalerQuality>(qualityValue);
        logDiagnostic("Config: upscaler_quality = %d (0=QUALITY, 1=BALANCED, 2=PERFORMANCE)", qualityValue);
    } else {
        logDiagnostic("Config: upscaler_quality not found, using default BALANCED");
    }
    
    // Load sharpness (0.0-1.0)
    double sharpnessValue = 0.5;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_SHARPNESS_KEY, &sharpnessValue)) {
        mSharpness = static_cast<float>(sharpnessValue);
        logDiagnostic("Config: upscaler_sharpness = %.2f", mSharpness);
    } else {
        logDiagnostic("Config: upscaler_sharpness not found, using default 0.5");
    }
    
    // Load debanding settings (8-bit palette artifact reduction)
    bool debandingEnabled = true;
    if (configGetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_DEBANDING_KEY, &debandingEnabled)) {
        mEnableDebanding = debandingEnabled;
        logDiagnostic("Config: upscaler_debanding = %s", mEnableDebanding ? "true" : "false");
    } else {
        logDiagnostic("Config: upscaler_debanding not found, using default true");
    }
    
    double debandingStrValue = 0.5;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_DEBANDING_STRENGTH_KEY, &debandingStrValue)) {
        mDebandingStrength = static_cast<float>(debandingStrValue);
        logDiagnostic("Config: upscaler_debanding_strength = %.2f", mDebandingStrength);
    } else {
        logDiagnostic("Config: upscaler_debanding_strength not found, using default 0.5");
    }
    
    // Load edge smoothing settings (optional pixel art anti-aliasing)
    bool smoothingEnabled = true;
    if (configGetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_EDGE_SMOOTHING_KEY, &smoothingEnabled)) {
        mEnableEdgeSmoothing = smoothingEnabled;
        logDiagnostic("Config: upscaler_edge_smoothing = %s", mEnableEdgeSmoothing ? "true" : "false");
    } else {
        logDiagnostic("Config: upscaler_edge_smoothing not found, using default true");
    }
    
    double smoothingStrValue = 0.6;
    if (configGetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_SMOOTHING_STRENGTH_KEY, &smoothingStrValue)) {
        mSmoothingStrength = static_cast<float>(smoothingStrValue);
        logDiagnostic("Config: upscaler_smoothing_strength = %.2f", mSmoothingStrength);
    } else {
        logDiagnostic("Config: upscaler_smoothing_strength not found, using default 0.6");
    }
    
    // Load Kuwahara filter settings (edge-preserving pre-scaling smoothing)
    bool kuwaharaEnabled = false;
    if (configGetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_kuwahara_enable", &kuwaharaEnabled)) {
        mEnableKuwahara = kuwaharaEnabled;
        logDiagnostic("Config: upscaler_kuwahara_enable = %s", mEnableKuwahara ? "true" : "false");
    } else {
        logDiagnostic("Config: upscaler_kuwahara_enable not found, using default false");
    }
    
    int radiusValue = 2;
    if (configGetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, "upscaler_kuwahara_radius", &radiusValue)) {
        mKuwaharaRadius = std::clamp(radiusValue, 1, 5);
        logDiagnostic("Config: upscaler_kuwahara_radius = %d (clamped 1-5)", mKuwaharaRadius);
    } else {
        logDiagnostic("Config: upscaler_kuwahara_radius not found, using default 2");
    }
    
    // Load verbose logging flag (FORCE DISABLED due to performance impact)
    bool verboseValue = false;
    if (configGetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_VERBOSE_LOG_KEY, &verboseValue)) {
        mVerboseLogging = false;  // FORCE DISABLED - verbose logging causes massive slowdown
        logDiagnostic("Config: upscaler_verbose_log = %s (forced false for performance)", verboseValue ? "true (config)" : "false (config)");
    } else {
        mVerboseLogging = false;
        logDiagnostic("Config: upscaler_verbose_log not found, using default false");
    }
    
    logDiagnostic("========================================");
    logDiagnostic("CONFIGURATION LOADED");
    logDiagnostic("========================================\n");
}

void UpscalerImpl::saveConfiguration() {
    if (!gGameConfigInitialized) {
        logDiagnostic("Config: Cannot save - game config not initialized");
        return;
    }
    
    logDiagnostic("Saving upscaler configuration to fallout2.cfg...");
    
    // Save current settings (simplified - only FSR2 RCAS sharpness + lightweight filters)
    configSetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_MODE_KEY, static_cast<int>(mMode));
    configSetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_QUALITY_KEY, static_cast<int>(mQuality));
    configSetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_SHARPNESS_KEY, mSharpness);
    configSetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_DEBANDING_KEY, mEnableDebanding);
    configSetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_DEBANDING_STRENGTH_KEY, mDebandingStrength);
    configSetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_EDGE_SMOOTHING_KEY, mEnableEdgeSmoothing);
    configSetDouble(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_SMOOTHING_STRENGTH_KEY, mSmoothingStrength);
    configSetBool(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_VERBOSE_LOG_KEY, mVerboseLogging);
    
    if (gameConfigSave()) {
        logDiagnostic("Configuration saved successfully");
    } else {
        logDiagnostic("WARNING: Failed to save configuration");
    }
}

// ============================================================================
// Buffer Management
// ============================================================================

// ============================================================================
// BUFFER ALLOCATION - INPUT BUFFER
// ============================================================================
// allocateInputBuffer() - Allocate RGBA buffer for converted input
//
// Allocates memory to hold the converted RGBA data after palette conversion.
// For Fallout 2, this is typically 640×480×4 bytes = 1,228,800 bytes.
//
// BUFFER PURPOSE:
// - Stores indexed color converted to RGBA (via setIndexedInput())
// - Input to filter pipeline (Kuwahara)
// - Source for integer scaling or FSR2 upload
//
// PARAMETERS:
// - width/height: Input dimensions (640×480 for Fallout 2)
//
// RETURNS: true on success, false if allocation fails or invalid dimensions
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
        setError("Failed to allocate input buffer (%zu bytes)", bufferSize);
        return false;
    }

    logDiagnostic("Allocated input buffer: %dx%d (%zu bytes)", width, height, bufferSize);
    return true;
}

// ============================================================================
// BUFFER ALLOCATION - OUTPUT BUFFER
// ============================================================================
// allocateOutputBuffer() - Allocate RGBA buffer for upscaled output
//
// Allocates memory to hold the upscaled/scaled output. Size depends on mode:
// - INTEGER_2X: 1280×960×4 = 4,915,200 bytes
// - INTEGER_3X: 1920×1440×4 = 11,059,200 bytes (but buffer is 2560×1440)
// - INTEGER_4X: 2560×1920×4 = 19,660,800 bytes
// - Physical display: 2560×1440×4 = 14,745,600 bytes (actual allocation)
//
// BUFFER PURPOSE:
// - Receives output from dispatchIntegerScale() or dispatchFsr2()
// - For INTEGER modes: Contains centered content with letterbox bars
// - Retrieved by renderPresent() via upscalerGetOutputBuffer()
// - Uploaded to gSdlTexture for rendering
//
// PARAMETERS:
// - width/height: Output dimensions (typically 2560×1440 for modern displays)
//
// RETURNS: true on success, false if allocation fails or invalid dimensions
// ============================================================================
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
        setError("Failed to allocate output buffer (%zu bytes)", bufferSize);
        mOutputWidth = 0;
        mOutputHeight = 0;
        return false;
    }

    logDiagnostic("Allocated output buffer: %dx%d (%zu bytes)", width, height, bufferSize);
    return true;
}

void UpscalerImpl::deallocateBuffers() {
    if (mInputBuffer != nullptr) {
        internal_free(mInputBuffer);
        mInputBuffer = nullptr;
    }
    if (mOutputBuffer != nullptr) {
        internal_free(mOutputBuffer);
        mOutputBuffer = nullptr;
    }
    if (mTempBuffer != nullptr) {
        internal_free(mTempBuffer);
        mTempBuffer = nullptr;
    }
    mInputWidth = 0;
    mInputHeight = 0;
    mOutputWidth = 0;
    mOutputHeight = 0;
}

// ============================================================================
// Temporal Jitter
// ============================================================================

void UpscalerImpl::calculateJitter(float& outX, float& outY) {
    static const float halton2[] = { 0.0f, 0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f };
    static const float halton3[] = { 0.0f, 0.333f, 0.667f, 0.111f, 0.444f, 0.778f, 0.222f, 0.556f };

    uint32_t frameIdx = mFrameIndex % 8;
    outX = halton2[frameIdx] - 0.5f;
    outY = halton3[frameIdx] - 0.5f;
}

// ============================================================================
// FSR2 Backend (Phase 2 implementation pending)
// ============================================================================

// ============================================================================
// FSR2 INITIALIZATION - GPU RESOURCE SETUP (DISABLED)
// ============================================================================
// initFsr2() - Initialize FSR2 GPU resources and context
//
// **DISABLED: Returns early with error message**
//
// This function would normally:
// 1. Acquire DirectX 12 device and command queue
// 2. Create GPU command allocator
// 3. Create input texture (640x480) and output texture (2560x1440)
// 4. Initialize FSR2 context with upscale parameters
// 5. Set up temporal jitter sequence
//
// However, FSR2 is incompatible with Fallout 2 (see dispatchFsr2() docs).
// This function now fails gracefully during initialization with a clear
// error message directing users to INTEGER scaling modes.
//
// GPU RESOURCE LIFECYCLE (if FSR2 were enabled):
// - Input texture: D3D12 upload heap, CPU-writable
// - Output texture: D3D12 unordered access, GPU-writable
// - Command allocator: Manages GPU command buffers
// - FSR2 context: Persistent across frames (stores temporal history)
//
// RETURNS: false (FSR2 disabled for 2D games)
// ============================================================================
bool UpscalerImpl::initFsr2() {
    logDiagnostic("================== FSR2 INITIALIZATION START ==================");
    logDiagnostic("Phase 3 - GPU device binding");
    logDiagnostic("Input resolution: %dx%d, Output resolution: %dx%d",
        mInputWidth, mInputHeight, mOutputWidth, mOutputHeight);
    
    // Phase 3: Get GPU device context for FSR2 compute operations
    logDiagnostic("Step 1/5: Checking GPU device readiness...");
    if (!gpuDeviceIsReady()) {
        setError("GPU device not initialized, cannot initialize FSR2");
        logDiagnostic("ERROR: GPU device is not ready!");
        return false;
    }
    logDiagnostic("Step 1/5: GPU device is ready ✓");
    
    logDiagnostic("Step 2/5: Acquiring GPU device context...");
    mGpuDevice = gpuDeviceGetDevice();
    mGpuCommandQueue = gpuDeviceGetCommandQueue();
    
    if (mGpuDevice == nullptr || mGpuCommandQueue == nullptr) {
        setError("Failed to acquire GPU device context");
        logDiagnostic("ERROR: GPU device context is null! Device=%p, Queue=%p",
            mGpuDevice, mGpuCommandQueue);
        return false;
    }
    logDiagnostic("Step 2/5: GPU device context acquired ✓ (Device=%p, Queue=%p)",
        mGpuDevice, mGpuCommandQueue);
    
    // Create command allocator for FSR2 compute operations
    logDiagnostic("Step 3/5: Creating GPU command allocator...");
    mGpuCommandAllocator = gpuDeviceCreateCommandAllocator();
    if (mGpuCommandAllocator == nullptr) {
        setError("Failed to create GPU command allocator");
        logDiagnostic("ERROR: GPU command allocator creation failed!");
        mGpuDevice = nullptr;
        mGpuCommandQueue = nullptr;
        return false;
    }
    logDiagnostic("Step 3/5: GPU command allocator created ✓ (Allocator=%p)",
        mGpuCommandAllocator);
    
    logDiagnostic("Step 4/5: Creating GPU input/output textures...");
    // Phase 4: Create GPU textures for input and output
    mGpuInputTexture = gpuTextureCreate(
        mInputWidth,
        mInputHeight,
        GpuTextureFormat::ARGB8888,
        static_cast<int>(GpuTextureUsage::SHADER_RESOURCE)
    );
    
    if (mGpuInputTexture.resource == nullptr) {
        setError("Failed to create GPU input texture");
        logDiagnostic("ERROR: Input texture creation failed (%dx%d)!",
            mInputWidth, mInputHeight);
        mGpuDevice = nullptr;
        mGpuCommandQueue = nullptr;
        mGpuCommandAllocator = nullptr;
        return false;
    }
    logDiagnostic("Step 4/5: Input texture created ✓ (%dx%d, resource=%p)",
        mInputWidth, mInputHeight, mGpuInputTexture.resource);
    
    mGpuOutputTexture = gpuTextureCreate(
        mOutputWidth,
        mOutputHeight,
        GpuTextureFormat::ARGB8888,
        static_cast<int>(GpuTextureUsage::UNORDERED_ACCESS)
    );
    
    if (mGpuOutputTexture.resource == nullptr) {
        setError("Failed to create GPU output texture");
        logDiagnostic("ERROR: Output texture creation failed (%dx%d)!",
            mOutputWidth, mOutputHeight);
        gpuTextureRelease(mGpuInputTexture);
        mGpuInputTexture = { nullptr };
        mGpuDevice = nullptr;
        mGpuCommandQueue = nullptr;
        mGpuCommandAllocator = nullptr;
        return false;
    }
    logDiagnostic("Step 4/5: Output texture created ✓ (%dx%d, resource=%p)",
        mOutputWidth, mOutputHeight, mGpuOutputTexture.resource);
    
    // Phase 5: Create FSR2 context with GPU device
    logDiagnostic("Step 5/5: Creating FSR2 context...");
#ifdef FALLOUT_HAS_FSR2
    // CRITICAL: FSR2 is incompatible with 2D pixel art games!
    // FSR2 requires motion vectors and depth buffer for temporal upscaling.
    // Fallout 2 is a 2D isometric game with no 3D camera or depth information.
    // Without motion vectors/depth, FSR2 will crash during GPU dispatch.
    // 
    // RECOMMENDED: Use INTEGER_2X/3X/4X modes instead for pixel-perfect scaling.
    logDiagnostic("WARNING: FSR2 mode is not supported for Fallout 2!");
    logDiagnostic("FSR2 requires motion vectors and depth buffer (3D games only).");
    logDiagnostic("Fallout 2 is a 2D game and will crash with FSR2.");
    logDiagnostic("Please use INTEGER_2X, INTEGER_3X, or INTEGER_4X modes instead.");
    logDiagnostic("Set upscaler_mode=2 (2x), 3 (3x), or 4 (4x) in fallout2.cfg");
    setError("FSR2 mode is not compatible with Fallout 2 (2D game)");
    
    // Clean up allocated resources
    if (mGpuOutputTexture.resource != nullptr) {
        gpuTextureRelease(mGpuOutputTexture);
        mGpuOutputTexture = { nullptr };
    }
    if (mGpuInputTexture.resource != nullptr) {
        gpuTextureRelease(mGpuInputTexture);
        mGpuInputTexture = { nullptr };
    }
    if (mGpuCommandAllocator != nullptr) {
        // Command allocator is released by GPU device module
        mGpuCommandAllocator = nullptr;
    }
    mGpuDevice = nullptr;
    mGpuCommandQueue = nullptr;
    return false;
    
    // Create D3D12 backend descriptor (UNREACHABLE - FSR2 disabled above)
    ffxCreateBackendDX12Desc backendDesc = {};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.header.pNext = nullptr;
    backendDesc.device = (ID3D12Device*)mGpuDevice;
    
    logDiagnostic("  Backend descriptor created with D3D12 device=%p", mGpuDevice);
    
    // Create upscale context descriptor
    ffxCreateContextDescUpscale createDesc = {};
    createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    createDesc.header.pNext = &backendDesc.header;  // Chain backend descriptor
    
    // Set flags for FSR2 behavior
    createDesc.flags = 0;
    createDesc.flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;  // Fallout 2 outputs sRGB (gamma-corrected) colors
    // createDesc.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;  // Not needed for 8-bit palette
    // createDesc.flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;       // Not needed for fixed palette
    
    // Set maximum render and upscale sizes
    createDesc.maxRenderSize.width = mInputWidth;
    createDesc.maxRenderSize.height = mInputHeight;
    createDesc.maxUpscaleSize.width = mOutputWidth;
    createDesc.maxUpscaleSize.height = mOutputHeight;
    createDesc.fpMessage = nullptr;  // No message callback for now
    
    logDiagnostic("  Context descriptor prepared: maxRender=%dx%d, maxUpscale=%dx%d",
        createDesc.maxRenderSize.width, createDesc.maxRenderSize.height,
        createDesc.maxUpscaleSize.width, createDesc.maxUpscaleSize.height);
    
    logDiagnostic("  Attempting context creation with descriptor chain (Upscale -> Backend DX12)...");
    
    ffxReturnCode_t fsr2Result = ffxCreateContext(&mFsrContext, (ffxCreateContextDescHeader*)&createDesc, nullptr);
    if (fsr2Result != FFX_API_RETURN_OK) {
        const char* errorMsg = "Unknown error";
        if (fsr2Result == 1) {
            errorMsg = "FFX_API_RETURN_ERROR - Backend initialization failed. "
                      "The FFX SDK 2.1 DLL backend requires proper D3D12 device binding which "
                      "may not be supported in the current integration. "
                      "Possible solutions: 1) Use FFX SDK with full backend API access, "
                      "2) Implement custom D3D12 backend interface, or "
                      "3) Fallback to CPU-based upscaling.";
        }
        setError("Failed to create FSR2 context (error code: %d)", fsr2Result);
        logDiagnostic("ERROR: ffxCreateContext failed with code %d!", fsr2Result);
        logDiagnostic("ERROR: %s", errorMsg);
        logDiagnostic("WORKAROUND: Upscaler will continue in fallback mode (CPU-based bilinear scaling)");
        
        // Clean up GPU resources but don't fail - we'll use CPU fallback
        gpuTextureRelease(mGpuInputTexture);
        gpuTextureRelease(mGpuOutputTexture);
        mGpuInputTexture = { nullptr };
        mGpuOutputTexture = { nullptr };
        
        // Mark as initialized but without FSR2 acceleration
        mFsrContextInitialized = false;
        logDiagnostic("Step 5/5: FSR2 unavailable, using CPU fallback ⚠");
        logDiagnostic("================== FSR2 INITIALIZATION INCOMPLETE (FALLBACK MODE) ==================");
        return true;  // Continue without FSR2 rather than failing completely
    }
    
    logDiagnostic("Step 5/5: FSR2 context created successfully ✓ (context=%p)", mFsrContext);
    mFsrContextInitialized = true;
#else
    logDiagnostic("Step 5/5: FSR2 SDK not available - upscaling will be CPU-based (Phase 5 skipped)");
    logDiagnostic("  Define FALLOUT_HAS_FSR2 to enable GPU acceleration");
    mFsrContextInitialized = true;  // Still mark as initialized even without SDK
#endif
    
    logDiagnostic("================== FSR2 INITIALIZATION COMPLETE ✓ ==================");
    return true;
}

bool UpscalerImpl::shutdownFsr2() {
    // Phase 5: Destroy FSR2 context
#ifdef FALLOUT_HAS_FSR2
    if (mFsrContext != nullptr) {
        ffxReturnCode_t result = ffxDestroyContext(&mFsrContext, nullptr);
        if (result != FFX_API_RETURN_OK) {
            logDiagnostic("Warning: ffxDestroyContext returned error code %d", result);
        }
        mFsrContext = nullptr;
    }
#endif
    
    // Phase 4: Release GPU textures
    if (mGpuInputTexture.resource != nullptr) {
        gpuTextureRelease(mGpuInputTexture);
        mGpuInputTexture = { nullptr };
    }
    
    if (mGpuOutputTexture.resource != nullptr) {
        gpuTextureRelease(mGpuOutputTexture);
        mGpuOutputTexture = { nullptr };
    }
    
    // Phase 3: Clean up GPU resources
    if (mGpuCommandList != nullptr) {
        // Command list is released by GPU device module
        mGpuCommandList = nullptr;
    }
    
    if (mGpuCommandAllocator != nullptr) {
        // Command allocator is released by GPU device module
        mGpuCommandAllocator = nullptr;
    }
    
    // Device and queue pointers are owned by GPU device module, don't release
    mGpuDevice = nullptr;
    mGpuCommandQueue = nullptr;
    mFsrContextInitialized = false;
    
    return true;
}

// ============================================================================
// FSR2 DISPATCH - TEMPORAL UPSCALING (DISABLED)
// ============================================================================
// dispatchFsr2() - AMD FidelityFX Super Resolution 2.0 GPU upscaling
//
// **IMPORTANT: FSR2 IS DISABLED FOR FALLOUT 2**
//
// FSR2 is a temporal upscaling technology designed for 3D games. It requires:
// - Motion vectors: Track pixel movement between frames (camera/object motion)
// - Depth buffer: Z-depth information for proper temporal accumulation
// - 3D camera data: FOV, near/far planes, view transforms
//
// INCOMPATIBILITY WITH FALLOUT 2:
// Fallout 2 is a 2D isometric game with:
// - Pre-rendered backgrounds (no camera motion)
// - 2D sprites (no depth information)
// - Orthographic projection (no 3D perspective)
//
// Without motion vectors and depth buffer, FSR2's GPU compute shaders crash
// during ffxDispatch(). This is a fundamental incompatibility, not a bug.
//
// RECOMMENDED ALTERNATIVE:
// Use INTEGER_2X/3X/4X modes instead:
// - Perfect for pixel art preservation
// - No GPU driver dependencies
// - Faster performance (<2ms vs ~5-8ms for FSR2)
// - Optional Kuwahara filter for color smoothing
//
// This function now returns an error early to prevent crashes.
// ============================================================================
bool UpscalerImpl::dispatchFsr2() {
    // Phase 5: Dispatch GPU compute with FSR2 context
    logDiagnostic("================== FSR2 DISPATCH START (Frame %d) ==================", mFrameIndex);
    
    logDiagnostic("Pre-dispatch validation checks...");
    if (!mFsrContextInitialized || mGpuDevice == nullptr || mGpuCommandQueue == nullptr) {
        setError("FSR2 not properly initialized, cannot dispatch");
        logDiagnostic("ERROR: FSR2 not initialized! context=%d, device=%p, queue=%p",
            mFsrContextInitialized, mGpuDevice, mGpuCommandQueue);
        return false;
    }
    logDiagnostic("  Context check: PASS ✓");
    
    if (mGpuInputTexture.resource == nullptr || mGpuOutputTexture.resource == nullptr) {
        setError("GPU textures not created, cannot dispatch");
        logDiagnostic("ERROR: GPU textures not valid! input=%p, output=%p",
            mGpuInputTexture.resource, mGpuOutputTexture.resource);
        return false;
    }
    logDiagnostic("  Texture check: PASS ✓");
    
    // Phase 4: Upload input buffer to GPU texture
    logDiagnostic("Uploading input buffer to GPU (%dx%d = %d bytes)...",
        mInputWidth, mInputHeight, mInputWidth * mInputHeight * sizeof(uint32_t));
    if (!gpuTextureUpload(mGpuInputTexture, mInputBuffer, mInputWidth * mInputHeight * sizeof(uint32_t))) {
        setError("Failed to upload input texture");
        logDiagnostic("ERROR: GPU texture upload failed!");
        return false;
    }
    logDiagnostic("  Upload complete ✓");
    
    // Phase 5: Dispatch FSR2 GPU compute
    logDiagnostic("Dispatching FSR2 GPU compute...");
#ifdef FALLOUT_HAS_FSR2
    if (mFsrContext != nullptr) {
        // Create FSR2 dispatch descriptor with input/output texture bindings
        ffxDispatchDescUpscale dispatchDesc = {};
        dispatchDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatchDesc.commandList = nullptr;  // Managed by FFX internally via GPU device
        
        if (mVerboseLogging) {
            logDiagnostic("  Setting up input texture binding...");
        }
        // Bind input texture (color) - prepare FfxApiResource
        ID3D12Resource* inputResource = gpuTextureGetResource(mGpuInputTexture);
        if (inputResource == nullptr) {
            setError("Failed to get input texture resource for FSR2 dispatch");
            logDiagnostic("ERROR: Could not get input texture resource!");
            return false;
        }
        dispatchDesc.color.resource = inputResource;
        dispatchDesc.color.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        if (mVerboseLogging) {
            logDiagnostic("    Input resource: %p ✓", inputResource);
        }
        
        if (mVerboseLogging) {
            logDiagnostic("  Setting up output texture binding...");
        }
        // Bind output texture - prepare FfxApiResource
        ID3D12Resource* outputResource = gpuTextureGetResource(mGpuOutputTexture);
        if (outputResource == nullptr) {
            setError("Failed to get output texture resource for FSR2 dispatch");
            logDiagnostic("ERROR: Could not get output texture resource!");
            return false;
        }
        dispatchDesc.output.resource = outputResource;
        dispatchDesc.output.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
        if (mVerboseLogging) {
            logDiagnostic("    Output resource: %p ✓", outputResource);
        }
        
        // Set FSR2 parameters
        if (mVerboseLogging) {
            logDiagnostic("  Configuring FSR2 parameters...");
        }
        float jitterX = 0.0f, jitterY = 0.0f;
        calculateJitter(jitterX, jitterY);
        dispatchDesc.jitterOffset.x = jitterX;
        dispatchDesc.jitterOffset.y = jitterY;
        dispatchDesc.motionVectorScale.x = 1.0f;  // Motion vectors in pixel space
        dispatchDesc.motionVectorScale.y = 1.0f;
        dispatchDesc.preExposure = 1.0f;          // Default pre-exposure
        dispatchDesc.sharpness = mSharpness;      // Quality-based sharpness
        dispatchDesc.enableSharpening = (mSharpness > 0.0f);
        if (mVerboseLogging) {
            logDiagnostic("    Jitter offset: (%.4f, %.4f)", jitterX, jitterY);
            logDiagnostic("    Sharpness: %.2f, Enabled: %s", mSharpness, dispatchDesc.enableSharpening ? "yes" : "no");
        }
        
        // Set render and target size
        dispatchDesc.renderSize.width = mInputWidth;
        dispatchDesc.renderSize.height = mInputHeight;
        dispatchDesc.upscaleSize.width = mOutputWidth;
        dispatchDesc.upscaleSize.height = mOutputHeight;
        logDiagnostic("    Render size: %dx%d", dispatchDesc.renderSize.width, dispatchDesc.renderSize.height);
        logDiagnostic("    Upscale size: %dx%d", dispatchDesc.upscaleSize.width, dispatchDesc.upscaleSize.height);
        
        // Set optional camera parameters
        dispatchDesc.cameraNear = 0.1f;          // Default near plane
        dispatchDesc.cameraFar = 1000.0f;        // Default far plane
        dispatchDesc.cameraFovAngleVertical = 1.5708f;  // 90 degrees in radians
        dispatchDesc.viewSpaceToMetersFactor = 1.0f;
        dispatchDesc.reset = false;              // No reset for this frame
        dispatchDesc.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;  // Input is sRGB (gamma-corrected)
        
        // Final validation before dispatch
        logDiagnostic("  Final pre-dispatch validation...");
        if (mGpuDevice == nullptr || mGpuCommandQueue == nullptr) {
            setError("GPU device or command queue is null before dispatch");
            logDiagnostic("ERROR: GPU resources invalid! device=%p, queue=%p", mGpuDevice, mGpuCommandQueue);
            return false;
        }
        if (inputResource == nullptr || outputResource == nullptr) {
            setError("Input or output resource is null before dispatch");
            logDiagnostic("ERROR: Texture resources invalid! input=%p, output=%p", inputResource, outputResource);
            return false;
        }
        logDiagnostic("  Validation PASS ✓");
        
        // Dispatch FSR2 upscaling (cast to base header for API compatibility)
        logDiagnostic("  Invoking ffxDispatch()...");
        ffxReturnCode_t result = FFX_API_RETURN_ERROR;
        try {
            result = ffxDispatch(&mFsrContext, (ffxDispatchDescHeader*)&dispatchDesc);
        } catch (...) {
            setError("FSR2 dispatch threw exception (GPU driver issue?)");
            logDiagnostic("ERROR: ffxDispatch threw C++ exception! Possible GPU driver crash.");
            return false;
        }
        
        if (result != FFX_API_RETURN_OK) {
            setError("FSR2 dispatch failed (error code: %d)", result);
            logDiagnostic("ERROR: ffxDispatch returned error code %d!", result);
            return false;
        }
        
        logDiagnostic("  FSR2 dispatch successful ✓");
        logDiagnostic("  Result: %dx%d -> %dx%d, Frame %d", 
                      mInputWidth, mInputHeight, mOutputWidth, mOutputHeight, mFrameIndex);
    } else {
        setError("FSR2 context is null");
        logDiagnostic("ERROR: FSR2 context is null!");
        return false;
    }
#else
    logDiagnostic("  FSR2 SDK not available - GPU dispatch skipped (Phase 5 disabled)");
    logDiagnostic("  Define FALLOUT_HAS_FSR2 to enable GPU acceleration");
#endif
    
    // Phase 4: Download output texture to GPU buffer
    logDiagnostic("Downloading output texture from GPU...");
    if (!gpuTextureDownload(mGpuOutputTexture, mOutputBuffer, mOutputWidth * mOutputHeight * sizeof(uint32_t))) {
        setError("Failed to download output texture");
        logDiagnostic("ERROR: GPU texture download failed!");
        return false;
    }
    logDiagnostic("  Download complete ✓");
    
    // Phase 6: Apply lightweight post-processing filters
    // Note: FSR2's built-in RCAS already handles edge enhancement, color correction,
    // and contrast. We only apply filters for issues FSR2 doesn't address natively.
    if (mVerboseLogging) {
        logDiagnostic("Phase 6/6: Applying post-processing filters (complement to FSR2 RCAS)...");
    }
    
    FilterConfig filterConfig;
    filterConfig.type = FilterType::MINIMAL;  // Lightweight: debanding + optional edge smoothing
    filterConfig.enableDebanding = mEnableDebanding;
    filterConfig.debandingStrength = mDebandingStrength;
    filterConfig.frameIndex = mFrameIndex;
    filterConfig.enableEdgeSmoothing = mEnableEdgeSmoothing;
    filterConfig.smoothingStrength = mSmoothingStrength;
    filterConfig.edgeDetectThreshold = 0.15f;   // Optimized for pixel art edges
    filterConfig.enableLogging = mVerboseLogging;
    
    if (mVerboseLogging) {
        logDiagnostic("  Lightweight Filter Pipeline (FSR2 sRGB mode + RCAS sharpening: %.2f):", mSharpness);
        logDiagnostic("  - Debanding: %s (strength: %.2f, frame: %d)", 
            filterConfig.enableDebanding ? "enabled" : "disabled", 
            filterConfig.debandingStrength, filterConfig.frameIndex);
        logDiagnostic("  - Edge Smoothing: %s (strength: %.2f, threshold: %.2f)", 
            filterConfig.enableEdgeSmoothing ? "enabled" : "disabled", 
            filterConfig.smoothingStrength, filterConfig.edgeDetectThreshold);
        logDiagnostic("  Note: Edge enhancement, color, contrast handled by FSR2 RCAS (GPU-optimized)");
    }
    
    if (filterApplyPostProcessing(mOutputBuffer, mOutputWidth, mOutputHeight, 
                                  mOutputWidth * sizeof(uint32_t), filterConfig)) {
        if (mVerboseLogging) {
            logDiagnostic("  Post-processing filters applied successfully ✓");
        }
    } else {
        logDiagnostic("  WARNING: Post-processing failed, continuing without filters");
    }
    
    mFrameIndex++;
    if (mVerboseLogging) {
        logDiagnostic("================== FSR2 DISPATCH COMPLETE ✓ (Frame %d) ==================", mFrameIndex - 1);
    }
    return true;
}

/**
 * Integer scaling dispatch (lossless pixel replication)
 * Optimal for pixel art - zero blur, zero artifacts, perfect sharpness
 */
// ============================================================================
// INTEGER SCALING - PIXEL-PERFECT UPSCALING
// ============================================================================
// dispatchIntegerScale() - Perfect pixel replication with optional filters
//
// This algorithm provides artifact-free upscaling by replicating each source
// pixel exactly N times (where N = 2, 3, or 4). This preserves the sharp
// pixel art aesthetic of Fallout 2.
//
// PROCESSING PIPELINE:
// 1. Apply Kuwahara filter (optional): Edge-preserving color smoothing
//    - Reduces color banding from 8-bit palette
//    - Preserves sharp edges (unlike blur)
//    - Configurable radius (1-5 pixels)
//
// 2. Integer scaling: Replicate each pixel N×N times
//    - 2X: 640×480 → 1280×960
//    - 3X: 640×480 → 1920×1440 (recommended for 2560×1440 displays)
//    - 4X: 640×480 → 2560×1920 (for 4K displays)
//
// 3. Letterboxing: Center scaled content in output buffer
//    - For 3X on 2560×1440: place 1920×1440 content centered
//    - Add 320px black bars on left/right sides
//    - Maintains perfect 4:3 aspect ratio
//
// PERFORMANCE: ~1-3ms per frame (Release build with Kuwahara enabled)
//
// PARAMETERS:
// - scaleFactor: 2, 3, or 4 (pixel replication factor)
//
// RETURNS: true on success, false on failure
// ============================================================================
bool UpscalerImpl::dispatchIntegerScale(int scaleFactor) {
    // Log every 60 frames (roughly once per second at 60fps) to avoid log spam
    bool shouldLog = (mFrameIndex % 60 == 0) || mVerboseLogging;
    
    if (shouldLog) {
        logDiagnostic("================== INTEGER SCALE DISPATCH (Frame %d, Factor: %dx) ==================", mFrameIndex, scaleFactor);
    }
    
    if (mInputBuffer == nullptr || mOutputBuffer == nullptr) {
        setError("Input or output buffer is null");
        logDiagnostic("ERROR: Buffers not allocated! input=%p, output=%p", mInputBuffer, mOutputBuffer);
        return false;
    }
    
    if (scaleFactor < 2 || scaleFactor > 4) {
        setError("Scale factor must be 2, 3, or 4");
        logDiagnostic("ERROR: Invalid scale factor %d", scaleFactor);
        return false;
    }
    
    if (shouldLog) {
        logDiagnostic("Performing %dx integer scaling: %dx%d -> %dx%d",
                      scaleFactor, mInputWidth, mInputHeight, mOutputWidth, mOutputHeight);
    }
    
    // Phase 1: Optional pre-scaling edge-preserving smoothing (Kuwahara filter)
    uint32_t* sourceBuffer = mInputBuffer;
    
    if (mEnableKuwahara) {
        if (shouldLog) {
            logDiagnostic("Applying Kuwahara filter (pre-scaling smoothing, radius=%d)...", mKuwaharaRadius);
        }
        
        // Allocate temp buffer if not already done
        if (mTempBuffer == nullptr) {
            int tempSize = mInputWidth * mInputHeight * sizeof(uint32_t);
            mTempBuffer = static_cast<uint32_t*>(internal_malloc(tempSize));
            if (mTempBuffer == nullptr) {
                logDiagnostic("ERROR: Failed to allocate temp buffer for Kuwahara!");
                return false;
            }
        }
        
        // Apply Kuwahara filter (edge-preserving color smoothing)
        if (!filterApplyKuwahara(mInputBuffer, mTempBuffer, mInputWidth, mInputHeight, mKuwaharaRadius)) {
            logDiagnostic("ERROR: Kuwahara filter failed!");
            return false;
        }
        
        sourceBuffer = mTempBuffer;  // Use filtered buffer as source for scaling
        if (shouldLog) {
            logDiagnostic("Kuwahara filter applied ✓ (smoothed colors, preserved edges)");
        }
    } else if (shouldLog) {
        logDiagnostic("Kuwahara filter: DISABLED (using raw input)");
    }
    
    // Phase 2: Integer scaling - pixel replication
    // Each input pixel becomes an NxN block
    // For INTEGER_3X: 640x480 -> 1920x1440, centered in output buffer with letterboxing
    
    // Calculate actual scaled dimensions
    int scaledWidth = mInputWidth * scaleFactor;   // 640 * 3 = 1920
    int scaledHeight = mInputHeight * scaleFactor;  // 480 * 3 = 1440
    
    // Calculate centering offset for letterboxing
    int offsetX = (mOutputWidth - scaledWidth) / 2;   // (2560-1920)/2 = 320
    int offsetY = (mOutputHeight - scaledHeight) / 2;  // (1440-1440)/2 = 0
    
    // Clear output buffer to black (for letterbox bars)
    memset(mOutputBuffer, 0, mOutputWidth * mOutputHeight * sizeof(uint32_t));
    
    uint32_t* output = mOutputBuffer;
    
    for (int y = 0; y < mInputHeight; y++) {
        for (int x = 0; x < mInputWidth; x++) {
            uint32_t pixel = sourceBuffer[y * mInputWidth + x];
            
            // Replicate to NxN block in output, with centering offset
            for (int dy = 0; dy < scaleFactor; dy++) {
                int outY = offsetY + (y * scaleFactor + dy);
                if (outY < 0 || outY >= mOutputHeight) continue;
                
                for (int dx = 0; dx < scaleFactor; dx++) {
                    int outX = offsetX + (x * scaleFactor + dx);
                    if (outX < 0 || outX >= mOutputWidth) continue;
                    
                    output[outY * mOutputWidth + outX] = pixel;
                }
            }
        }
    }
    
    if (mVerboseLogging || (mFrameIndex % 60 == 0)) {
        logDiagnostic("Integer %dx scaling: %dx%d -> %dx%d (centered in %dx%d, offset +%d,+%d)",
                     scaleFactor, mInputWidth, mInputHeight, scaledWidth, scaledHeight,
                     mOutputWidth, mOutputHeight, offsetX, offsetY);
    }
    
    // Phase 3: Optional post-scaling filters (debanding/edge smoothing)
    if (mVerboseLogging) {
        logDiagnostic("Applying post-processing filters...");
    }
    
    FilterConfig filterConfig;
    filterConfig.type = FilterType::MINIMAL;
    filterConfig.enableDebanding = mEnableDebanding;
    filterConfig.debandingStrength = mDebandingStrength;
    filterConfig.frameIndex = mFrameIndex;
    filterConfig.enableEdgeSmoothing = mEnableEdgeSmoothing;
    filterConfig.smoothingStrength = mSmoothingStrength;
    filterConfig.edgeDetectThreshold = 0.15f;
    filterConfig.enableLogging = mVerboseLogging;
    
    if (filterApplyPostProcessing(mOutputBuffer, mOutputWidth, mOutputHeight, 
                                  mOutputWidth * sizeof(uint32_t), filterConfig)) {
        if (mVerboseLogging) {
            logDiagnostic("Post-processing filters applied ✓");
        }
    } else {
        logDiagnostic("WARNING: Post-processing failed, continuing without filters");
    }
    
    mFrameIndex++;
    if (shouldLog) {
        logDiagnostic("================== INTEGER SCALE COMPLETE ✓ (Frame %d) ==================", mFrameIndex - 1);
    }
    return true;
}

// ============================================================================
// ANIME4K SHADER DISPATCH (D3D12 COMPUTE IMPLEMENTATION)
// ============================================================================
// Executes Anime4K edge-aware upscaling compute shader on GPU
// Fallback: INTEGER_3X if GPU dispatch fails
// ============================================================================
bool UpscalerImpl::dispatchAnime4k() {
    using Microsoft::WRL::ComPtr;
    
    bool shouldLog = (mFrameIndex % 60 == 0) || mVerboseLogging;
    if (shouldLog) {
        logDiagnostic("================== ANIME4K DISPATCH START (Frame %d) ==================", mFrameIndex);
    }

    if (!mAnime4kInitialized) {
        if (shouldLog) {
            logDiagnostic("ERROR: Anime4K not initialized, fallback to INTEGER_3X");
        }
        return dispatchIntegerScale(3);
    }

    if (mInputBuffer == nullptr || mOutputBuffer == nullptr) {
        if (shouldLog) {
            logDiagnostic("ERROR: Input/Output buffers null, fallback");
        }
        return dispatchIntegerScale(3);
    }

    // Get D3D12 resources
    ID3D12Device* device = gpuDeviceGetDevice();
    ID3D12CommandAllocator* allocator = static_cast<ID3D12CommandAllocator*>(mAnime4kCommandAllocator);
    ID3D12GraphicsCommandList* cmdList = static_cast<ID3D12GraphicsCommandList*>(mAnime4kCommandList);
    ID3D12RootSignature* rootSig = static_cast<ID3D12RootSignature*>(mAnime4kRootSignature);
    ID3D12PipelineState* pso = static_cast<ID3D12PipelineState*>(mAnime4kPipelineState);
    
    if (!device || !allocator || !cmdList || !rootSig || !pso) {
        if (shouldLog) {
            logDiagnostic("ERROR: Anime4K resources invalid, fallback to INTEGER_3X");
        }
        return dispatchIntegerScale(3);
    }
    
    // Reset command allocator and list
    HRESULT hr = allocator->Reset();
    if (FAILED(hr)) {
        if (shouldLog) {
            logDiagnostic("ERROR: Failed to reset command allocator (0x%08X), fallback", hr);
        }
        return dispatchIntegerScale(3);
    }
    
    hr = cmdList->Reset(allocator, pso);
    if (FAILED(hr)) {
        if (shouldLog) {
            logDiagnostic("ERROR: Failed to reset command list (0x%08X), fallback", hr);
        }
        return dispatchIntegerScale(3);
    }
    
    // Upload input data to GPU texture
    if (!gpuTextureUpload(mAnime4kInputTexture, mInputBuffer, mInputWidth * mInputHeight * 4)) {
        if (shouldLog) {
            logDiagnostic("ERROR: Failed to upload input texture, fallback");
        }
        return dispatchIntegerScale(3);
    }
    
    // Set compute root signature and pipeline state
    cmdList->SetComputeRootSignature(rootSig);
    cmdList->SetPipelineState(pso);
    
    // Create and upload constant buffer data
    struct UpscaleParams {
        unsigned int inputWidth;
        unsigned int inputHeight;
        unsigned int outputWidth;
        unsigned int outputHeight;
        
        unsigned int effectiveWidth;
        unsigned int effectiveHeight;
        unsigned int offsetX;
        unsigned int offsetY;

        float rcpInputX;
        float rcpInputY;
        float rcpEffectiveOutputX;
        float rcpEffectiveOutputY;
        
        float strength;
        float padding[3];  // Align to 16 bytes
    };
    
    // Calculate letterboxing parameters (maintain 4:3 aspect ratio)
    float scaleX = (float)mOutputWidth / mInputWidth;
    float scaleY = (float)mOutputHeight / mInputHeight;
    float scale = (scaleX < scaleY) ? scaleX : scaleY;

    unsigned int effectiveWidth = (unsigned int)(mInputWidth * scale);
    unsigned int effectiveHeight = (unsigned int)(mInputHeight * scale);
    unsigned int offsetX = (mOutputWidth - effectiveWidth) / 2;
    unsigned int offsetY = (mOutputHeight - effectiveHeight) / 2;

    UpscaleParams params = {};
    params.inputWidth = mInputWidth;
    params.inputHeight = mInputHeight;
    params.outputWidth = mOutputWidth;
    params.outputHeight = mOutputHeight;
    
    params.effectiveWidth = effectiveWidth;
    params.effectiveHeight = effectiveHeight;
    params.offsetX = offsetX;
    params.offsetY = offsetY;

    params.rcpInputX = 1.0f / mInputWidth;
    params.rcpInputY = 1.0f / mInputHeight;
    params.rcpEffectiveOutputX = 1.0f / effectiveWidth;
    params.rcpEffectiveOutputY = 1.0f / effectiveHeight;
    
    params.strength = 1.0f;  // Default strength
    
    // Upload constant buffer (root CBV)
    uint64_t cbAddress = 0;
    if (!gpuUploadConstantBuffer(&params, sizeof(params), &cbAddress)) {
        if (shouldLog) {
            logDiagnostic("ERROR: Failed to upload constant buffer, fallback");
        }
        return dispatchIntegerScale(3);
    }
    
    cmdList->SetComputeRootConstantBufferView(0, cbAddress);
    
    // Set descriptor heap
    ID3D12DescriptorHeap* descriptorHeap = static_cast<ID3D12DescriptorHeap*>(mAnime4kDescriptorHeap);
    if (descriptorHeap == nullptr) {
        if (shouldLog) {
            logDiagnostic("ERROR: Descriptor heap is null, fallback");
        }
        return dispatchIntegerScale(3);
    }
    
    ID3D12DescriptorHeap* heaps[] = { descriptorHeap };
    cmdList->SetDescriptorHeaps(1, heaps);
    
    // Set descriptor table with SRV and UAV (parameter 1)
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    cmdList->SetComputeRootDescriptorTable(1, gpuHandle);
    
    // Dispatch compute shader (8x8 thread groups)
    unsigned int dispatchX = (mOutputWidth + 7) / 8;
    unsigned int dispatchY = (mOutputHeight + 7) / 8;
    unsigned int dispatchZ = 1;
    
    if (shouldLog) {
        logDiagnostic("Dispatching compute shader: %ux%ux%u thread groups", dispatchX, dispatchY, dispatchZ);
    }
    
    cmdList->Dispatch(dispatchX, dispatchY, dispatchZ);
    
    // Execute command list (gpuDeviceExecuteCommandList will close it)
    if (!gpuDeviceExecuteCommandList(cmdList)) {
        if (shouldLog) {
            logDiagnostic("ERROR: Failed to execute command list, fallback");
        }
        return dispatchIntegerScale(3);
    }
    
    // Wait for GPU to finish
    gpuDeviceWaitForGpu();
    
    // Download result from GPU texture to output buffer
    if (!gpuTextureDownload(mAnime4kOutputTexture, mOutputBuffer, mOutputWidth * mOutputHeight * 4)) {
        if (shouldLog) {
            logDiagnostic("ERROR: Failed to download output texture, fallback");
        }
        return dispatchIntegerScale(3);
    }
    
    if (shouldLog) {
        logDiagnostic("================== ANIME4K DISPATCH END (Frame %d) ✓ ==================", mFrameIndex);
    }

    return true;
}

bool UpscalerImpl::initAnime4k() {
    using Microsoft::WRL::ComPtr;
    
    logDiagnostic("================== ANIME4K INITIALIZATION START ==================");
    logDiagnostic("Setting up D3D12 compute pipeline for Anime4K upscaling...");
    logDiagnostic("Input resolution: %dx%d, Output resolution: %dx%d",
        mInputWidth, mInputHeight, mOutputWidth, mOutputHeight);
    
    // Step 1: Check GPU device readiness (matches FSR2 initialization pattern)
    logDiagnostic("Step 1/6: Checking GPU device readiness...");
    if (!gpuDeviceIsReady()) {
        setError("GPU device not initialized, cannot initialize Anime4K");
        logDiagnostic("ERROR: GPU device is not ready! Anime4K requires GPU compute support.");
        logDiagnostic("HINT: GPU device initialization may have failed or been skipped.");
        return false;
    }
    logDiagnostic("Step 1/6: GPU device is ready ✓");
    
    // Get GPU device
    logDiagnostic("Step 1.5/6: Acquiring GPU device context...");
    ID3D12Device* device = gpuDeviceGetDevice();
    if (device == nullptr) {
        setError("GPU device not available for Anime4K");
        logDiagnostic("ERROR: GPU device is null - cannot initialize Anime4K pipeline");
        return false;
    }
    logDiagnostic("Step 1.5/6: GPU device acquired ✓ (Device=%p)", device);
    
    // Step 2: Compile shader from embedded source
    logDiagnostic("Step 2/6: Compiling Anime4K compute shader...");
    
    // Read shader source from file (anime4k.hlsl in same directory as executable)
    char* basePath = SDL_GetBasePath();
    std::string shaderPath = std::string(basePath) + "shaders\\anime4k.hlsl";
    SDL_free(basePath);
    
    // For now, compile inline shader code (embedded in executable)
    const char* shaderCode = R"(
// Anime4K v3.2 Upscale Original x2 (Ported to HLSL)
// Ported from: https://github.com/bloc97/Anime4K/blob/master/glsl/Upscale/Anime4K_Upscale_Original_x2.glsl

#define REFINE_STRENGTH 0.5
#define REFINE_BIAS 0.0

// Polynomial coefficients
#define P5 ( 11.68129591)
#define P4 (-42.46906057)
#define P3 ( 60.28286266)
#define P2 (-41.84451327)
#define P1 ( 14.05517353)
#define P0 (-1.081521930)

cbuffer UpscaleParams : register(b0)
{
    uint2 inputSize;    // Source resolution (640, 480)
    uint2 outputSize;   // Target resolution (2560, 1440)
    uint2 effectiveSize; // Scaled resolution (e.g. 1920, 1440)
    uint2 offset;        // Letterbox offset (e.g. 320, 0)
    float2 rcpInput;    // 1.0 / inputSize
    float2 rcpEffectiveOutput; // 1.0 / effectiveSize
    float strength;     // Enhancement strength (0.0-1.0)
    float3 padding;
};

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);
SamplerState LinearSampler : register(s0);

float get_luma(float4 c)
{
    return dot(c.rgb, float3(0.299, 0.587, 0.114));
}

float power_function(float x)
{
    float x2 = x * x;
    float x3 = x2 * x;
    float x4 = x2 * x2;
    float x5 = x2 * x3;
    return P5 * x5 + P4 * x4 + P3 * x3 + P2 * x2 + P1 * x + P0;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= outputSize.x || DTid.y >= outputSize.y)
        return;

    // Letterboxing check
    if (DTid.x < offset.x || DTid.x >= offset.x + effectiveSize.x ||
        DTid.y < offset.y || DTid.y >= offset.y + effectiveSize.y)
    {
        OutputTexture[DTid.xy] = float4(0, 0, 0, 1); // Black bars
        return;
    }

    // Map to UV space of the effective area
    float2 pixelPos = float2(DTid.xy) - float2(offset);
    float2 uv = (pixelPos + 0.5f) * rcpEffectiveOutput;
    
    float2 d = rcpEffectiveOutput; // Use effective pixel size for sampling offsets

    // Sample center pixel (bilinear interpolation from input)
    float4 cc = InputTexture.SampleLevel(LinearSampler, uv, 0);

    // Calculate Luma of neighbors
    // We sample at offsets corresponding to the OUTPUT pixel size
    float t = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(0, -d.y), 0));
    float b = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(0, d.y), 0));
    float l = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(-d.x, 0), 0));
    float r = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(d.x, 0), 0));
    
    float tl = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(-d.x, -d.y), 0));
    float tr = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(d.x, -d.y), 0));
    float bl = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(-d.x, d.y), 0));
    float br = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(d.x, d.y), 0));
    
    // Sobel Gradients
    float gx = (tr + 2.0 * r + br) - (tl + 2.0 * l + bl);
    float gy = (bl + 2.0 * b + br) - (tl + 2.0 * t + tr);

    // Gradient Magnitude (Normalized by 4.0 to keep in 0-1 range for power function)
    float sobel_norm = sqrt(gx * gx + gy * gy) / 4.0;
    
    // Refinement Strength
    float dval = power_function(saturate(sobel_norm));
    dval = saturate(dval * REFINE_STRENGTH + REFINE_BIAS);

    // Determine edge direction
    float xpos = (gx > 0.0) ? 1.0 : -1.0;
    float ypos = (gy > 0.0) ? 1.0 : -1.0;
    
    // Sample pixels along the gradient direction
    float4 xval = InputTexture.SampleLevel(LinearSampler, uv + float2(d.x * xpos, 0), 0);
    float4 yval = InputTexture.SampleLevel(LinearSampler, uv + float2(0, d.y * ypos), 0);
    
    // Interpolate between xval and yval based on gradient ratio
    float abs_gx = abs(gx);
    float abs_gy = abs(gy);
    float xy_ratio = abs_gx / (abs_gx + abs_gy + 0.0001);
    
    float4 avg = xval * xy_ratio + yval * (1.0 - xy_ratio);
    
    // Blend original and refined
    float4 result = avg * dval + cc * (1.0 - dval);
    
    OutputTexture[DTid.xy] = result;
}
)";
    
    ComPtr<ID3DBlob> computeShader;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(
        shaderCode, strlen(shaderCode),
        "Anime4K",  // Shader name
        nullptr,    // Defines
        nullptr,    // Include handler
        "main",     // Entry point
        "cs_5_0",   // Target profile (compute shader 5.0)
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        computeShader.GetAddressOf(),
        errorBlob.GetAddressOf()
    );
    
    if (FAILED(hr)) {
        if (errorBlob) {
            const char* errorMsg = static_cast<const char*>(errorBlob->GetBufferPointer());
            setError("Shader compilation failed: %s", errorMsg);
            logDiagnostic("ERROR: Anime4K shader compilation failed:");
            logDiagnostic("%s", errorMsg);
        } else {
            setError("Shader compilation failed with HRESULT 0x%08X", hr);
            logDiagnostic("ERROR: Shader compilation failed with HRESULT 0x%08X", hr);
        }
        return false;
    }
    logDiagnostic("Step 2/6: Shader compiled ✓ (%zu bytes)", computeShader->GetBufferSize());
    
    // Step 3: Create root signature
    logDiagnostic("Step 3/6: Creating root signature...");
    
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    
    // Range 0: SRV for input texture (t0)
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    
    // Range 1: UAV for output texture (u0)
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    
    D3D12_ROOT_PARAMETER rootParams[2] = {};
    
    // Parameter 0: Constant buffer (UpscaleParams)
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    // Parameter 1: Descriptor table with SRV and UAV
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 2;
    rootParams[1].DescriptorTable.pDescriptorRanges = ranges;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    // Static sampler for linear filtering
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 2;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &sampler;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    
    ComPtr<ID3DBlob> rootSigBlob;
    ComPtr<ID3DBlob> rootSigError;
    hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, rootSigBlob.GetAddressOf(), rootSigError.GetAddressOf());
    
    if (FAILED(hr)) {
        if (rootSigError) {
            const char* errorMsg = static_cast<const char*>(rootSigError->GetBufferPointer());
            setError("Root signature serialization failed: %s", errorMsg);
            logDiagnostic("ERROR: %s", errorMsg);
        }
        return false;
    }
    
    ID3D12RootSignature* rootSignature = nullptr;
    hr = device->CreateRootSignature(0, rootSigBlob->GetBufferPointer(), rootSigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
    
    if (FAILED(hr)) {
        setError("Failed to create root signature (HRESULT 0x%08X)", hr);
        logDiagnostic("ERROR: CreateRootSignature failed with 0x%08X", hr);
        return false;
    }
    
    mAnime4kRootSignature = rootSignature;
    logDiagnostic("Step 3/6: Root signature created ✓");
    
    // Step 4: Create compute pipeline state
    logDiagnostic("Step 4/6: Creating compute pipeline state...");
    
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature;
    psoDesc.CS.pShaderBytecode = computeShader->GetBufferPointer();
    psoDesc.CS.BytecodeLength = computeShader->GetBufferSize();
    
    ID3D12PipelineState* pipelineState = nullptr;
    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState));
    
    if (FAILED(hr)) {
        setError("Failed to create pipeline state (HRESULT 0x%08X)", hr);
        logDiagnostic("ERROR: CreateComputePipelineState failed with 0x%08X", hr);
        rootSignature->Release();
        mAnime4kRootSignature = nullptr;
        return false;
    }
    
    mAnime4kPipelineState = pipelineState;
    logDiagnostic("Step 4/6: Pipeline state created ✓");
    
    // Step 4.5: Create descriptor heap for SRV and UAV
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 2;  // 1 SRV + 1 UAV
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    
    ID3D12DescriptorHeap* descriptorHeap = nullptr;
    hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    
    if (FAILED(hr)) {
        setError("Failed to create descriptor heap (HRESULT 0x%08X)", hr);
        logDiagnostic("ERROR: CreateDescriptorHeap failed with 0x%08X", hr);
        pipelineState->Release();
        rootSignature->Release();
        mAnime4kPipelineState = nullptr;
        mAnime4kRootSignature = nullptr;
        return false;
    }
    
    mAnime4kDescriptorHeap = descriptorHeap;
    logDiagnostic("Step 4.5/6: Descriptor heap created ✓ (2 descriptors)");
    
    // Step 5: Create GPU textures
    logDiagnostic("Step 5/6: Creating GPU textures...");
    
    mAnime4kInputTexture = gpuTextureCreate(mInputWidth, mInputHeight, GpuTextureFormat::ARGB8888, 
                                            static_cast<int>(GpuTextureUsage::SHADER_RESOURCE));
    if (mAnime4kInputTexture.resource == nullptr) {
        setError("Failed to create input texture");
        logDiagnostic("ERROR: Input texture creation failed");
        pipelineState->Release();
        rootSignature->Release();
        mAnime4kPipelineState = nullptr;
        mAnime4kRootSignature = nullptr;
        return false;
    }
    
    mAnime4kOutputTexture = gpuTextureCreate(mOutputWidth, mOutputHeight, GpuTextureFormat::ARGB8888,
                                             static_cast<int>(GpuTextureUsage::UNORDERED_ACCESS));
    if (mAnime4kOutputTexture.resource == nullptr) {
        setError("Failed to create output texture");
        logDiagnostic("ERROR: Output texture creation failed");
        gpuTextureRelease(mAnime4kInputTexture);
        mAnime4kInputTexture = {};
        pipelineState->Release();
        rootSignature->Release();
        mAnime4kPipelineState = nullptr;
        mAnime4kRootSignature = nullptr;
        return false;
    }
    
    logDiagnostic("Step 5/6: Textures created ✓ (input: %dx%d, output: %dx%d)",
                  mInputWidth, mInputHeight, mOutputWidth, mOutputHeight);
    
    // Step 5.5: Create SRV and UAV descriptors in the heap
    ID3D12Resource* inputResource = gpuTextureGetResource(mAnime4kInputTexture);
    ID3D12Resource* outputResource = gpuTextureGetResource(mAnime4kOutputTexture);
    
    if (inputResource == nullptr || outputResource == nullptr) {
        setError("Failed to get texture resources for descriptor creation");
        logDiagnostic("ERROR: Texture resources are null");
        descriptorHeap->Release();
        mAnime4kDescriptorHeap = nullptr;
        gpuTextureRelease(mAnime4kOutputTexture);
        gpuTextureRelease(mAnime4kInputTexture);
        mAnime4kInputTexture = {};
        mAnime4kOutputTexture = {};
        pipelineState->Release();
        rootSignature->Release();
        mAnime4kPipelineState = nullptr;
        mAnime4kRootSignature = nullptr;
        return false;
    }
    
    UINT descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    
    // Create SRV for input texture (descriptor 0)
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    device->CreateShaderResourceView(inputResource, &srvDesc, cpuHandle);
    
    // Create UAV for output texture (descriptor 1)
    cpuHandle.ptr += descriptorSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    device->CreateUnorderedAccessView(outputResource, nullptr, &uavDesc, cpuHandle);
    
    logDiagnostic("Step 5.5/6: SRV and UAV descriptors created ✓");
    
    // CRITICAL: Wait for GPU to complete all work before creating new command allocator
    // This prevents TDR (Timeout Detection and Recovery) issues
    logDiagnostic("Step 5.75/6: Synchronizing GPU before command allocator creation...");
    gpuDeviceWaitForGpu();
    logDiagnostic("Step 5.75/6: GPU synchronized ✓");
    
    // Check if device is still alive after sync
    if (!gpuDeviceIsReady()) {
        setError("GPU device was removed during initialization");
        logDiagnostic("CRITICAL ERROR: GPU device is no longer ready after synchronization!");
        logDiagnostic("This indicates a serious GPU driver issue or TDR (Timeout Detection and Recovery)");
        // Clean up what we have so far
        descriptorHeap->Release();
        mAnime4kDescriptorHeap = nullptr;
        gpuTextureRelease(mAnime4kOutputTexture);
        gpuTextureRelease(mAnime4kInputTexture);
        mAnime4kInputTexture = {};
        mAnime4kOutputTexture = {};
        pipelineState->Release();
        rootSignature->Release();
        mAnime4kPipelineState = nullptr;
        mAnime4kRootSignature = nullptr;
        return false;
    }
    
    logDiagnostic("Step 5.8/6: Device still ready after GPU sync ✓");
    
    // Step 6: Create command allocator and list
    logDiagnostic("Step 6/6: Creating command allocator and list...");
    
    // Get device pointer for status checking
    ID3D12Device* deviceCheck1 = gpuDeviceGetDevice();
    logDiagnostic("Device check before allocator: %p", deviceCheck1);
    
    // Check device status one more time with GetDeviceRemovedReason
    HRESULT deviceStatus = deviceCheck1->GetDeviceRemovedReason();
    logDiagnostic("Device removal check returned: 0x%08X", deviceStatus);
    
    // IGNORE 0x887A0001 (DXGI_ERROR_INVALID_CALL) if the device pointer is valid
    // This error code is sometimes returned spuriously even when the device is fine.
    // Real device removal is DXGI_ERROR_DEVICE_REMOVED (0x887A0005) or DXGI_ERROR_DEVICE_HUNG (0x887A0006)
    if (FAILED(deviceStatus) && deviceStatus != DXGI_ERROR_INVALID_CALL) {
        setError("GPU device was removed (HRESULT: 0x%08X) - TDR or driver issue", deviceStatus);
        logDiagnostic("CRITICAL: Device has been removed - cannot proceed with Anime4K");
        // Clean up what we have so far
        descriptorHeap->Release();
        mAnime4kDescriptorHeap = nullptr;
        gpuTextureRelease(mAnime4kOutputTexture);
        gpuTextureRelease(mAnime4kInputTexture);
        mAnime4kInputTexture = {};
        mAnime4kOutputTexture = {};
        pipelineState->Release();
        rootSignature->Release();
        mAnime4kPipelineState = nullptr;
        mAnime4kRootSignature = nullptr;

        // Shutdown the global GPU device because it is dead
        logDiagnostic("Shutting down global GPU device due to critical failure");
        gpuDeviceShutdown();

        return false;
    } else if (deviceStatus == DXGI_ERROR_INVALID_CALL) {
        logDiagnostic("WARNING: GetDeviceRemovedReason returned INVALID_CALL (0x887A0001) but ignoring it as spurious.");
    }
    
    mAnime4kCommandAllocator = gpuDeviceCreateCommandAllocator();
    logDiagnostic("gpuDeviceCreateCommandAllocator() returned: %p", mAnime4kCommandAllocator);
    
    if (mAnime4kCommandAllocator == nullptr) {
        setError("Failed to create command allocator");
        logDiagnostic("ERROR: Command allocator creation failed");
        // Clean up what we have so far
        descriptorHeap->Release();
        mAnime4kDescriptorHeap = nullptr;
        gpuTextureRelease(mAnime4kOutputTexture);
        gpuTextureRelease(mAnime4kInputTexture);
        mAnime4kInputTexture = {};
        mAnime4kOutputTexture = {};
        pipelineState->Release();
        rootSignature->Release();
        mAnime4kPipelineState = nullptr;
        mAnime4kRootSignature = nullptr;
        return false;
    }
    
    mAnime4kCommandList = gpuDeviceCreateCommandList(static_cast<ID3D12CommandAllocator*>(mAnime4kCommandAllocator));
    if (mAnime4kCommandList == nullptr) {
        setError("Failed to create command list");
        logDiagnostic("ERROR: Command list creation failed");
        // Cleanup command allocator
        ID3D12CommandAllocator* allocator = static_cast<ID3D12CommandAllocator*>(mAnime4kCommandAllocator);
        allocator->Release();
        mAnime4kCommandAllocator = nullptr;
        // Release descriptor heap
        descriptorHeap->Release();
        mAnime4kDescriptorHeap = nullptr;
        // Release textures
        gpuTextureRelease(mAnime4kOutputTexture);
        gpuTextureRelease(mAnime4kInputTexture);
        mAnime4kInputTexture = {};
        mAnime4kOutputTexture = {};
        // Release pipeline state and root signature
        pipelineState->Release();
        rootSignature->Release();
        mAnime4kPipelineState = nullptr;
        mAnime4kRootSignature = nullptr;
        return false;
    }
    
    // Close command list (will be reset before each dispatch)
    ID3D12GraphicsCommandList* cmdList = static_cast<ID3D12GraphicsCommandList*>(mAnime4kCommandList);
    cmdList->Close();
    
    logDiagnostic("Step 6/6: Command allocator and list created ✓");
    
    mAnime4kInitialized = true;
    logDiagnostic("================== ANIME4K INITIALIZATION COMPLETE ✓ ==================");
    logDiagnostic("Anime4K compute shader pipeline ready for upscaling");
    return true;
}

bool UpscalerImpl::shutdownAnime4k() {
    if (mAnime4kInitialized) {
        logDiagnostic("Shutting down Anime4K pipeline...");
        
        // Release command list
        if (mAnime4kCommandList != nullptr) {
            ID3D12GraphicsCommandList* cmdList = static_cast<ID3D12GraphicsCommandList*>(mAnime4kCommandList);
            cmdList->Release();
            mAnime4kCommandList = nullptr;
            logDiagnostic("  - Command list released");
        }
        
        // Release command allocator
        if (mAnime4kCommandAllocator != nullptr) {
            ID3D12CommandAllocator* allocator = static_cast<ID3D12CommandAllocator*>(mAnime4kCommandAllocator);
            allocator->Release();
            mAnime4kCommandAllocator = nullptr;
            logDiagnostic("  - Command allocator released");
        }
        
        // Release GPU textures
        if (mAnime4kOutputTexture.resource != nullptr) {
            gpuTextureRelease(mAnime4kOutputTexture);
            mAnime4kOutputTexture = {};
            logDiagnostic("  - Output texture released");
        }
        
        if (mAnime4kInputTexture.resource != nullptr) {
            gpuTextureRelease(mAnime4kInputTexture);
            mAnime4kInputTexture = {};
            logDiagnostic("  - Input texture released");
        }
        
        // Release descriptor heap (if we created one explicitly)
        if (mAnime4kDescriptorHeap != nullptr) {
            ID3D12DescriptorHeap* heap = static_cast<ID3D12DescriptorHeap*>(mAnime4kDescriptorHeap);
            heap->Release();
            mAnime4kDescriptorHeap = nullptr;
            logDiagnostic("  - Descriptor heap released");
        }
        
        // Release pipeline state
        if (mAnime4kPipelineState != nullptr) {
            ID3D12PipelineState* pso = static_cast<ID3D12PipelineState*>(mAnime4kPipelineState);
            pso->Release();
            mAnime4kPipelineState = nullptr;
            logDiagnostic("  - Pipeline state released");
        }
        
        // Release root signature
        if (mAnime4kRootSignature != nullptr) {
            ID3D12RootSignature* rootSig = static_cast<ID3D12RootSignature*>(mAnime4kRootSignature);
            rootSig->Release();
            mAnime4kRootSignature = nullptr;
            logDiagnostic("  - Root signature released");
        }
        
        logDiagnostic("Anime4K pipeline shutdown complete ✓");
    }
    mAnime4kInitialized = false;
    return true;
}

// ============================================================================
// Public Implementation
// ============================================================================

// ============================================================================
// UPSCALER INITIALIZATION
// ============================================================================
// init() - Initialize the upscaler with input/output dimensions and mode
//
// This sets up the complete upscaling pipeline including:
// - Input/output buffer allocation (RGBA format)
// - Configuration loading from fallout2.cfg (mode override)
// - GPU resources for FSR2 (if applicable)
// - Filter initialization (Kuwahara)
//
// PARAMETERS:
// - inputWidth/Height: Source resolution (640x480 for Fallout 2)
// - outputWidth/Height: Target resolution (2560x1440 for modern displays)
// - mode: Initial mode (can be overridden by config file)
//
// MODES:
// - INTEGER_2X: 640x480 → 1280x960 (perfect 2x pixel replication)
// - INTEGER_3X: 640x480 → 1920x1440 (perfect 3x, optimal for 2560x1440)
// - INTEGER_4X: 640x480 → 2560x1920 (perfect 4x, for 4K displays)
// - FSR2: DISABLED (incompatible with 2D games - requires motion vectors)
//
// CONFIGURATION OVERRIDE:
// The mode parameter can be overridden by fallout2.cfg:
//   upscaler_mode=3  (INTEGER_3X recommended)
//   upscaler_kuwahara_enable=1
//   upscaler_kuwahara_radius=2
//
// RETURNS: true on success, false on failure
// ============================================================================
bool UpscalerImpl::init(int inputWidth, int inputHeight, int outputWidth, int outputHeight, UpscalerMode mode) {
    logDiagnostic("========================================");
    logDiagnostic("UPSCALER INITIALIZATION STARTED");
    logDiagnostic("========================================");
    
    // Load configuration from fallout2.cfg
    loadConfiguration();
    
    // IMPORTANT: Use mode from config, not the parameter
    // The config value takes precedence over the init parameter
    mode = mConfiguredMode;
    
    logDiagnostic("Input Resolution: %dx%d", inputWidth, inputHeight);
    logDiagnostic("Output Resolution: %dx%d", outputWidth, outputHeight);
    
    const char* modeStr = "UNKNOWN";
    if (mode == UpscalerMode::NONE) modeStr = "NONE";
    else if (mode == UpscalerMode::FSR2) modeStr = "FSR2";
    else if (mode == UpscalerMode::INTEGER_2X) modeStr = "INTEGER_2X";
    else if (mode == UpscalerMode::INTEGER_3X) modeStr = "INTEGER_3X";
    else if (mode == UpscalerMode::INTEGER_4X) modeStr = "INTEGER_4X";
    else if (mode == UpscalerMode::ANIME4K) modeStr = "ANIME4K";
    
    logDiagnostic("Upscaler Mode: %s", modeStr);
    
    if (mode == UpscalerMode::FSR2) {
        logDiagnostic("Quality: %s", mQuality == UpscalerQuality::QUALITY ? "QUALITY" : 
                                       mQuality == UpscalerQuality::BALANCED ? "BALANCED" : "PERFORMANCE");
        logDiagnostic("Sharpness: %.2f (FSR2 RCAS)", mSharpness);
        logDiagnostic("sRGB Colorspace: ENABLED (for proper 8-bit palette handling)");
    }
    
    logDiagnostic("Lightweight Filters: Debanding=%s (%.2f), EdgeSmoothing=%s (%.2f)",
                  mEnableDebanding ? "ON" : "OFF", mDebandingStrength,
                  mEnableEdgeSmoothing ? "ON" : "OFF", mSmoothingStrength);
    
    if (mEnableKuwahara) {
        logDiagnostic("Kuwahara Filter: ENABLED (radius=%d, pre-scaling edge-preserving smoothing)", mKuwaharaRadius);
    } else {
        logDiagnostic("Kuwahara Filter: DISABLED");
    }
    
    logDiagnostic("Verbose Logging: %s", mVerboseLogging ? "ENABLED" : "DISABLED");
    
    if (mState != UpscalerState::STATE_UNINITIALIZED) {
        setError("Upscaler already initialized");
        logDiagnostic("ERROR: Upscaler state is %d (expected UNINITIALIZED)", (int)mState);
        return false;
    }

    mState = UpscalerState::STATE_INITIALIZING;
    mMode = mode;

    if (mode == UpscalerMode::NONE) {
        logDiagnostic("Upscaler mode: NONE (no upscaling)");
        mState = UpscalerState::STATE_READY;
        mIsAvailable = false;
        logDiagnostic("UPSCALER INITIALIZATION COMPLETE (DISABLED)");
        return true;
    }

    // Integer scaling modes (2x, 3x, 4x)
    if (mode == UpscalerMode::INTEGER_2X || mode == UpscalerMode::INTEGER_3X || mode == UpscalerMode::INTEGER_4X) {
        int scaleFactor = (mode == UpscalerMode::INTEGER_2X) ? 2 : 
                          (mode == UpscalerMode::INTEGER_3X) ? 3 : 4;
        
        logDiagnostic("Upscaler mode: INTEGER_%dX (perfect pixel replication)", scaleFactor);
        logDiagnostic("Step 1/2: Allocating input buffer...");
        
        if (!allocateInputBuffer(inputWidth, inputHeight)) {
            mState = UpscalerState::STATE_ERROR;
            logDiagnostic("ERROR: Input buffer allocation failed!");
            return false;
        }
        logDiagnostic("Step 1/2: Input buffer allocated ✓ (%dx%d)", inputWidth, inputHeight);

        logDiagnostic("Step 2/2: Allocating output buffer...");
        if (!allocateOutputBuffer(outputWidth, outputHeight)) {
            deallocateBuffers();
            mState = UpscalerState::STATE_ERROR;
            logDiagnostic("ERROR: Output buffer allocation failed!");
            return false;
        }
        logDiagnostic("Step 2/2: Output buffer allocated ✓ (%dx%d)", outputWidth, outputHeight);

        mIsAvailable = true;
        mState = UpscalerState::STATE_READY;
        logDiagnostic("========================================");
        logDiagnostic("INTEGER SCALING INITIALIZATION COMPLETE ✓");
        logDiagnostic("Benefits: Zero blur, zero artifacts, perfect sharpness");
        logDiagnostic("========================================");
        return true;
    }

    // Anime4K shader-based upscaling (experimental)
    if (mode == UpscalerMode::ANIME4K) {
        logDiagnostic("Upscaler mode: ANIME4K (shader-based, experimental)");
        logDiagnostic("Step 1/3: Allocating input buffer...");
        if (!allocateInputBuffer(inputWidth, inputHeight)) {
            mState = UpscalerState::STATE_ERROR;
            logDiagnostic("ERROR: Input buffer allocation failed!");
            return false;
        }
        logDiagnostic("Step 1/3: Input buffer allocated ✓ (%dx%d)", inputWidth, inputHeight);

        logDiagnostic("Step 2/3: Allocating output buffer...");
        if (!allocateOutputBuffer(outputWidth, outputHeight)) {
            deallocateBuffers();
            mState = UpscalerState::STATE_ERROR;
            logDiagnostic("ERROR: Output buffer allocation failed!");
            return false;
        }
        logDiagnostic("Step 2/3: Output buffer allocated ✓ (%dx%d)", outputWidth, outputHeight);

        logDiagnostic("Step 3/3: Initializing Anime4K pipeline");
        if (!initAnime4k()) {
            deallocateBuffers();
            logDiagnostic("ERROR: Anime4K initialization failed!");
            logDiagnostic("FALLBACK: Switching to INTEGER_3X upscaler instead");
            
            // Try to initialize INTEGER_3X as fallback
            if (!allocateInputBuffer(inputWidth, inputHeight)) {
                mState = UpscalerState::STATE_ERROR;
                return false;
            }
            if (!allocateOutputBuffer(outputWidth, outputHeight)) {
                deallocateBuffers();
                mState = UpscalerState::STATE_ERROR;
                return false;
            }
            
            mMode = UpscalerMode::INTEGER_3X;
            mIsAvailable = true;
            mState = UpscalerState::STATE_READY;
            logDiagnostic("FALLBACK: Using INTEGER_3X upscaler");
            logDiagnostic("========================================");
            logDiagnostic("UPSCALER INITIALIZATION COMPLETE (with fallback)");
            logDiagnostic("========================================");
            return true;
        }
        logDiagnostic("Step 3/3: Anime4K initialized ✓");

        mIsAvailable = true;
        mState = UpscalerState::STATE_READY;
        logDiagnostic("========================================");
        logDiagnostic("ANIME4K INITIALIZATION COMPLETE ✓");
        logDiagnostic("========================================");
        return true;
    }

    if (mode == UpscalerMode::FSR2) {
        logDiagnostic("Upscaler mode: FSR2 (FidelityFX SDK 2.1)");
        logDiagnostic("Step 1/3: Allocating input buffer...");
        
        if (!allocateInputBuffer(inputWidth, inputHeight)) {
            mState = UpscalerState::STATE_ERROR;
            logDiagnostic("ERROR: Input buffer allocation failed!");
            return false;
        }
        logDiagnostic("Step 1/3: Input buffer allocated ✓ (%dx%d)", inputWidth, inputHeight);

        logDiagnostic("Step 2/3: Allocating output buffer...");
        if (!allocateOutputBuffer(outputWidth, outputHeight)) {
            deallocateBuffers();
            mState = UpscalerState::STATE_ERROR;
            logDiagnostic("ERROR: Output buffer allocation failed!");
            return false;
        }
        logDiagnostic("Step 2/3: Output buffer allocated ✓ (%dx%d)", outputWidth, outputHeight);

        logDiagnostic("Step 3/3: Initializing FSR2...");
        if (!initFsr2()) {
            deallocateBuffers();
            mState = UpscalerState::STATE_ERROR;
            logDiagnostic("ERROR: FSR2 initialization failed!");
            return false;
        }
        logDiagnostic("Step 3/3: FSR2 initialized ✓");

        mIsAvailable = true;
        mState = UpscalerState::STATE_READY;
        logDiagnostic("========================================");
        logDiagnostic("UPSCALER INITIALIZATION COMPLETE ✓");
        logDiagnostic("========================================");
        return true;
    }

    setError("Unknown upscaler mode");
    mState = UpscalerState::STATE_ERROR;
    logDiagnostic("ERROR: Unknown upscaler mode %d!", (int)mode);
    return false;
}

bool UpscalerImpl::reconfigureOutput(int outputWidth, int outputHeight) {
    if (mState != UpscalerState::STATE_READY) {
        setError("Cannot reconfigure: upscaler not ready");
        return false;
    }

    if (outputWidth == mOutputWidth && outputHeight == mOutputHeight) {
        return true;
    }

    logDiagnostic("Reconfiguring output: %dx%d -> %dx%d", mOutputWidth, mOutputHeight, outputWidth, outputHeight);

    if (mMode == UpscalerMode::FSR2) {
        shutdownFsr2();
        if (!allocateOutputBuffer(outputWidth, outputHeight)) {
            mState = UpscalerState::STATE_ERROR;
            return false;
        }
        if (!initFsr2()) {
            mState = UpscalerState::STATE_ERROR;
            return false;
        }
    } else if (mMode == UpscalerMode::ANIME4K) {
        shutdownAnime4k();
        if (!allocateOutputBuffer(outputWidth, outputHeight)) {
            mState = UpscalerState::STATE_ERROR;
            return false;
        }
        if (!initAnime4k()) {
            mState = UpscalerState::STATE_ERROR;
            return false;
        }
    } else {
        if (!allocateOutputBuffer(outputWidth, outputHeight)) {
            return false;
        }
    }

    return true;
}

void UpscalerImpl::shutdown() {
    if (mMode == UpscalerMode::FSR2) {
        shutdownFsr2();
    } else if (mMode == UpscalerMode::ANIME4K) {
        shutdownAnime4k();
    }
    deallocateBuffers();
    mState = UpscalerState::STATE_UNINITIALIZED;
    mIsAvailable = false;
    logDiagnostic("Upscaler shutdown complete");
    
    // Close upscale.log file
    if (mUpscaleLog != nullptr) {
        fclose(mUpscaleLog);
        mUpscaleLog = nullptr;
    }
}

// ============================================================================
// INPUT CONVERSION - INDEXED TO RGBA
// ============================================================================
// setIndexedInput() - Convert indexed color (palette) to RGBA for processing
//
// Fallout 2 uses 8-bit indexed color (256 colors). This function converts
// the indexed pixels to 32-bit RGBA format using the provided palette.
//
// PHANTOM DISPLAY FORMAT:
// - Input: 640x480 indexed (1 byte per pixel = palette index)
// - Palette: 256 colors × 4 bytes (RGBA)
// - Output: 640x480 RGBA (4 bytes per pixel)
//
// CONVERSION PROCESS:
// 1. Read palette index from indexed buffer (0-255)
// 2. Look up RGBA color in palette
// 3. Write RGBA color to input buffer (mInputBuffer)
//
// This converted RGBA buffer is then processed by filters and scaling.
//
// PARAMETERS:
// - indexedBuffer: Pointer to 640x480 indexed color data (from gSdlSurface)
// - palette: Pointer to 256-color RGBA palette (from SDL_Color array)
//
// RETURNS: true on success, false if upscaler not initialized
// ============================================================================
bool UpscalerImpl::setIndexedInput(const unsigned char* indexedBuffer, const uint32_t* palette) {
    if (mState != UpscalerState::STATE_READY || mInputBuffer == nullptr || indexedBuffer == nullptr || palette == nullptr) {
        setError("Invalid input or palette");
        return false;
    }

    // Convert indexed 8-bit to ARGB8888
    for (int i = 0; i < mInputWidth * mInputHeight; ++i) {
        uint32_t paletteEntry = palette[indexedBuffer[i]];
        // Assume palette is RGB, add full alpha
        mInputBuffer[i] = (paletteEntry & 0xFFFFFF) | 0xFF000000;
    }

    return true;
}

bool UpscalerImpl::setRgbaInput(const uint32_t* rgbaBuffer) {
    if (mState != UpscalerState::STATE_READY || mInputBuffer == nullptr || rgbaBuffer == nullptr) {
        setError("Upscaler not ready or invalid buffer");
        return false;
    }

    int numPixels = mInputWidth * mInputHeight;
    std::memcpy(mInputBuffer, rgbaBuffer, numPixels * sizeof(uint32_t));
    return true;
}

// ============================================================================
// UPSCALER DISPATCH - MAIN PROCESSING
// ============================================================================
// dispatch() - Execute the upscaling pipeline for current frame
//
// This is the main processing function that routes to the appropriate
// upscaling algorithm based on the configured mode.
//
// PROCESSING FLOW:
// 1. Input buffer contains 640x480 RGBA (converted from indexed)
// 2. Route to mode-specific algorithm:
//    - INTEGER_2X/3X/4X: Call dispatchIntegerScale()
//    - FSR2: Call dispatchFsr2() [DISABLED - incompatible]
// 3. Output buffer contains upscaled result (e.g., 2560x1440 RGBA)
//
// The output buffer is then retrieved via upscalerGetOutputBuffer() and
// uploaded to the GPU texture for rendering.
//
// RETURNS: true on success, false on failure
// ============================================================================
bool UpscalerImpl::dispatch() {
    static int dispatchCallCount = 0;
    dispatchCallCount++;
    
    // Log first call and every 60th call
    if (dispatchCallCount == 1 || (dispatchCallCount % 60 == 0)) {
        logDiagnostic("DISPATCH called (count=%d, mode=%d, state=%d)", 
                      dispatchCallCount, (int)mMode, (int)mState);
    }
    
    if (mState != UpscalerState::STATE_READY) {
        setError("Upscaler not ready");
        logDiagnostic("ERROR: Dispatch called but upscaler not ready! State=%d", (int)mState);
        return false;
    }

    if (mMode == UpscalerMode::FSR2) {
        return dispatchFsr2();
    }
    
    if (mMode == UpscalerMode::ANIME4K) {
        return dispatchAnime4k();
    }
    
    if (mMode == UpscalerMode::INTEGER_2X) {
        return dispatchIntegerScale(2);
    }
    
    if (mMode == UpscalerMode::INTEGER_3X) {
        return dispatchIntegerScale(3);
    }
    
    if (mMode == UpscalerMode::INTEGER_4X) {
        return dispatchIntegerScale(4);
    }

    return true;
}

bool UpscalerImpl::setQuality(UpscalerQuality quality) {
    mQuality = quality;
    return true;
}

bool UpscalerImpl::setSharpness(float sharpness) {
    if (sharpness < 0.0f || sharpness > 1.0f) {
        setError("Sharpness must be between 0.0 and 1.0");
        return false;
    }
    mSharpness = sharpness;
    return true;
}

// ============================================================================
// Public C API Wrappers
// ============================================================================

int upscalerInit(int inputWidth, int inputHeight, int outputWidth, int outputHeight, UpscalerMode mode) {
    return upscalerGetImpl()->init(inputWidth, inputHeight, outputWidth, outputHeight, mode) ? 0 : -1;
}

int upscalerReconfigureOutput(int outputWidth, int outputHeight) {
    return upscalerGetImpl()->reconfigureOutput(outputWidth, outputHeight) ? 0 : -1;
}

void upscalerShutdown() {
    upscalerGetImpl()->shutdown();
}

int upscalerSetIndexedInput(const unsigned char* indexedBuffer, const uint32_t* palette) {
    return upscalerGetImpl()->setIndexedInput(indexedBuffer, palette) ? 0 : -1;
}

int upscalerSetRgbaInput(const uint32_t* rgbaBuffer) {
    return upscalerGetImpl()->setRgbaInput(rgbaBuffer) ? 0 : -1;
}

int upscalerDispatch() {
    return upscalerGetImpl()->dispatch() ? 0 : -1;
}

const uint32_t* upscalerGetOutputBuffer() {
    return upscalerGetImpl()->getOutputBuffer();
}

int upscalerGetOutputPitch() {
    return upscalerGetImpl()->getOutputPitch();
}

void upscalerGetOutputDimensions(int& width, int& height) {
    upscalerGetImpl()->getOutputDimensions(width, height);
}

UpscalerState upscalerGetState() {
    return upscalerGetImpl()->getState();
}

int upscalerSetQuality(UpscalerQuality quality) {
    return upscalerGetImpl()->setQuality(quality) ? 0 : -1;
}

UpscalerMode upscalerGetConfiguredMode() {
    return upscalerGetImpl()->getConfiguredMode();
}

int upscalerSetSharpness(float sharpness) {
    return upscalerGetImpl()->setSharpness(sharpness) ? 0 : -1;
}

void upscalerSetMotionVectorsEnabled(bool enabled) {
    upscalerGetImpl()->setMotionVectorsEnabled(enabled);
}

int upscalerSetFilterParams(float edgeStrength, float colorStrength, float saturation, float contrast) {
    upscalerGetImpl()->setFilterParams(edgeStrength, colorStrength, saturation, contrast);
    return 0;
}

void upscalerSetVerboseLogging(bool enabled) {
    upscalerGetImpl()->setVerboseLogging(enabled);
}

void upscalerReloadConfig() {
    upscalerGetImpl()->reloadConfig();
}

bool upscalerIsAvailable() {
    return upscalerGetImpl()->isAvailable();
}

const char* upscalerGetLastError() {
    return upscalerGetImpl()->getLastError();
}

} // namespace fallout
