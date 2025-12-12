#include "upscaler.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include "diagnostics.h"
#include "gpu_device.h"
#include "gpu_texture.h"
#include "memory.h"

// FidelityFX SDK 2.1 headers (optional, only if FALLOUT_HAS_FSR2 is defined)
#ifdef FALLOUT_HAS_FSR2
    #include "ffx_upscale.h"
    #include "ffx_api.h"
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

    bool isAvailable() const { return mIsAvailable; }
    const char* getLastError() const { return mLastError; }

private:
    UpscalerImpl() = default;

    void logDiagnostic(const char* format, ...);
    void setError(const char* format, ...);
    
    bool initFsr2();
    bool shutdownFsr2();
    bool dispatchFsr2();

    bool allocateInputBuffer(int width, int height);
    bool allocateOutputBuffer(int width, int height);
    void deallocateBuffers();

    void calculateJitter(float& outX, float& outY);

    // State variables
    UpscalerState mState = UpscalerState::UNINITIALIZED;
    UpscalerMode mMode = UpscalerMode::NONE;
    UpscalerQuality mQuality = UpscalerQuality::BALANCED;
    bool mIsAvailable = false;

    // Dimensions
    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;

    // Buffers (ARGB8888 format)
    uint32_t* mInputBuffer = nullptr;
    uint32_t* mOutputBuffer = nullptr;
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

    // Error tracking
    char mLastError[ERROR_MSG_SIZE] = {};
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
        setError("Failed to allocate input buffer (%zu bytes)", bufferSize);
        return false;
    }

    logDiagnostic("Allocated input buffer: %dx%d (%zu bytes)", width, height, bufferSize);
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
    ffxCreateContextDescUpscale createDesc = {};
    createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    createDesc.header.pNext = nullptr;
    
    // Set flags for FSR2 behavior
    createDesc.flags = 0;
    // createDesc.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;  // Enable if needed
    // createDesc.flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;       // Enable if needed
    
    // Set maximum render and upscale sizes
    createDesc.maxRenderSize.width = mInputWidth;
    createDesc.maxRenderSize.height = mInputHeight;
    createDesc.maxUpscaleSize.width = mOutputWidth;
    createDesc.maxUpscaleSize.height = mOutputHeight;
    createDesc.fpMessage = nullptr;  // No message callback for now
    
    logDiagnostic("  Context descriptor prepared: maxRender=%dx%d, maxUpscale=%dx%d",
        createDesc.maxRenderSize.width, createDesc.maxRenderSize.height,
        createDesc.maxUpscaleSize.width, createDesc.maxUpscaleSize.height);
    
    ffxReturnCode_t fsr2Result = ffxCreateContext(&mFsrContext, (ffxCreateContextDescHeader*)&createDesc, nullptr);
    if (fsr2Result != FFX_API_RETURN_OK) {
        setError("Failed to create FSR2 context (error code: %d)", fsr2Result);
        logDiagnostic("ERROR: ffxCreateContext failed with code %d!", fsr2Result);
        gpuTextureRelease(mGpuInputTexture);
        gpuTextureRelease(mGpuOutputTexture);
        mGpuInputTexture = { nullptr };
        mGpuOutputTexture = { nullptr };
        mGpuDevice = nullptr;
        mGpuCommandQueue = nullptr;
        mGpuCommandAllocator = nullptr;
        return false;
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
        
        logDiagnostic("  Setting up input texture binding...");
        // Bind input texture (color) - prepare FfxApiResource
        ID3D12Resource* inputResource = gpuTextureGetResource(mGpuInputTexture);
        if (inputResource == nullptr) {
            setError("Failed to get input texture resource for FSR2 dispatch");
            logDiagnostic("ERROR: Could not get input texture resource!");
            return false;
        }
        dispatchDesc.color.resource = inputResource;
        dispatchDesc.color.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        logDiagnostic("    Input resource: %p ✓", inputResource);
        
        logDiagnostic("  Setting up output texture binding...");
        // Bind output texture - prepare FfxApiResource
        ID3D12Resource* outputResource = gpuTextureGetResource(mGpuOutputTexture);
        if (outputResource == nullptr) {
            setError("Failed to get output texture resource for FSR2 dispatch");
            logDiagnostic("ERROR: Could not get output texture resource!");
            return false;
        }
        dispatchDesc.output.resource = outputResource;
        dispatchDesc.output.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
        logDiagnostic("    Output resource: %p ✓", outputResource);
        
        // Set FSR2 parameters
        logDiagnostic("  Configuring FSR2 parameters...");
        float jitterX = 0.0f, jitterY = 0.0f;
        calculateJitter(jitterX, jitterY);
        dispatchDesc.jitterOffset.x = jitterX;
        dispatchDesc.jitterOffset.y = jitterY;
        dispatchDesc.motionVectorScale.x = 1.0f;  // Motion vectors in pixel space
        dispatchDesc.motionVectorScale.y = 1.0f;
        dispatchDesc.preExposure = 1.0f;          // Default pre-exposure
        dispatchDesc.sharpness = mSharpness;      // Quality-based sharpness
        dispatchDesc.enableSharpening = (mSharpness > 0.0f);
        logDiagnostic("    Jitter offset: (%.4f, %.4f)", jitterX, jitterY);
        logDiagnostic("    Sharpness: %.2f, Enabled: %s", mSharpness, dispatchDesc.enableSharpening ? "yes" : "no");
        
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
        
        // Dispatch FSR2 upscaling (cast to base header for API compatibility)
        logDiagnostic("  Invoking ffxDispatch()...");
        ffxReturnCode_t result = ffxDispatch(&mFsrContext, (ffxDispatchDescHeader*)&dispatchDesc);
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
    
    mFrameIndex++;
    logDiagnostic("================== FSR2 DISPATCH COMPLETE ✓ (Frame %d) ==================", mFrameIndex - 1);
    return true;
}

// ============================================================================
// Public Implementation
// ============================================================================

bool UpscalerImpl::init(int inputWidth, int inputHeight, int outputWidth, int outputHeight, UpscalerMode mode) {
    logDiagnostic("========================================");
    logDiagnostic("UPSCALER INITIALIZATION");
    logDiagnostic("========================================");
    logDiagnostic("Input: %dx%d, Output: %dx%d, Mode: %d", inputWidth, inputHeight, outputWidth, outputHeight, (int)mode);
    
    if (mState != UpscalerState::UNINITIALIZED) {
        setError("Upscaler already initialized");
        logDiagnostic("ERROR: Upscaler state is %d (expected UNINITIALIZED)", (int)mState);
        return false;
    }

    mState = UpscalerState::INITIALIZING;
    mMode = mode;

    if (mode == UpscalerMode::NONE) {
        logDiagnostic("Upscaler mode: NONE (no upscaling)");
        mState = UpscalerState::READY;
        mIsAvailable = false;
        logDiagnostic("UPSCALER INITIALIZATION COMPLETE (DISABLED)");
        return true;
    }

    if (mode == UpscalerMode::FSR2) {
        logDiagnostic("Upscaler mode: FSR2 (FidelityFX SDK 2.1)");
        logDiagnostic("Step 1/3: Allocating input buffer...");
        
        if (!allocateInputBuffer(inputWidth, inputHeight)) {
            mState = UpscalerState::ERROR;
            logDiagnostic("ERROR: Input buffer allocation failed!");
            return false;
        }
        logDiagnostic("Step 1/3: Input buffer allocated ✓ (%dx%d)", inputWidth, inputHeight);

        logDiagnostic("Step 2/3: Allocating output buffer...");
        if (!allocateOutputBuffer(outputWidth, outputHeight)) {
            deallocateBuffers();
            mState = UpscalerState::ERROR;
            logDiagnostic("ERROR: Output buffer allocation failed!");
            return false;
        }
        logDiagnostic("Step 2/3: Output buffer allocated ✓ (%dx%d)", outputWidth, outputHeight);

        logDiagnostic("Step 3/3: Initializing FSR2...");
        if (!initFsr2()) {
            deallocateBuffers();
            mState = UpscalerState::ERROR;
            logDiagnostic("ERROR: FSR2 initialization failed!");
            return false;
        }
        logDiagnostic("Step 3/3: FSR2 initialized ✓");

        mIsAvailable = true;
        mState = UpscalerState::READY;
        logDiagnostic("========================================");
        logDiagnostic("UPSCALER INITIALIZATION COMPLETE ✓");
        logDiagnostic("========================================");
        return true;
    }

    setError("Unknown upscaler mode");
    mState = UpscalerState::ERROR;
    logDiagnostic("ERROR: Unknown upscaler mode %d!", (int)mode);
    return false;
}

bool UpscalerImpl::reconfigureOutput(int outputWidth, int outputHeight) {
    if (mState != UpscalerState::READY) {
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
            mState = UpscalerState::ERROR;
            return false;
        }
        if (!initFsr2()) {
            mState = UpscalerState::ERROR;
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
    }
    deallocateBuffers();
    mState = UpscalerState::UNINITIALIZED;
    mIsAvailable = false;
    logDiagnostic("Upscaler shutdown complete");
}

bool UpscalerImpl::setIndexedInput(const unsigned char* indexedBuffer, const uint32_t* palette) {
    if (mState != UpscalerState::READY || mInputBuffer == nullptr || indexedBuffer == nullptr || palette == nullptr) {
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
    if (mState != UpscalerState::READY || mInputBuffer == nullptr || rgbaBuffer == nullptr) {
        setError("Upscaler not ready or invalid buffer");
        return false;
    }

    int numPixels = mInputWidth * mInputHeight;
    std::memcpy(mInputBuffer, rgbaBuffer, numPixels * sizeof(uint32_t));
    return true;
}

bool UpscalerImpl::dispatch() {
    if (mState != UpscalerState::READY) {
        setError("Upscaler not ready");
        return false;
    }

    if (mMode == UpscalerMode::FSR2) {
        return dispatchFsr2();
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

int upscalerSetSharpness(float sharpness) {
    return upscalerGetImpl()->setSharpness(sharpness) ? 0 : -1;
}

void upscalerSetMotionVectorsEnabled(bool enabled) {
    upscalerGetImpl()->setMotionVectorsEnabled(enabled);
}

bool upscalerIsAvailable() {
    return upscalerGetImpl()->isAvailable();
}

const char* upscalerGetLastError() {
    return upscalerGetImpl()->getLastError();
}

} // namespace fallout
