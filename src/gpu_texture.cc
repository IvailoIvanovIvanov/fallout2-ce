#include "gpu_texture.h"

#include <d3d12.h>
#include <wrl/client.h>
#include "diagnostics.h"
#include "gpu_device.h"

using Microsoft::WRL::ComPtr;

namespace fallout {

/**
 * @brief Internal texture metadata
 */
struct TextureMetadata {
    int width;
    int height;
    GpuTextureFormat format;
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Resource> uploadBuffer;      // For CPU->GPU transfers
    ComPtr<ID3D12Resource> readbackBuffer;    // For GPU->CPU transfers
};

/**
 * @brief Get DXGI format from GpuTextureFormat
 */
static DXGI_FORMAT getD3dFormat(GpuTextureFormat format) {
    switch (format) {
        case GpuTextureFormat::ARGB8888:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case GpuTextureFormat::RGBA8888:
            return DXGI_FORMAT_R8G8B8A8_UNORM;  // Same format, different interpretation
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

/**
 * @brief Get resource usage flags from GpuTextureUsage
 */
static D3D12_RESOURCE_FLAGS getD3dResourceFlags(int usage) {
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    
    if (usage & static_cast<int>(GpuTextureUsage::RENDER_TARGET)) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (usage & static_cast<int>(GpuTextureUsage::UNORDERED_ACCESS)) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    
    return flags;
}

GpuTextureHandle gpuTextureCreate(int width, int height, GpuTextureFormat format, int usage)
{
    GpuTextureHandle invalidHandle = { nullptr };
    
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
            "Invalid texture dimensions: %dx%d", width, height);
        return invalidHandle;
    }
    
    ID3D12Device* device = gpuDeviceGetDevice();
    if (device == nullptr) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
            "GPU device not initialized");
        return invalidHandle;
    }
    
    auto metadata = new (std::nothrow) TextureMetadata();
    if (metadata == nullptr) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
            "Failed to allocate texture metadata");
        return invalidHandle;
    }
    
    metadata->width = width;
    metadata->height = height;
    metadata->format = format;
    
    // Create texture resource
    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = width;
    resourceDesc.Height = height;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = getD3dFormat(format);
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = getD3dResourceFlags(usage);
    
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;
    
    if (FAILED(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(metadata->resource.ReleaseAndGetAddressOf())))) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
            "Failed to create GPU texture resource (%dx%d)", width, height);
        delete metadata;
        return invalidHandle;
    }
    
    diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
        "Created GPU texture: %dx%d, format=%d", width, height, static_cast<int>(format));
    
    GpuTextureHandle handle;
    handle.resource = metadata;
    return handle;
}

void gpuTextureRelease(GpuTextureHandle handle)
{
    if (handle.resource == nullptr) {
        return;
    }
    
    auto metadata = static_cast<TextureMetadata*>(handle.resource);
    diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
        "Releasing GPU texture: %dx%d", metadata->width, metadata->height);
    
    metadata->resource = nullptr;
    metadata->uploadBuffer = nullptr;
    metadata->readbackBuffer = nullptr;
    delete metadata;
}

bool gpuTextureUpload(GpuTextureHandle handle, const void* data, int dataSize)
{
    if (handle.resource == nullptr || data == nullptr || dataSize <= 0) {
        return false;
    }
    
    auto metadata = static_cast<TextureMetadata*>(handle.resource);
    ID3D12Device* device = gpuDeviceGetDevice();
    ID3D12CommandQueue* queue = gpuDeviceGetCommandQueue();
    
    if (device == nullptr || queue == nullptr || metadata->resource == nullptr) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
            "Cannot upload: GPU device or texture not initialized");
        return false;
    }
    
    // Create upload buffer if not already created
    if (metadata->uploadBuffer == nullptr) {
        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Alignment = 0;
        uploadDesc.Width = dataSize;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.SampleDesc.Quality = 0;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        uploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        
        D3D12_HEAP_PROPERTIES uploadHeapProps = {};
        uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        uploadHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        uploadHeapProps.CreationNodeMask = 1;
        uploadHeapProps.VisibleNodeMask = 1;
        
        if (FAILED(device->CreateCommittedResource(
            &uploadHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(metadata->uploadBuffer.ReleaseAndGetAddressOf())))) {
            diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
                "Failed to create upload buffer");
            return false;
        }
    }
    
    // Copy data to upload buffer
    void* uploadData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    if (FAILED(metadata->uploadBuffer->Map(0, &readRange, &uploadData))) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
            "Failed to map upload buffer");
        return false;
    }
    
    memcpy(uploadData, data, dataSize);
    metadata->uploadBuffer->Unmap(0, nullptr);
    
    // Phase 4 TODO: Record copy command and execute
    // This would include:
    // 1. Create command allocator and list
    // 2. Record: CopyBufferRegion or CopyTextureRegion
    // 3. Execute command list
    // 4. Wait for GPU
    
    diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
        "Texture upload pending: %d bytes (Phase 4)", dataSize);
    return true;
}

bool gpuTextureDownload(GpuTextureHandle handle, void* outData, int dataSize)
{
    if (handle.resource == nullptr || outData == nullptr || dataSize <= 0) {
        return false;
    }
    
    auto metadata = static_cast<TextureMetadata*>(handle.resource);
    ID3D12Device* device = gpuDeviceGetDevice();
    
    if (device == nullptr || metadata->resource == nullptr) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
            "Cannot download: GPU device or texture not initialized");
        return false;
    }
    
    // Create readback buffer if not already created
    if (metadata->readbackBuffer == nullptr) {
        D3D12_RESOURCE_DESC readbackDesc = {};
        readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readbackDesc.Alignment = 0;
        readbackDesc.Width = dataSize;
        readbackDesc.Height = 1;
        readbackDesc.DepthOrArraySize = 1;
        readbackDesc.MipLevels = 1;
        readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
        readbackDesc.SampleDesc.Count = 1;
        readbackDesc.SampleDesc.Quality = 0;
        readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        readbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        
        D3D12_HEAP_PROPERTIES readbackHeapProps = {};
        readbackHeapProps.Type = D3D12_HEAP_TYPE_READBACK;
        readbackHeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        readbackHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        readbackHeapProps.CreationNodeMask = 1;
        readbackHeapProps.VisibleNodeMask = 1;
        
        if (FAILED(device->CreateCommittedResource(
            &readbackHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(metadata->readbackBuffer.ReleaseAndGetAddressOf())))) {
            diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
                "Failed to create readback buffer");
            return false;
        }
    }
    
    // Phase 4 TODO: Record copy command and execute
    // This would include:
    // 1. Transition texture to COPY_SOURCE state
    // 2. Create command allocator and list
    // 3. Record: CopyTextureRegion or CopyBufferRegion
    // 4. Execute command list
    // 5. Wait for GPU
    // 6. Map readback buffer and copy data
    
    diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
        "Texture download pending: %d bytes (Phase 4)", dataSize);
    return true;
}

ID3D12Resource* gpuTextureGetResource(GpuTextureHandle handle)
{
    if (handle.resource == nullptr) {
        return nullptr;
    }
    
    auto metadata = static_cast<TextureMetadata*>(handle.resource);
    return metadata->resource.Get();
}

bool gpuTextureGetDimensions(GpuTextureHandle handle, int& outWidth, int& outHeight)
{
    if (handle.resource == nullptr) {
        return false;
    }
    
    auto metadata = static_cast<TextureMetadata*>(handle.resource);
    outWidth = metadata->width;
    outHeight = metadata->height;
    return true;
}

GpuTextureHandle gpuTextureCreateShared(int width, int height)
{
    // Phase 4 TODO: Create shared texture that can be accessed by both SDL renderer and FSR2 compute
    // This would involve:
    // 1. Querying SDL's D3D12 context for shared heap
    // 2. Creating texture in shared memory
    // 3. Enabling cross-process/cross-queue sharing if needed
    
    diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
        "Shared texture creation pending (Phase 4): %dx%d", width, height);
    
    GpuTextureHandle invalidHandle = { nullptr };
    return invalidHandle;
}

}  // namespace fallout
