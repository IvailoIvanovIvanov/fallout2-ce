#ifndef FALLOUT_RENDERER_BUFFER_MANAGER_H
#define FALLOUT_RENDERER_BUFFER_MANAGER_H

#include "d3d12_context.h"
#include <vector>
#include <memory>

namespace fallout {
namespace renderer {

class BufferManager {
public:
    BufferManager();
    ~BufferManager();

    bool Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight);
    void Shutdown();

    // Double buffering logic
    void SwapBuffers();

    // Upload input data to the current frame's input buffer
    bool UploadInput(D3D12Context& context, const void* data, size_t size);

    // Getters for current frame resources
    ID3D12Resource* GetInputBuffer() const;
    ID3D12Resource* GetOutputBuffer() const;
    ID3D12Resource* GetCurrentReadbackBuffer() const;
    
    // Getters for previous frame resources (for readback)
    ID3D12Resource* GetReadbackBuffer() const;
    const void* MapReadback();

    int GetWidth() const { return mInputWidth; }
    int GetHeight() const { return mInputHeight; }
    int GetOutputWidth() const { return mOutputWidth; }
    int GetOutputHeight() const { return mOutputHeight; }

private:
    struct FrameResources {
        Microsoft::WRL::ComPtr<ID3D12Resource> inputBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> outputBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
        void* mappedPtr = nullptr;
    };

    FrameResources mFrames[2];
    int mCurrentFrameIndex = 0;
    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
    D3D12_RESOURCE_STATES mInputBufferState[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_BUFFER_MANAGER_H
