#include "buffer_manager.h"
#include "logger.h"
#include <cstring>

namespace fallout {
namespace renderer {

BufferManager::BufferManager() = default;

BufferManager::~BufferManager() {
    // Note: Shutdown() must be called explicitly with GPU context before destruction
}

bool BufferManager::Init(GpuContext& context, int inputWidth, int inputHeight, 
                          int outputWidth, int outputHeight) {
    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;

    TextureDesc inputDesc{inputWidth, inputHeight, TextureFormat::RGBA8, true};
    TextureDesc outputDesc{outputWidth, outputHeight, TextureFormat::RGBA16F, true};

    for (int i = 0; i < kFrameCount; ++i) {
        if (!CreateFrameResources(context, mFrames[i], inputDesc, outputDesc)) {
            Logger::Log(LogLevel::Error, "BufferManager: Failed to create frame resources");
            return false;
        }
    }
    
    return true;
}

bool BufferManager::CreateFrameResources(GpuContext& context, FrameResources& frame,
                                          const TextureDesc& inputDesc, 
                                          const TextureDesc& outputDesc) {
    frame.inputBuffer = context.CreateTexture(inputDesc);
    frame.outputBuffer = context.CreateTexture(outputDesc);
    frame.readbackData.resize(outputDesc.width * outputDesc.height * 4);

    return frame.inputBuffer != nullptr && frame.outputBuffer != nullptr;
}

void BufferManager::Shutdown(GpuContext& context) {
    for (int i = 0; i < kFrameCount; ++i) {
        DestroyFrameResources(context, mFrames[i]);
    }
}

void BufferManager::DestroyFrameResources(GpuContext& context, FrameResources& frame) {
    if (frame.inputBuffer) {
        context.DestroyTexture(frame.inputBuffer);
        frame.inputBuffer = nullptr;
    }
    if (frame.outputBuffer) {
        context.DestroyTexture(frame.outputBuffer);
        frame.outputBuffer = nullptr;
    }
    frame.readbackData.clear();
}

void BufferManager::SwapBuffers() {
    mCurrentFrameIndex = 1 - mCurrentFrameIndex;
}

bool BufferManager::UploadInput(GpuContext& context, const void* data, size_t size) {
    context.UpdateTexture(mFrames[mCurrentFrameIndex].inputBuffer, data, mInputWidth, mInputHeight);
    return true;
}

RenderSurface BufferManager::GetInputSurface() const {
    return {mFrames[mCurrentFrameIndex].inputBuffer, mInputWidth, mInputHeight, TextureFormat::RGBA8};
}

RenderSurface BufferManager::GetOutputSurface() const {
    return {mFrames[mCurrentFrameIndex].outputBuffer, mOutputWidth, mOutputHeight, TextureFormat::RGBA16F};
}

const void* BufferManager::ReadbackOutput(GpuContext& context) {
    const int prevFrameIndex = 1 - mCurrentFrameIndex;
    FrameResources& prevFrame = mFrames[prevFrameIndex];
    
    context.ReadbackTexture(prevFrame.outputBuffer, prevFrame.readbackData.data(), 
                             static_cast<int>(prevFrame.readbackData.size()));
    
    return prevFrame.readbackData.data();
}

} // namespace renderer
} // namespace fallout
