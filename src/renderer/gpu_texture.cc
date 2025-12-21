#include "gpu_texture.h"

#include <d3d12.h>
#include <wrl/client.h>
#include "../diagnostics.h"
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
            return DXGI_FORMAT_B8G8R8A8_UNORM;  // Windows ARGB is BGRA in memory
        case GpuTextureFormat::RGBA8888:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case GpuTextureFormat::R32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
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
    
    // Execute copy command to transfer from upload buffer to texture
    ID3D12CommandAllocator* allocator = gpuDeviceCreateCommandAllocator();
    if (allocator == nullptr) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE", "Failed to create command allocator for upload");
        return false;
    }
    
    ID3D12GraphicsCommandList* cmdList = gpuDeviceCreateCommandList(allocator);
    if (cmdList == nullptr) {
        allocator->Release();
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE", "Failed to create command list for upload");
        return false;
    }
    
    // Transition texture to copy destination state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = metadata->resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
    
    // Copy from upload buffer to texture
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    footprint.Offset = 0;
    footprint.Footprint.Format = getD3dFormat(metadata->format);
    footprint.Footprint.Width = metadata->width;
    footprint.Footprint.Height = metadata->height;
    footprint.Footprint.Depth = 1;
    footprint.Footprint.RowPitch = metadata->width * 4;  // 4 bytes per pixel (RGBA)
    
    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
    srcLocation.pResource = metadata->uploadBuffer.Get();
    srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLocation.PlacedFootprint = footprint;
    
    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
    dstLocation.pResource = metadata->resource.Get();
    dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLocation.SubresourceIndex = 0;
    
    cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
    
    // Transition texture to shader resource state
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cmdList->ResourceBarrier(1, &barrier);
    
    // Execute and wait
    if (!gpuDeviceExecuteCommandList(cmdList)) {
        cmdList->Release();
        allocator->Release();
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE", "Failed to execute upload command list");
        return false;
    }
    
    gpuDeviceWaitForGpu();
    
    cmdList->Release();
    allocator->Release();
    
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
    
    // Execute copy command to transfer from texture to readback buffer
    ID3D12CommandAllocator* allocator = gpuDeviceCreateCommandAllocator();
    if (allocator == nullptr) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE", "Failed to create command allocator for download");
        return false;
    }
    
    ID3D12GraphicsCommandList* cmdList = gpuDeviceCreateCommandList(allocator);
    if (cmdList == nullptr) {
        allocator->Release();
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE", "Failed to create command list for download");
        return false;
    }
    
    // Transition texture to copy source state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = metadata->resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
    
    // Copy from texture to readback buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    footprint.Offset = 0;
    footprint.Footprint.Format = getD3dFormat(metadata->format);
    footprint.Footprint.Width = metadata->width;
    footprint.Footprint.Height = metadata->height;
    footprint.Footprint.Depth = 1;
    footprint.Footprint.RowPitch = metadata->width * 4;  // 4 bytes per pixel (RGBA)
    
    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
    srcLocation.pResource = metadata->resource.Get();
    srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLocation.SubresourceIndex = 0;
    
    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
    dstLocation.pResource = metadata->readbackBuffer.Get();
    dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLocation.PlacedFootprint = footprint;
    
    cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
    
    // Transition texture back to UAV state
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cmdList->ResourceBarrier(1, &barrier);
    
    // Execute and wait
    if (!gpuDeviceExecuteCommandList(cmdList)) {
        cmdList->Release();
        allocator->Release();
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE", "Failed to execute download command list");
        return false;
    }
    
    gpuDeviceWaitForGpu();
    
    cmdList->Release();
    allocator->Release();
    
    // Now read from readback buffer
    void* readbackData = nullptr;
    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(dataSize) };
    if (FAILED(metadata->readbackBuffer->Map(0, &readRange, &readbackData))) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
            "Failed to map readback buffer");
        return false;
    }
    
    memcpy(outData, readbackData, dataSize);
    
    D3D12_RANGE writeRange = { 0, 0 };
    metadata->readbackBuffer->Unmap(0, &writeRange);
    
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

bool gpuTextureGetDescriptor(GpuTextureHandle handle, bool isSRV, void* outCPU, void* outGPU)
{
    if (handle.resource == nullptr || outCPU == nullptr || outGPU == nullptr) {
        return false;
    }
    
    auto metadata = static_cast<TextureMetadata*>(handle.resource);
    ID3D12Device* device = gpuDeviceGetDevice();
    
    if (device == nullptr || metadata->resource == nullptr) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
            "Cannot create descriptor: device or resource not initialized");
        return false;
    }
    
    // TODO: This is a simplified implementation that doesn't manage descriptor heaps properly.
    // For production code, we should:
    // 1. Create/manage a persistent descriptor heap for SRVs and UAVs
    // 2. Allocate descriptor slots from the heap
    // 3. Cache descriptors per texture to avoid recreating them each frame
    //
    // For now, we'll create a temporary descriptor heap (inefficient but functional)
    
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(descriptorHeap.ReleaseAndGetAddressOf()));
    
    if (FAILED(hr)) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_TEXTURE",
            "Failed to create descriptor heap for texture");
        return false;
    }
    
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    
    if (isSRV) {
        // Create Shader Resource View (read-only)
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = getD3dFormat(metadata->format);
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
        
        device->CreateShaderResourceView(metadata->resource.Get(), &srvDesc, cpuHandle);
    } else {
        // Create Unordered Access View (read-write)
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = getD3dFormat(metadata->format);
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;
        
        device->CreateUnorderedAccessView(metadata->resource.Get(), nullptr, &uavDesc, cpuHandle);
    }
    
    // Copy handles to output
    memcpy(outCPU, &cpuHandle, sizeof(D3D12_CPU_DESCRIPTOR_HANDLE));
    memcpy(outGPU, &gpuHandle, sizeof(D3D12_GPU_DESCRIPTOR_HANDLE));
    
    // Keep descriptor heap alive (memory leak, but functional for proof-of-concept)
    // TODO: Implement proper descriptor heap management
    descriptorHeap.Detach();
    
    return true;
}

GpuTextureHandle gpuTextureCreateShared(int width, int height)
{
    // Phase 4 TODO: Create shared texture that can be accessed by both SDL renderer and compute shaders
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
