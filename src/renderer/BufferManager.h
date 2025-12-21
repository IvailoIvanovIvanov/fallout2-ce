#ifndef FALLOUT_RENDERER_BUFFER_MANAGER_H
#define FALLOUT_RENDERER_BUFFER_MANAGER_H

#include "D3D12Context.h"
#include <vector>
#include <memory>

namespace fallout {
namespace renderer {

class BufferManager {
public:
    BufferManager();
    ~BufferManager();

    bool Init(D3D12Context& context, int width, int height);
    void Shutdown();

    // Double buffering logic
    void SwapBuffers();

    // Upload input data to the current frame's input buffer
    bool UploadInput(D3D12Context& context, const void* data, size_t size);

    // Getters for current frame resources
    ID3D12Resource* GetInputBuffer() const;
    ID3D12Resource* GetOutputBuffer() const;
    
    // Getters for previous frame resources (for readback)
    ID3D12Resource* GetReadbackBuffer() const;
    const void* MapReadback();

private:
    struct FrameResources {
        Microsoft::WRL::ComPtr<ID3D12Resource> inputBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> outputBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
        void* mappedPtr = nullptr;
    };

    FrameResources mFrames[2];
    int mCurrentFrameIndex = 0;
    int mWidth = 0;
    int mHeight = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_BUFFER_MANAGER_H
