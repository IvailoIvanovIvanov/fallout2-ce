#include "render_pipeline.h"
#include "../diagnostics.h"
#include <d3d12.h>

#include "blur_filter.h"
#include "hdr_filter.h"
#include "ml_upscale_pass.h"

namespace fallout {
namespace renderer {

RenderPipeline::RenderPipeline() {
    mScalerPass = std::make_unique<ScalerPass>();
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
        diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Failed to initialize D3D12Context");
        return false;
    }

    if (!mBuffers.Init(mContext, inputWidth, inputHeight, outputWidth, outputHeight)) {
        diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Failed to initialize BufferManager");
        return false;
    }

    if (!CreateIntermediateBuffers()) {
        diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Failed to create intermediate buffers");
        return false;
    }

    // Initialize passes (Phantom Screen: 640x480 -> 640x480)
    for (auto& pass : mPasses) {
        if (!pass->Init(mContext, inputWidth, inputHeight, inputWidth, inputHeight)) {
            return false;
        }
    }

    // Initialize Scaler Pass (640x480 -> WindowSize)
    if (!mScalerPass->Init(mContext, inputWidth, inputHeight, outputWidth, outputHeight)) {
        diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Failed to initialize ScalerPass");
        return false;
    }

    mInitialized = true;
    return true;
}

bool RenderPipeline::CreateIntermediateBuffers() {
    auto device = mContext.GetDevice();
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = mInputWidth;
    desc.Height = mInputHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (int i = 0; i < 2; ++i) {
        if (FAILED(device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&mIntermediateBuffers[i])))) {
            return false;
        }
    }
    return true;
}

void RenderPipeline::Shutdown() {
    for (auto& pass : mPasses) {
        pass->Shutdown();
    }
    mPasses.clear();
    
    if (mScalerPass) mScalerPass->Shutdown();

    mIntermediateBuffers[0].Reset();
    mIntermediateBuffers[1].Reset();

    mBuffers.Shutdown();
    mContext.Shutdown();
    mInitialized = false;
}

void RenderPipeline::Dispatch(const PhantomDisplay& display) {
    if (!mInitialized) return;

    // 1. Swap Buffers (Move to next frame)
    mBuffers.SwapBuffers();

    // 2. Begin Frame
    mContext.BeginFrame();

    // 3. Upload Input
    if (!mBuffers.UploadInput(mContext, display.GetPixels(), display.GetWidth() * display.GetHeight() * 4)) {
        diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Failed to upload input");
    }

    auto cmdList = mContext.GetCommandList();
    
    // 4. Execute Filter Chain
    ID3D12Resource* currentInput = mBuffers.GetInputBuffer();
    int targetIndex = 0;

    // Ensure InputBuffer is in SRV state (BufferManager leaves it in PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE)
    
    for (auto& pass : mPasses) {
        ID3D12Resource* currentOutput = mIntermediateBuffers[targetIndex].Get();

        // Transition Output to UAV
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = currentOutput;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; // Or previous state
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);

        // Transition Input to SRV (if it's an intermediate buffer)
        if (currentInput != mBuffers.GetInputBuffer()) {
            barrier.Transition.pResource = currentInput;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; // It was output of previous pass
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            cmdList->ResourceBarrier(1, &barrier);
        }

        pass->Execute(mContext, currentInput, currentOutput);

        // Transition Output to Common (or SRV for next pass)
        barrier.Transition.pResource = currentOutput;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON; // Reset to Common
        cmdList->ResourceBarrier(1, &barrier);
        
        // If input was intermediate, transition it back to Common too?
        if (currentInput != mBuffers.GetInputBuffer()) {
             barrier.Transition.pResource = currentInput;
             barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
             barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
             cmdList->ResourceBarrier(1, &barrier);
        }

        currentInput = currentOutput; // Output becomes input for next pass
        targetIndex = 1 - targetIndex;
    }

    // 5. Execute Scaler Pass
    // Input: currentInput (Could be InputBuffer or an Intermediate Buffer)
    // Output: BufferManager::OutputBuffer

    auto outputBuffer = mBuffers.GetOutputBuffer();

    // Transition Input to SRV
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    int barrierCount = 0;

    if (currentInput != mBuffers.GetInputBuffer()) {
        // It's an intermediate buffer in COMMON state (because we reset it above)
        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[barrierCount].Transition.pResource = currentInput;
        barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrierCount++;
    }

    // Transition Output to UAV
    barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[barrierCount].Transition.pResource = outputBuffer;
    barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrierCount++;

    cmdList->ResourceBarrier(barrierCount, barriers);

    mScalerPass->Execute(mContext, currentInput, outputBuffer);

    // Reset Input to COMMON if it was intermediate
    if (currentInput != mBuffers.GetInputBuffer()) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = currentInput;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);
    }

    // 6. Copy Output to Readback
    {
        auto readbackBuffer = mBuffers.GetCurrentReadbackBuffer();

        // Transition Output to COPY_SOURCE
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = outputBuffer;
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

        // Transition Output back to COMMON
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        cmdList->ResourceBarrier(1, &barrier);
    }

    // 7. End Frame
    mContext.EndFrame();
}

const void* RenderPipeline::GetOutput() {
    if (!mInitialized) return nullptr;
    return mBuffers.MapReadback();
}

void RenderPipeline::AddPass(std::unique_ptr<ShaderPass> pass) {
    if (mInitialized) {
        // Initialize with Phantom Resolution (640x480)
        if (!pass->Init(mContext, mInputWidth, mInputHeight, mInputWidth, mInputHeight)) {
            diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Failed to initialize added pass");
            return;
        }
    }
    mPasses.push_back(std::move(pass));
}

} // namespace renderer
} // namespace fallout
