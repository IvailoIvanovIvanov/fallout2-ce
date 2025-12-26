#ifndef FALLOUT_GPU_CONTEXT_H_
#define FALLOUT_GPU_CONTEXT_H_

/**
 * @file gpu_context.h
 * @brief Abstract GPU context interface for graphics backend abstraction.
 *
 * This file defines the GpuContext abstract class which provides a
 * hardware-agnostic interface for GPU operations. Concrete implementations
 * (e.g., OpenGLContext) provide the actual graphics API calls.
 */

#include <cstdint>
#include <string>
#include <vector>
#include "render_types.h"

namespace fallout {
namespace renderer {

/**
 * @class GpuContext
 * @brief Abstract interface for GPU operations.
 *
 * Provides a hardware-agnostic abstraction layer for:
 * - Texture resource management
 * - Compute shader execution
 * - State binding and management
 * - Frame lifecycle control
 * - Data transfer between CPU and GPU
 *
 * All methods are pure virtual and must be implemented by concrete backends.
 */
class GpuContext {
public:
    virtual ~GpuContext() = default;

    //-------------------------------------------------------------------------
    // Lifecycle Management
    //-------------------------------------------------------------------------

    /**
     * @brief Initializes the GPU context and graphics API.
     * @return true if initialization succeeded, false otherwise.
     */
    virtual bool Init() = 0;

    /**
     * @brief Releases all GPU resources and shuts down the context.
     */
    virtual void Shutdown() = 0;

    //-------------------------------------------------------------------------
    // Resource Management
    //-------------------------------------------------------------------------

    /**
     * @brief Creates a GPU texture resource.
     * @param desc Texture descriptor specifying dimensions and format.
     * @param initialData Optional pointer to initial pixel data (can be nullptr).
     * @return Opaque handle to the created texture, or nullptr on failure.
     */
    virtual void* CreateTexture(const TextureDesc& desc, const void* initialData = nullptr) = 0;

    /**
     * @brief Destroys a previously created texture and releases GPU memory.
     * @param textureHandle Handle obtained from CreateTexture().
     */
    virtual void DestroyTexture(void* textureHandle) = 0;

    //-------------------------------------------------------------------------
    // Compute Shader Operations
    //-------------------------------------------------------------------------

    /**
     * @brief Compiles a compute shader from source code.
     * @param source GLSL compute shader source code.
     * @param outShader Pointer to receive the compiled shader program handle.
     * @return true if compilation and linking succeeded, false otherwise.
     */
    virtual bool CreateComputeShader(const std::string& source, void** outShader) = 0;

    /**
     * @brief Dispatches a compute shader with the specified work group counts.
     * @param shader Handle to a compiled compute shader program.
     * @param x Number of work groups in the X dimension.
     * @param y Number of work groups in the Y dimension.
     * @param z Number of work groups in the Z dimension.
     */
    virtual void Dispatch(void* shader, int x, int y, int z) = 0;

    //-------------------------------------------------------------------------
    // State Binding
    //-------------------------------------------------------------------------

    /**
     * @brief Binds a texture to a shader sampler slot for reading.
     * @param slot Sampler binding slot index (0-based).
     * @param textureHandle Handle to the texture to bind.
     */
    virtual void BindTexture(int slot, void* textureHandle) = 0;

    /**
     * @brief Binds a texture as an unordered access view (UAV) for compute writes.
     * @param slot Image binding slot index.
     * @param textureHandle Handle to the texture to bind.
     * @param format Texture format for proper memory layout interpretation.
     */
    virtual void BindUnorderedAccessView(int slot, void* textureHandle, 
                                          TextureFormat format = TextureFormat::RGBA8) = 0;

    /**
     * @brief Sets constant buffer data for shader uniforms.
     * @param slot Uniform buffer binding slot.
     * @param data Pointer to the constant data.
     * @param size Size of the constant data in bytes.
     */
    virtual void SetConstants(int slot, const void* data, int size) = 0;

    //-------------------------------------------------------------------------
    // Frame Management
    //-------------------------------------------------------------------------

    /**
     * @brief Begins a new rendering frame. Call at the start of each frame.
     */
    virtual void BeginFrame() = 0;

    /**
     * @brief Ends the current frame and ensures all GPU commands are submitted.
     */
    virtual void EndFrame() = 0;

    /**
     * @brief Reconfigures the context for a new window size.
     * @param width New window width.
     * @param height New window height.
     * @return true if reconfiguration succeeded.
     */
    virtual bool Reconfigure(int width, int height) { return true; }

    /**
     * @brief Presents a texture to the screen with aspect-correct scaling.
     * @param textureHandle Handle to the texture to present.
     * @param srcWidth Width of the source texture in pixels.
     * @param srcHeight Height of the source texture in pixels.
     * @param windowWidth Width of the target window in pixels.
     * @param windowHeight Height of the target window in pixels.
     */
    virtual void Present(void* textureHandle, int srcWidth, int srcHeight, 
                         int windowWidth, int windowHeight) = 0;

    //-------------------------------------------------------------------------
    // Data Transfer
    //-------------------------------------------------------------------------

    /**
     * @brief Uploads pixel data from CPU to GPU texture.
     * @param textureHandle Handle to the destination texture.
     * @param data Pointer to RGBA pixel data.
     * @param width Width of the data in pixels.
     * @param height Height of the data in pixels.
     */
    virtual void UpdateTexture(void* textureHandle, const void* data, int width, int height) = 0;

    /**
     * @brief Reads texture data back from GPU to CPU memory.
     * @param textureHandle Handle to the source texture.
     * @param data Pointer to destination buffer for pixel data.
     * @param size Size of the destination buffer in bytes.
     */
    virtual void ReadbackTexture(void* textureHandle, void* data, int size) = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_GPU_CONTEXT_H_
