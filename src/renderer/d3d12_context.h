#ifndef FALLOUT_RENDERER_D3D12_CONTEXT_H
#define FALLOUT_RENDERER_D3D12_CONTEXT_H

// OBSOLETE: This file is part of the Direct3D 12 backend which is being replaced by OpenGL.
// See OPENGL_MIGRATION_PLAN.md

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace fallout {
namespace renderer {

class D3D12Context {
public:
    D3D12Context();
    ~D3D12Context();

    bool Init();
    void Shutdown();

    // Command List Management
    void BeginFrame();
    void EndFrame();
    void WaitForGpu();

    // Accessors
    ID3D12Device* GetDevice() const { return mDevice.Get(); }
    ID3D12GraphicsCommandList* GetCommandList() const { return mCommandList.Get(); }
    ID3D12CommandQueue* GetCommandQueue() const { return mCommandQueue.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12Device> mDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> mCommandQueue;
    
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> mCommandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> mCommandList;

    // Synchronization
    Microsoft::WRL::ComPtr<ID3D12Fence> mFence;
    uint64_t mFenceValue = 0;
    void* mFenceEvent = nullptr;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_D3D12_CONTEXT_H
