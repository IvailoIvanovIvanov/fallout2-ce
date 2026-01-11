#ifndef FALLOUT_RENDERER_BUFFER_MANAGER_H
#define FALLOUT_RENDERER_BUFFER_MANAGER_H

#include "gpu_context.h"
#include <vector>
#include <memory>

namespace fallout {
namespace renderer {

class BufferManager {
public:
    BufferManager();
    ~BufferManager();

    bool Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight);
    void Shutdown(GpuContext& context);

    // Double buffering logic
    void SwapBuffers();

    // Upload input data to the current frame's input buffer
    bool UploadInput(GpuContext& context, const void* data, size_t size);

    // Getters for current frame resources
    void* GetInputBuffer() const;
    void* GetOutputBuffer() const;
    
    // Getters for previous frame resources (for readback)
    const void* ReadbackOutput(GpuContext& context);

    int GetWidth() const { return mInputWidth; }
    int GetHeight() const { return mInputHeight; }
    int GetOutputWidth() const { return mOutputWidth; }
    int GetOutputHeight() const { return mOutputHeight; }

private:
    struct FrameResources {
        void* inputBuffer = nullptr;
        void* outputBuffer = nullptr;
        std::vector<uint8_t> readbackData;
    };

    FrameResources mFrames[2];
    int mCurrentFrameIndex = 0;
    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_BUFFER_MANAGER_H
