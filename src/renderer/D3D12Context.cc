#include "D3D12Context.h"
#include "gpu_device.h"
#include "../diagnostics.h"
#include <d3d12.h>

namespace fallout {
namespace renderer {

D3D12Context::D3D12Context() = default;

D3D12Context::~D3D12Context() {
    Shutdown();
}

bool D3D12Context::Init() {
    // Get shared device and queue
    ID3D12Device* device = gpuDeviceGetDevice();
    ID3D12CommandQueue* queue = gpuDeviceGetCommandQueue();

    if (!device || !queue) {
        diagnosticsLog(DiagnosticsLevel::Info, "D3D12Context", "Failed to get device or queue");
        return false;
    }

    mDevice = device;
    mCommandQueue = queue;

    // Create Command Allocator
    if (FAILED(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCommandAllocator)))) {
        diagnosticsLog(DiagnosticsLevel::Info, "D3D12Context", "Failed to create command allocator");
        return false;
    }

    // Create Command List
    if (FAILED(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&mCommandList)))) {
        diagnosticsLog(DiagnosticsLevel::Info, "D3D12Context", "Failed to create command list");
        return false;
    }
    // Command lists are created in recording state, but we usually want them closed until BeginFrame
    mCommandList->Close();

    // Create Fence
    if (FAILED(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)))) {
        diagnosticsLog(DiagnosticsLevel::Info, "D3D12Context", "Failed to create fence");
        return false;
    }
    mFenceValue = 1;

    // Create Fence Event
    mFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!mFenceEvent) {
        diagnosticsLog(DiagnosticsLevel::Info, "D3D12Context", "Failed to create fence event");
        return false;
    }

    return true;
}

void D3D12Context::Shutdown() {
    WaitForGpu();
    if (mFenceEvent) {
        CloseHandle(mFenceEvent);
        mFenceEvent = nullptr;
    }
    mCommandList.Reset();
    mCommandAllocator.Reset();
    mFence.Reset();
    mCommandQueue.Reset();
    mDevice.Reset();
}

void D3D12Context::BeginFrame() {
    mCommandAllocator->Reset();
    mCommandList->Reset(mCommandAllocator.Get(), nullptr);
}

void D3D12Context::EndFrame() {
    mCommandList->Close();
    ID3D12CommandList* ppCommandLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, ppCommandLists);

    // Signal fence
    const uint64_t fence = mFenceValue;
    mCommandQueue->Signal(mFence.Get(), fence);
    mFenceValue++;
}

void D3D12Context::WaitForGpu() {
    if (mFence->GetCompletedValue() < mFenceValue - 1) {
        if (FAILED(mFence->SetEventOnCompletion(mFenceValue - 1, mFenceEvent))) {
            return;
        }
        WaitForSingleObject(mFenceEvent, INFINITE);
    }
}

} // namespace renderer
} // namespace fallout
