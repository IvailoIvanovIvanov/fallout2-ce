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
    logDiagnostic("FSR2 initialization (Phase 3 - GPU device binding)");
    
    // Phase 3: Get GPU device context for FSR2 compute operations
    if (!gpuDeviceIsReady()) {
        setError("GPU device not initialized, cannot initialize FSR2");
        return false;
    }
    
    mGpuDevice = gpuDeviceGetDevice();
    mGpuCommandQueue = gpuDeviceGetCommandQueue();
    
    if (mGpuDevice == nullptr || mGpuCommandQueue == nullptr) {
        setError("Failed to acquire GPU device context");
        return false;
    }
    
    // Create command allocator for FSR2 compute operations
    mGpuCommandAllocator = gpuDeviceCreateCommandAllocator();
    if (mGpuCommandAllocator == nullptr) {
        setError("Failed to create GPU command allocator");
        mGpuDevice = nullptr;
        mGpuCommandQueue = nullptr;
        return false;
    }
    
    logDiagnostic("GPU device context acquired: Device=%p, Queue=%p, Allocator=%p",
        mGpuDevice, mGpuCommandQueue, mGpuCommandAllocator);
    
    // Phase 4: Create GPU textures for input and output
    mGpuInputTexture = gpuTextureCreate(
        mInputWidth,
        mInputHeight,
        GpuTextureFormat::ARGB8888,
        static_cast<int>(GpuTextureUsage::SHADER_RESOURCE)
    );
    
    if (mGpuInputTexture.resource == nullptr) {
        setError("Failed to create GPU input texture");
        mGpuDevice = nullptr;
        mGpuCommandQueue = nullptr;
        mGpuCommandAllocator = nullptr;
        return false;
    }
    
    mGpuOutputTexture = gpuTextureCreate(
        mOutputWidth,
        mOutputHeight,
        GpuTextureFormat::ARGB8888,
        static_cast<int>(GpuTextureUsage::UNORDERED_ACCESS)
    );
    
    if (mGpuOutputTexture.resource == nullptr) {
        setError("Failed to create GPU output texture");
        gpuTextureRelease(mGpuInputTexture);
        mGpuInputTexture = { nullptr };
        mGpuDevice = nullptr;
        mGpuCommandQueue = nullptr;
        mGpuCommandAllocator = nullptr;
        return false;
    }
    
    logDiagnostic("GPU textures created: Input=%dx%d, Output=%dx%d",
        mInputWidth, mInputHeight, mOutputWidth, mOutputHeight);
    
    // Phase 2 TODO: Create FSR2 context with ffxCreateContext() using GPU device
    mFsrContextInitialized = true;
    return true;
}

bool UpscalerImpl::shutdownFsr2() {
    // Phase 2 TODO: Implement actual FSR2 context destruction with ffxDestroyContext()
    
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
    // Phase 3-4: Dispatch GPU compute with FSR2 context
    if (!mFsrContextInitialized || mGpuDevice == nullptr || mGpuCommandQueue == nullptr) {
        setError("FSR2 not properly initialized, cannot dispatch");
        return false;
    }
    
    if (mGpuInputTexture.resource == nullptr || mGpuOutputTexture.resource == nullptr) {
        setError("GPU textures not created, cannot dispatch");
        return false;
    }
    
    // Phase 4: Upload input buffer to GPU texture
    if (!gpuTextureUpload(mGpuInputTexture, mInputBuffer, mInputWidth * mInputHeight * sizeof(uint32_t))) {
        setError("Failed to upload input texture");
        return false;
    }
    
    // Phase 4 TODO: Create command list for this dispatch
    // if (mGpuCommandList == nullptr) {
    //     mGpuCommandList = gpuDeviceCreateCommandList(mGpuCommandAllocator);
    //     if (mGpuCommandList == nullptr) {
    //         setError("Failed to create GPU command list");
    //         return false;
    //     }
    // }
    
    // Phase 4 TODO: Implement actual GPU upscaling dispatch with ffxDispatch()
    // This would include:
    // 1. Binding input texture (mGpuInputTexture) as shader resource
    // 2. Setting up FSR2 parameters (jitter, exposure, etc.)
    // 3. Recording GPU compute commands using mGpuCommandList
    // 4. Executing command list with gpuDeviceExecuteCommandList()
    // 5. Waiting for GPU with gpuDeviceWaitForGpu()
    // 6. Reading back output texture to mOutputBuffer via gpuTextureDownload()
    
    logDiagnostic("FSR2 dispatch: GPU compute pending (Phase 4)");
    mFrameIndex++;
    return true;
}

// ============================================================================
// Public Implementation
// ============================================================================

bool UpscalerImpl::init(int inputWidth, int inputHeight, int outputWidth, int outputHeight, UpscalerMode mode) {
    if (mState != UpscalerState::UNINITIALIZED) {
        setError("Upscaler already initialized");
        return false;
    }

    mState = UpscalerState::INITIALIZING;
    mMode = mode;

    if (mode == UpscalerMode::NONE) {
        logDiagnostic("Upscaler mode: NONE (no upscaling)");
        mState = UpscalerState::READY;
        mIsAvailable = false;
        return true;
    }

    if (mode == UpscalerMode::FSR2) {
        logDiagnostic("Upscaler mode: FSR2 (FidelityFX SDK 2.1)");
        
        if (!allocateInputBuffer(inputWidth, inputHeight)) {
            mState = UpscalerState::ERROR;
            return false;
        }

        if (!allocateOutputBuffer(outputWidth, outputHeight)) {
            deallocateBuffers();
            mState = UpscalerState::ERROR;
            return false;
        }

        if (!initFsr2()) {
            deallocateBuffers();
            mState = UpscalerState::ERROR;
            return false;
        }

        mIsAvailable = true;
        mState = UpscalerState::READY;
        logDiagnostic("Upscaler initialized successfully");
        return true;
    }

    setError("Unknown upscaler mode");
    mState = UpscalerState::ERROR;
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
