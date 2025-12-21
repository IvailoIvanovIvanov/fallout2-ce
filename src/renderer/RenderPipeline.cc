#include "RenderPipeline.h"
#include "../diagnostics.h"
#include <d3d12.h>

namespace fallout {
namespace renderer {

RenderPipeline::RenderPipeline() {
}

RenderPipeline::~RenderPipeline() {
    Shutdown();
}

bool RenderPipeline::Init(int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    if (mInitialized) {
        return true;
    }

    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;

    if (!mContext.Init()) {
        diagnosticsLog(DiagnosticsLevel::Error, "RenderPipeline", "Failed to initialize D3D12Context");
        return false;
    }

    if (!mBuffers.Init(mContext, inputWidth, inputHeight, outputWidth, outputHeight)) {
        diagnosticsLog(DiagnosticsLevel::Error, "RenderPipeline", "Failed to initialize BufferManager");
        return false;
    }

    // TODO: Initialize passes here
    // mPasses.push_back(std::make_unique<PreprocessingPass>());

    for (auto& pass : mPasses) {
        if (!pass->Init(mContext, inputWidth, inputHeight, outputWidth, outputHeight)) {
            return false;
        }
    }

    mInitialized = true;
    return true;
}

void RenderPipeline::Shutdown() {
    for (auto& pass : mPasses) {
        pass->Shutdown();
    }
    mPasses.clear();

    mBuffers.Shutdown();
    mContext.Shutdown();
    mInitialized = false;
}

void RenderPipeline::Dispatch(const void* inputPixels, int width, int height) {
    if (!mInitialized) return;

    // 1. Swap Buffers (Move to next frame)
    mBuffers.SwapBuffers();

    // 2. Begin Frame (Wait for GPU to finish with this frame's resources)
    mContext.BeginFrame();

    // 3. Upload Input
    if (!mBuffers.UploadInput(mContext, inputPixels, width * height * 4)) {
        diagnosticsLog(DiagnosticsLevel::Error, "RenderPipeline", "Failed to upload input");
    }

    // 4. Execute Passes
    if (!mPasses.empty()) {
        for (auto& pass : mPasses) {
            pass->Execute(mContext, mBuffers);
        }
    } else {
        // If no passes, copy Input to Output
        auto cmdList = mContext.GetCommandList();
        auto inputBuffer = mBuffers.GetInputBuffer();
        auto outputBuffer = mBuffers.GetOutputBuffer();

        // Transition Input to COPY_SOURCE
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = inputBuffer;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        // Transition Output to COPY_DEST
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = outputBuffer;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        cmdList->ResourceBarrier(2, barriers);

        // CopyResource requires same dimensions. If scaling, we can't use CopyResource.
        // We need to use a blit shader or CopyTextureRegion if dimensions match.
        if (mInputWidth == mOutputWidth && mInputHeight == mOutputHeight) {
            cmdList->CopyResource(outputBuffer, inputBuffer);
        } else {
            // Scaling required but no passes?
            // We should probably have a default BlitPass.
            // For now, just skip copy (black screen) or log error.
            diagnosticsLog(DiagnosticsLevel::Error, "RenderPipeline", "No passes and input/output size mismatch!");
        }

        // Restore states
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; // Assume UAV for consistency

        cmdList->ResourceBarrier(2, barriers);
    }

    // 5. Copy Output to Readback
    {
        auto cmdList = mContext.GetCommandList();
        auto outputBuffer = mBuffers.GetOutputBuffer();
        auto readbackBuffer = mBuffers.GetCurrentReadbackBuffer();

        // Transition Output to COPY_SOURCE
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = outputBuffer;
        // If passes ran, state is likely UAV. If no passes (and we did copy), state is UAV.
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);

        // Copy Texture to Buffer
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readbackBuffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = mOutputWidth;
        dst.PlacedFootprint.Footprint.Height = mOutputHeight;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = (mOutputWidth * 4 + 255) & ~255;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = outputBuffer;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        // Transition Output back to COMMON (or whatever)
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        cmdList->ResourceBarrier(1, &barrier);
    }

    // 6. End Frame (Signal Fence)
    mContext.EndFrame();
}

const void* RenderPipeline::GetOutput() {
    if (!mInitialized) return nullptr;
    return mBuffers.MapReadback();
}

void RenderPipeline::AddPass(std::unique_ptr<ShaderPass> pass) {
    mPasses.push_back(std::move(pass));
}

} // namespace renderer
} // namespace fallout
