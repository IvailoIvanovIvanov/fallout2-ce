#ifndef FALLOUT_RENDERER_BUFFER_MANAGER_H
#define FALLOUT_RENDERER_BUFFER_MANAGER_H

/**
 * @file buffer_manager.h
 * @brief Double-buffered GPU resource manager for frame synchronization.
 *
 * The BufferManager handles creation and management of double-buffered
 * GPU textures for input/output operations, enabling pipelined rendering
 * without GPU stalls from synchronous operations.
 */

#include "gpu_context.h"
#include "render_types.h"
#include <vector>
#include <memory>

namespace fallout {
namespace renderer {

/**
 * @class BufferManager
 * @brief Manages double-buffered GPU resources for pipelined rendering.
 *
 * Implements a double-buffering strategy where:
 * - Frame N: Upload input, execute shaders, write output
 * - Frame N-1: Readback output (already complete)
 *
 * This eliminates GPU/CPU synchronization stalls by always reading from
 * the previous frame's completed output.
 */
class BufferManager {
public:
    BufferManager();
    ~BufferManager();

    //-------------------------------------------------------------------------
    // Lifecycle
    //-------------------------------------------------------------------------

    /**
     * @brief Initializes double-buffered GPU resources.
     * @param context GPU context for resource creation.
     * @param inputWidth Width of input textures.
     * @param inputHeight Height of input textures.
     * @param outputWidth Width of output textures.
     * @param outputHeight Height of output textures.
     * @return true if initialization succeeded, false otherwise.
     */
    bool Init(GpuContext& context, int inputWidth, int inputHeight, 
              int outputWidth, int outputHeight);

    /**
     * @brief Releases all GPU resources.
     * @param context GPU context for resource destruction.
     */
    void Shutdown(GpuContext& context);

    //-------------------------------------------------------------------------
    // Frame Management
    //-------------------------------------------------------------------------

    /**
     * @brief Swaps to the next frame's buffer set.
     *
     * Call at the beginning of each frame before uploading new input data.
     * This switches which buffer set is "current" and which is "previous".
     */
    void SwapBuffers();

    //-------------------------------------------------------------------------
    // Data Transfer
    //-------------------------------------------------------------------------

    /**
     * @brief Uploads input pixel data to the current frame's input buffer.
     * @param context GPU context for data upload.
     * @param data Pointer to RGBA pixel data.
     * @param size Size of pixel data in bytes.
     * @return true if upload succeeded.
     */
    bool UploadInput(GpuContext& context, const void* data, size_t size);

    /**
     * @brief Reads back the previous frame's output data.
     * @param context GPU context for readback.
     * @return Pointer to RGBA pixel data, valid until next SwapBuffers().
     */
    const void* ReadbackOutput(GpuContext& context);

    //-------------------------------------------------------------------------
    // Surface Accessors
    //-------------------------------------------------------------------------

    /**
     * @brief Gets the current frame's input surface for shader binding.
     */
    RenderSurface GetInputSurface() const;

    /**
     * @brief Gets the current frame's output surface for shader binding.
     */
    RenderSurface GetOutputSurface() const;

    //-------------------------------------------------------------------------
    // Dimension Accessors
    //-------------------------------------------------------------------------

    int GetInputWidth() const { return mInputWidth; }
    int GetInputHeight() const { return mInputHeight; }
    int GetOutputWidth() const { return mOutputWidth; }
    int GetOutputHeight() const { return mOutputHeight; }
    
    Dimensions GetInputDimensions() const { return {mInputWidth, mInputHeight}; }
    Dimensions GetOutputDimensions() const { return {mOutputWidth, mOutputHeight}; }

private:
    /**
     * @struct FrameResources
     * @brief GPU resources for a single frame in the double-buffer system.
     */
    struct FrameResources {
        void* inputBuffer = nullptr;        ///< Input texture handle
        void* outputBuffer = nullptr;       ///< Output texture handle
        std::vector<uint8_t> readbackData;  ///< CPU-side readback storage
    };

    static constexpr int kFrameCount = 2;   ///< Double-buffering uses 2 frames
    
    /**
     * @brief Creates GPU resources for a single frame.
     */
    bool CreateFrameResources(GpuContext& context, FrameResources& frame,
                               const TextureDesc& inputDesc, const TextureDesc& outputDesc);
    
    /**
     * @brief Destroys GPU resources for a single frame.
     */
    void DestroyFrameResources(GpuContext& context, FrameResources& frame);
    
    FrameResources mFrames[kFrameCount];
    int mCurrentFrameIndex = 0;
    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_BUFFER_MANAGER_H
