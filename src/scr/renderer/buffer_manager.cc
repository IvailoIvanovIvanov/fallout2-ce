#include "buffer_manager.h"
#include "logger.h"
#include <cstring>

namespace fallout {
namespace renderer {

BufferManager::BufferManager() = default;

BufferManager::~BufferManager() {
    // Shutdown should be called explicitly with context
}

bool BufferManager::Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;

    TextureDesc inputDesc = { inputWidth, inputHeight, TextureFormat::RGBA8 };
    // Use RGBA16F for output to support high-precision pipeline
    TextureDesc outputDesc = { outputWidth, outputHeight, TextureFormat::RGBA16F };

    for (int i = 0; i < 2; ++i) {
        mFrames[i].inputBuffer = context.CreateTexture(inputDesc);
        mFrames[i].outputBuffer = context.CreateTexture(outputDesc);
        mFrames[i].readbackData.resize(outputWidth * outputHeight * 4);

        if (!mFrames[i].inputBuffer || !mFrames[i].outputBuffer) {
            Logger::Log(LogLevel::Error, "BufferManager: Failed to create textures");
            return false;
        }
    }
    return true;
}

void BufferManager::Shutdown(GpuContext& context) {
    for (int i = 0; i < 2; ++i) {
        if (mFrames[i].inputBuffer) {
            context.DestroyTexture(mFrames[i].inputBuffer);
            mFrames[i].inputBuffer = nullptr;
        }
        if (mFrames[i].outputBuffer) {
            context.DestroyTexture(mFrames[i].outputBuffer);
            mFrames[i].outputBuffer = nullptr;
        }
        mFrames[i].readbackData.clear();
    }
}

void BufferManager::SwapBuffers() {
    mCurrentFrameIndex = 1 - mCurrentFrameIndex;
}

bool BufferManager::UploadInput(GpuContext& context, const void* data, size_t size) {
    // Assuming size matches input dimensions
    context.UpdateTexture(mFrames[mCurrentFrameIndex].inputBuffer, data, mInputWidth, mInputHeight);
    return true;
}

RenderSurface BufferManager::GetInputSurface() const {
    return { mFrames[mCurrentFrameIndex].inputBuffer, mInputWidth, mInputHeight, TextureFormat::RGBA8 };
}

RenderSurface BufferManager::GetOutputSurface() const {
    return { mFrames[mCurrentFrameIndex].outputBuffer, mOutputWidth, mOutputHeight, TextureFormat::RGBA16F };
}

const void* BufferManager::ReadbackOutput(GpuContext& context) {
    // Readback from the PREVIOUS frame (which is now ready)
    int prevFrameIndex = 1 - mCurrentFrameIndex;
    
    context.ReadbackTexture(mFrames[prevFrameIndex].outputBuffer, mFrames[prevFrameIndex].readbackData.data(), static_cast<int>(mFrames[prevFrameIndex].readbackData.size()));
    
    return mFrames[prevFrameIndex].readbackData.data();
}

} // namespace renderer
} // namespace fallout
