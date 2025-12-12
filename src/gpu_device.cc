#include "gpu_device.h"

#include <SDL.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include "diagnostics.h"

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
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(factory->EnumAdapters(0, adapter.ReleaseAndGetAddressOf()))) {
            diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Failed to enumerate GPU adapter");
            return false;
        }
        
        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(device.ReleaseAndGetAddressOf())))) {
            diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Failed to create D3D12 device");
            return false;
        }
        
        gGpuContext.device = device.Get();
        device.Detach();  // Transfer ownership to gGpuContext
        
        // Create command queue
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        
        ComPtr<ID3D12CommandQueue> commandQueue;
        if (FAILED(gGpuContext.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(commandQueue.ReleaseAndGetAddressOf())))) {
            diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Failed to create command queue");
            gGpuContext.device->Release();
            gGpuContext.device = nullptr;
            return false;
        }
        
        gGpuContext.commandQueue = commandQueue.Get();
        commandQueue.Detach();  // Transfer ownership to gGpuContext
        
        // Create synchronization fence
        if (FAILED(gGpuContext.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(gGpuContext.fence.ReleaseAndGetAddressOf())))) {
            diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Failed to create fence");
            gGpuContext.commandQueue->Release();
            gGpuContext.commandQueue = nullptr;
            gGpuContext.device->Release();
            gGpuContext.device = nullptr;
            return false;
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
        return nullptr;
    }
    
    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(gGpuContext.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf())))) {
        diagnosticsLog(DiagnosticsLevel::Info, "GPU_DEVICE", "Failed to create command allocator");
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
}

bool gpuDeviceIsReady()
{
    return gGpuContext.device != nullptr && gGpuContext.commandQueue != nullptr;
}

}  // namespace fallout

