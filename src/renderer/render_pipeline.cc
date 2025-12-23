#include "render_pipeline.h"
#include "opengl_context.h"
#include "../diagnostics.h"

#include "blur_filter.h"
#include "hdr_filter.h"

namespace fallout {
namespace renderer {

RenderPipeline::RenderPipeline() {
    mScalerPass = std::make_unique<ScalerPass>();
}

RenderPipeline::~RenderPipeline() {
    Shutdown();
}

bool RenderPipeline::Init(int inputWidth, int inputHeight, const RealDisplay& outputDisplay, SDL_Window* window) {
    if (mInitialized) {
        return true;
    }

    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    mOutputWidth = outputDisplay.GetWidth();
    mOutputHeight = outputDisplay.GetHeight();

    // Instantiate OpenGL Context
    // TODO: Make this configurable via fallout2.cfg
    mContext = std::make_unique<OpenGLContext>(window);

    if (!mContext->Init()) {
        diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Failed to initialize GpuContext");
        return false;
    }

    if (!mBuffers.Init(*mContext, inputWidth, inputHeight, mOutputWidth, mOutputHeight)) {
        diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Failed to initialize BufferManager");
        return false;
    }

    if (!CreateIntermediateBuffers()) {
        diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Failed to create intermediate buffers");
        return false;
    }

    // Initialize passes (Phantom Screen: 640x480 -> 640x480)
    for (auto& pass : mPasses) {
        if (!pass->Init(*mContext, inputWidth, inputHeight, inputWidth, inputHeight)) {
            return false;
        }
    }

    // Initialize Scaler Pass (640x480 -> WindowSize)
    if (!mScalerPass->Init(*mContext, inputWidth, inputHeight, mOutputWidth, mOutputHeight)) {
        diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Failed to initialize ScalerPass");
        return false;
    }

    mInitialized = true;
    return true;
}

bool RenderPipeline::CreateIntermediateBuffers() {
    TextureDesc desc = { mInputWidth, mInputHeight, TextureFormat::RGBA8 };

    for (int i = 0; i < 2; ++i) {
        mIntermediateBuffers[i] = mContext->CreateTexture(desc);
        if (!mIntermediateBuffers[i]) return false;
    }
    return true;
}

void RenderPipeline::Shutdown() {
    if (!mContext) return;

    for (auto& pass : mPasses) {
        pass->Shutdown(*mContext);
    }
    mPasses.clear();
    
    if (mScalerPass) mScalerPass->Shutdown(*mContext);

    for (int i = 0; i < 2; ++i) {
        if (mIntermediateBuffers[i]) {
            mContext->DestroyTexture(mIntermediateBuffers[i]);
            mIntermediateBuffers[i] = nullptr;
        }
    }

    mBuffers.Shutdown(*mContext);
    mContext->Shutdown();
    mInitialized = false;
}

void RenderPipeline::Dispatch(const PhantomDisplay& display) {
    if (!mInitialized) return;

    // 1. Swap Buffers (Move to next frame)
    mBuffers.SwapBuffers();

    // 2. Begin Frame
    mContext->BeginFrame();

    // 3. Upload Input
    if (!mBuffers.UploadInput(*mContext, display.GetPixels(), display.GetWidth() * display.GetHeight() * 4)) {
        diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Failed to upload input");
    }

    // 4. Execute Filter Chain
    void* currentInput = mBuffers.GetInputBuffer();
    int targetIndex = 0;

    diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Executing %zu passes", mPasses.size());
    int passIndex = 0;
    for (auto& pass : mPasses) {
        diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Executing pass %d", passIndex++);
        void* currentOutput = mIntermediateBuffers[targetIndex];

        pass->Execute(*mContext, currentInput, currentOutput);

        currentInput = currentOutput; // Output becomes input for next pass
        targetIndex = 1 - targetIndex;
    }

    // 5. Execute Scaler Pass
    void* outputBuffer = mBuffers.GetOutputBuffer();
    mScalerPass->Execute(*mContext, currentInput, outputBuffer);

    // 6. End Frame (Readback happens on demand via GetOutput)
    mContext->EndFrame();
}

const void* RenderPipeline::GetOutput() {
    if (!mInitialized) return nullptr;
    return mBuffers.ReadbackOutput(*mContext);
}

void RenderPipeline::AddPass(std::unique_ptr<ShaderPass> pass) {
    if (mInitialized) {
        // Initialize with Phantom Resolution (640x480)
        if (!pass->Init(*mContext, mInputWidth, mInputHeight, mInputWidth, mInputHeight)) {
            diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Failed to initialize added pass");
            return;
        }
    }
    mPasses.push_back(std::move(pass));
}

void RenderPipeline::SetScalerPass(std::unique_ptr<ShaderPass> pass) {
    mScalerPass = std::move(pass);
    if (mInitialized && mScalerPass) {
        if (!mScalerPass->Init(*mContext, mInputWidth, mInputHeight, mOutputWidth, mOutputHeight)) {
            diagnosticsLog(DiagnosticsLevel::Info, "RenderPipeline", "Failed to initialize scaler pass");
        }
    }
}

} // namespace renderer
} // namespace fallout
