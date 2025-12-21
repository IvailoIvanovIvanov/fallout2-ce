#include "BufferManager.h"
#include "../diagnostics.h"
#include <d3d12.h>
#include <cstring>
#include <algorithm>

namespace fallout {
namespace renderer {

BufferManager::BufferManager() = default;

BufferManager::~BufferManager() {
    Shutdown();
}

bool BufferManager::Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;
    
    // Calculate aligned size for Upload Buffer (Input)
    size_t inputRowPitch = (inputWidth * 4 + 255) & ~255;
    size_t inputAlignedSize = inputRowPitch * inputHeight;

    // Calculate aligned size for Readback Buffer (Output)
    size_t outputRowPitch = (outputWidth * 4 + 255) & ~255;
    size_t outputAlignedSize = outputRowPitch * outputHeight;
    
    for (int i = 0; i < 2; ++i) {
        mInputBufferState[i] = D3D12_RESOURCE_STATE_COMMON;

        // 1. Create Readback Buffer (Output Size)
        D3D12_HEAP_PROPERTIES readbackHeap = {};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        readbackHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        readbackHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        readbackHeap.CreationNodeMask = 1;
        readbackHeap.VisibleNodeMask = 1;
        
        D3D12_RESOURCE_DESC bufDesc = {};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = outputAlignedSize;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        if (FAILED(context.GetDevice()->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&mFrames[i].readbackBuffer)))) {
            diagnosticsLog(DiagnosticsLevel::Info, "BufferManager", "Failed to create readback buffer %d", i);
            return false;
        }

        // Map it persistently
        D3D12_RANGE readRange = { 0, outputAlignedSize };
        if (FAILED(mFrames[i].readbackBuffer->Map(0, &readRange, &mFrames[i].mappedPtr))) {
            diagnosticsLog(DiagnosticsLevel::Info, "BufferManager", "Failed to map readback buffer %d", i);
            return false;
        }

        // 2. Create Upload Buffer (Input Size)
        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        uploadHeap.CreationNodeMask = 1;
        uploadHeap.VisibleNodeMask = 1;
        
        bufDesc.Width = inputAlignedSize; // Use input size

        if (FAILED(context.GetDevice()->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&mFrames[i].uploadBuffer)))) {
            diagnosticsLog(DiagnosticsLevel::Info, "BufferManager", "Failed to create upload buffer %d", i);
            return false;
        }

        // 3. Create Input Texture (Input Size)
        D3D12_HEAP_PROPERTIES defaultHeap = {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        defaultHeap.CreationNodeMask = 1;
        defaultHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = inputWidth;
        texDesc.Height = inputHeight;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if (FAILED(context.GetDevice()->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&mFrames[i].inputBuffer)))) {
            diagnosticsLog(DiagnosticsLevel::Info, "BufferManager", "Failed to create input buffer %d", i);
            return false;
        }

        // 4. Create Output Texture (Output Size)
        texDesc.Width = outputWidth;
        texDesc.Height = outputHeight;

        if (FAILED(context.GetDevice()->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&mFrames[i].outputBuffer)))) {
            diagnosticsLog(DiagnosticsLevel::Info, "BufferManager", "Failed to create output buffer %d", i);
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
        mFrames[i].uploadBuffer.Reset();
        mFrames[i].inputBuffer.Reset();
        mFrames[i].outputBuffer.Reset();
        mFrames[i].mappedPtr = nullptr;
    }
}

void BufferManager::SwapBuffers() {
    mCurrentFrameIndex = (mCurrentFrameIndex + 1) % 2;
}

bool BufferManager::UploadInput(D3D12Context& context, const void* data, size_t size) {
    auto& frame = mFrames[mCurrentFrameIndex];
    
    // 1. Copy to Upload Buffer (handling pitch)
    void* pData;
    D3D12_RANGE readRange = { 0, 0 };
    if (FAILED(frame.uploadBuffer->Map(0, &readRange, &pData))) {
        return false;
    }

    size_t rowPitch = (mInputWidth * 4 + 255) & ~255;
    const uint8_t* srcBytes = static_cast<const uint8_t*>(data);
    uint8_t* dstBytes = static_cast<uint8_t*>(pData);

    for (int y = 0; y < mInputHeight; ++y) {
        memcpy(dstBytes + y * rowPitch, srcBytes + y * mInputWidth * 4, mInputWidth * 4);
    }
    
    frame.uploadBuffer->Unmap(0, nullptr);

    // 2. Record Copy Command
    auto cmdList = context.GetCommandList();

    // Transition Input Buffer to COPY_DEST
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.inputBuffer.Get();
    barrier.Transition.StateBefore = mInputBufferState[mCurrentFrameIndex];
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
    
    mInputBufferState[mCurrentFrameIndex] = D3D12_RESOURCE_STATE_COPY_DEST;

    // Copy Buffer to Texture
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = frame.inputBuffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = frame.uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = mInputWidth;
    src.PlacedFootprint.Footprint.Height = mInputHeight;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);
    
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Transition Input Buffer to SHADER_RESOURCE
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmdList->ResourceBarrier(1, &barrier);
    
    mInputBufferState[mCurrentFrameIndex] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    return true;
}

ID3D12Resource* BufferManager::GetInputBuffer() const {
    return mFrames[mCurrentFrameIndex].inputBuffer.Get();
}

ID3D12Resource* BufferManager::GetOutputBuffer() const {
    return mFrames[mCurrentFrameIndex].outputBuffer.Get();
}

ID3D12Resource* BufferManager::GetCurrentReadbackBuffer() const {
    return mFrames[mCurrentFrameIndex].readbackBuffer.Get();
}

ID3D12Resource* BufferManager::GetReadbackBuffer() const {
    int prevIndex = (mCurrentFrameIndex + 1) % 2;
    return mFrames[prevIndex].readbackBuffer.Get();
}

const void* BufferManager::MapReadback() {
    int prevIndex = (mCurrentFrameIndex + 1) % 2;
    return mFrames[prevIndex].mappedPtr;
}

} // namespace renderer
} // namespace fallout
