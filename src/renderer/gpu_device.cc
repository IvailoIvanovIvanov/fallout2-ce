#include "gpu_device.h"

#include <SDL.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include "../diagnostics.h"

using Microsoft::WRL::ComPtr;

namespace fallout {

/**
 * @brief Static GPU device context
 * 
 * Holds pointers to D3D12 device and command queue created for GPU operations.
 */
static struct {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;
    HANDLE fenceEvent = nullptr;
    std::vector<ID3D12Resource*> pendingResources;
} gGpuContext;

bool gpuDeviceInit()
{
    // Phase 8: Initialize D3D12 device for GPU compute operations
    // Create our own device that will be used for GPU operations like AI upscaling
    
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Failed to create DXGI factory");
        return false;
    }
    
    // Create device if not already created
    if (gGpuContext.device == nullptr) {
        // Try to find a suitable GPU adapter
        // Start with the primary GPU (adapter 0), but have fallback logic
        for (UINT adapterIndex = 0; adapterIndex < 4; adapterIndex++) {
            ComPtr<IDXGIAdapter> adapter;
            if (FAILED(factory->EnumAdapters(adapterIndex, adapter.ReleaseAndGetAddressOf()))) {
                // No more adapters
                if (adapterIndex == 0) {
                    diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Failed to enumerate any GPU adapters");
                    return false;
                }
                break;  // Try with adapters we found
            }
            
            DXGI_ADAPTER_DESC adapterDesc = {};
            adapter->GetDesc(&adapterDesc);
            diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", 
                "Trying adapter %u: %S (VRAM: %llu MB)", 
                adapterIndex, adapterDesc.Description, adapterDesc.DedicatedVideoMemory / (1024 * 1024));
            
            ComPtr<ID3D12Device> device;
            // Try feature level 12.1 first, then 12.0 as fallback
            if (!FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(device.ReleaseAndGetAddressOf()))) ||
                !FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.ReleaseAndGetAddressOf())))) {
                
                diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Successfully created D3D12 device on adapter %u", adapterIndex);
                gGpuContext.device = device.Get();
                device.Detach();
                
                // Create command queue
                D3D12_COMMAND_QUEUE_DESC queueDesc = {};
                queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
                queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                
                ComPtr<ID3D12CommandQueue> commandQueue;
                if (FAILED(gGpuContext.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(commandQueue.ReleaseAndGetAddressOf())))) {
                    diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Failed to create command queue");
                    gGpuContext.device->Release();
                    gGpuContext.device = nullptr;
                    continue;  // Try next adapter
                }
                
                gGpuContext.commandQueue = commandQueue.Get();
                commandQueue.Detach();
                
                // Create synchronization fence
                if (FAILED(gGpuContext.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(gGpuContext.fence.ReleaseAndGetAddressOf())))) {
                    diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Failed to create fence");
                    gGpuContext.commandQueue->Release();
                    gGpuContext.commandQueue = nullptr;
                    gGpuContext.device->Release();
                    gGpuContext.device = nullptr;
                    continue;  // Try next adapter
                }
                
                // Create fence event
                gGpuContext.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (gGpuContext.fenceEvent == nullptr) {
                    diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Failed to create fence event");
                    gGpuContext.fence = nullptr;
                    gGpuContext.commandQueue->Release();
                    gGpuContext.commandQueue = nullptr;
                    gGpuContext.device->Release();
                    gGpuContext.device = nullptr;
                    continue;  // Try next adapter
                }
                
                // TEST: Verify device is truly usable by creating a command allocator
                // This catches devices that are technically "created" but broken/removed (TDR)
                ComPtr<ID3D12CommandAllocator> testAllocator;
                HRESULT testHr = gGpuContext.device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, 
                    IID_PPV_ARGS(testAllocator.ReleaseAndGetAddressOf()));
                
                if (FAILED(testHr)) {
                    diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", 
                        "Device validation failed: CreateCommandAllocator returned 0x%08X", testHr);
                    
                    if (gGpuContext.fenceEvent) CloseHandle(gGpuContext.fenceEvent);
                    gGpuContext.fenceEvent = nullptr;
                    gGpuContext.fence = nullptr;
                    gGpuContext.commandQueue->Release();
                    gGpuContext.commandQueue = nullptr;
                    gGpuContext.device->Release();
                    gGpuContext.device = nullptr;
                    continue; // Try next adapter
                }
                
                // Successfully initialized - break out of adapter loop
                break;
            }
        }
        
        if (gGpuContext.device == nullptr) {
            diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Failed to create D3D12 device on any adapter");
            return false;
        }
    }
    
    diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE",
        "GPU device initialized successfully (D3D12)");
    
    return true;
}

void gpuDeviceShutdown()
{
    if (gGpuContext.fenceEvent != nullptr) {
        CloseHandle(gGpuContext.fenceEvent);
        gGpuContext.fenceEvent = nullptr;
    }
    
    gGpuContext.fence = nullptr;
    
    if (gGpuContext.commandQueue != nullptr) {
        gGpuContext.commandQueue->Release();
        gGpuContext.commandQueue = nullptr;
    }
    
    if (gGpuContext.device != nullptr) {
        gGpuContext.device->Release();
        gGpuContext.device = nullptr;
    }
    
    gGpuContext.fenceValue = 0;
}

ID3D12Device* gpuDeviceGetDevice()
{
    return gGpuContext.device;
}

ID3D12CommandQueue* gpuDeviceGetCommandQueue()
{
    return gGpuContext.commandQueue;
}

ID3D12CommandAllocator* gpuDeviceCreateCommandAllocator()
{
    if (gGpuContext.device == nullptr) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Cannot create command allocator: device is null");
        return nullptr;
    }
    
    ComPtr<ID3D12CommandAllocator> allocator;
    HRESULT hr = gGpuContext.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf()));
    
    if (FAILED(hr)) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", 
            "Failed to create command allocator (HRESULT: 0x%08X)", hr);
        return nullptr;
    }
    
    return allocator.Detach();
}

ID3D12GraphicsCommandList* gpuDeviceCreateCommandList(ID3D12CommandAllocator* allocator)
{
    if (gGpuContext.device == nullptr || allocator == nullptr) {
        return nullptr;
    }
    
    ComPtr<ID3D12GraphicsCommandList> commandList;
    if (FAILED(gGpuContext.device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator,
        nullptr,
        IID_PPV_ARGS(commandList.ReleaseAndGetAddressOf())))) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Failed to create command list");
        return nullptr;
    }
    
    return commandList.Detach();
}

bool gpuDeviceExecuteCommandList(ID3D12GraphicsCommandList* commandList)
{
    if (gGpuContext.commandQueue == nullptr || commandList == nullptr) {
        return false;
    }
    
    // Close command list (must be closed before execution)
    if (FAILED(commandList->Close())) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Failed to close command list");
        return false;
    }
    
    // Execute command list
    ID3D12CommandList* ppCommandLists[] = { commandList };
    gGpuContext.commandQueue->ExecuteCommandLists(1, ppCommandLists);
    
    return true;
}

void gpuDeviceWaitForGpu()
{
    if (gGpuContext.commandQueue == nullptr || gGpuContext.fence == nullptr) {
        return;
    }
    
    // Signal fence with increment
    gGpuContext.fenceValue++;
    if (FAILED(gGpuContext.commandQueue->Signal(gGpuContext.fence.Get(), gGpuContext.fenceValue))) {
        return;
    }
    
    // Wait for fence to complete
    if (gGpuContext.fence->GetCompletedValue() < gGpuContext.fenceValue) {
        gGpuContext.fence->SetEventOnCompletion(gGpuContext.fenceValue, gGpuContext.fenceEvent);
        WaitForSingleObject(gGpuContext.fenceEvent, INFINITE);
    }

    // Release pending resources now that GPU is done
    for (auto* res : gGpuContext.pendingResources) {
        res->Release();
    }
    gGpuContext.pendingResources.clear();
}

bool gpuDeviceIsReady()
{
    return gGpuContext.device != nullptr && gGpuContext.commandQueue != nullptr;
}

bool gpuUploadConstantBuffer(const void* data, int size, uint64_t* outAddress)
{
    if (data == nullptr || size <= 0 || outAddress == nullptr) {
        return false;
    }
    
    if (gGpuContext.device == nullptr) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE",
            "Cannot upload constant buffer: device not initialized");
        return false;
    }
    
    // Align size to 256 bytes (required for constant buffers)
    int alignedSize = (size + 255) & ~255;
    
    // Create upload heap for constant buffer
    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeapProps.CreationNodeMask = 1;
    uploadHeapProps.VisibleNodeMask = 1;
    
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Alignment = 0;
    bufferDesc.Width = alignedSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.SampleDesc.Quality = 0;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    
    ComPtr<ID3D12Resource> uploadBuffer;
    HRESULT hr = gGpuContext.device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(uploadBuffer.ReleaseAndGetAddressOf()));
    
    if (FAILED(hr)) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE",
            "Failed to create constant buffer upload resource (HRESULT: 0x%08X)", hr);
        return false;
    }
    
    // Map and copy data
    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = uploadBuffer->Map(0, &readRange, &mappedData);
    
    if (FAILED(hr)) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE",
            "Failed to map constant buffer upload resource");
        return false;
    }
    
    memcpy(mappedData, data, size);
    uploadBuffer->Unmap(0, nullptr);
    
    // Return GPU virtual address
    *outAddress = uploadBuffer->GetGPUVirtualAddress();
    
    // Keep alive until GPU is done
    gGpuContext.pendingResources.push_back(uploadBuffer.Detach());
    
    return true;
}

}  // namespace fallout

