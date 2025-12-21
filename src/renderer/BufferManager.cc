#include "BufferManager.h"
#include "../diagnostics.h"
#include <d3d12.h>

namespace fallout {
namespace renderer {

BufferManager::BufferManager() = default;

BufferManager::~BufferManager() {
    Shutdown();
}

bool BufferManager::Init(D3D12Context& context, int width, int height) {
    mWidth = width;
    mHeight = height;
    size_t bufferSize = width * height * 4; // RGBA8

    for (int i = 0; i < 2; ++i) {
        // 1. Create Readback Buffer
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
        
        D3D12_RESOURCE_DESC resDesc = {};
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resDesc.Width = bufferSize;
        resDesc.Height = 1;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels = 1;
        resDesc.Format = DXGI_FORMAT_UNKNOWN;
        resDesc.SampleDesc.Count = 1;
        resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        if (FAILED(context.GetDevice()->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&mFrames[i].readbackBuffer)))) {
            diagnosticsLog(DiagnosticsLevel::Info, "BufferManager", "Failed to create readback buffer %d", i);
            return false;
        }

        // Map it persistently
        D3D12_RANGE readRange = { 0, bufferSize };
        if (FAILED(mFrames[i].readbackBuffer->Map(0, &readRange, &mFrames[i].mappedPtr))) {
            diagnosticsLog(DiagnosticsLevel::Info, "BufferManager", "Failed to map readback buffer %d", i);
            return false;
        }
    }

    return true;
}

void BufferManager::Shutdown() {
    for (int i = 0; i < 2; ++i) {
        if (mFrames[i].readbackBuffer) {
            mFrames[i].readbackBuffer->Unmap(0, nullptr);
            mFrames[i].readbackBuffer.Reset();
        }
        mFrames[i].inputBuffer.Reset();
        mFrames[i].outputBuffer.Reset();
        mFrames[i].mappedPtr = nullptr;
    }
}

void BufferManager::SwapBuffers() {
    mCurrentFrameIndex = (mCurrentFrameIndex + 1) % 2;
}

ID3D12Resource* BufferManager::GetReadbackBuffer() const {
    // Return the buffer for the PREVIOUS frame (which we are about to read)
    // If we are processing Frame N (mCurrentFrameIndex), we want to read Frame N-1.
    // N-1 is (mCurrentFrameIndex + 1) % 2 in a 2-buffer system.
    int prevIndex = (mCurrentFrameIndex + 1) % 2;
    return mFrames[prevIndex].readbackBuffer.Get();
}

const void* BufferManager::MapReadback() {
    int prevIndex = (mCurrentFrameIndex + 1) % 2;
    return mFrames[prevIndex].mappedPtr;
}

} // namespace renderer
} // namespace fallout
