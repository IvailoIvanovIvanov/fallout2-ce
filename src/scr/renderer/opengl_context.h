#ifndef FALLOUT_OPENGL_CONTEXT_H_
#define FALLOUT_OPENGL_CONTEXT_H_

/**
 * @file opengl_context.h
 * @brief OpenGL 4.3 implementation of the GpuContext interface.
 *
 * Provides GPU operations using OpenGL compute shaders for image processing.
 * Requires OpenGL 4.3 or higher for compute shader support.
 */

#include "gpu_context.h"
#include <SDL.h>
#include <map>

namespace fallout {
namespace renderer {

/**
 * @class OpenGLContext
 * @brief OpenGL 4.3 implementation of GpuContext.
 *
 * Implements the GpuContext interface using OpenGL for:
 * - Texture creation and management
 * - Compute shader compilation and dispatch
 * - Frame presentation with aspect-correct scaling
 * - CPU/GPU data transfer
 */
class OpenGLContext : public GpuContext {
public:
    /**
     * @brief Constructs an OpenGL context for the given SDL window.
     * @param window SDL window to associate with this context.
     */
    explicit OpenGLContext(SDL_Window* window);
    ~OpenGLContext() override;

    //-------------------------------------------------------------------------
    // GpuContext Interface Implementation
    //-------------------------------------------------------------------------
    
    bool Init() override;
    void Shutdown() override;

    void* CreateTexture(const TextureDesc& desc, const void* initialData = nullptr) override;
    void DestroyTexture(void* textureHandle) override;

    bool CreateComputeShader(const std::string& source, void** outShader) override;
    void Dispatch(void* shader, int x, int y, int z) override;

    void BindTexture(int slot, void* textureHandle) override;
    void BindUnorderedAccessView(int slot, void* textureHandle, 
                                  TextureFormat format = TextureFormat::RGBA8) override;
    void SetConstants(int slot, const void* data, int size) override;

    void BeginFrame() override;
    void EndFrame() override;

    void UpdateTexture(void* textureHandle, const void* data, int width, int height) override;
    void ReadbackTexture(void* textureHandle, void* data, int size) override;
    void Present(void* textureHandle, int srcWidth, int srcHeight, 
                 int windowWidth, int windowHeight) override;

private:
    //-------------------------------------------------------------------------
    // Initialization Helpers
    //-------------------------------------------------------------------------
    
    /**
     * @brief Sets OpenGL context attributes before context creation.
     */
    void SetContextAttributes();
    
    /**
     * @brief Creates the SDL OpenGL context.
     * @return true if context creation succeeded.
     */
    bool CreateContext();
    
    /**
     * @brief Initializes presentation resources (FBOs).
     */
    void InitPresentationResources();

    //-------------------------------------------------------------------------
    // Presentation Helpers
    //-------------------------------------------------------------------------
    
    /**
     * @brief Calculates destination rectangle for aspect-correct presentation.
     * @param srcWidth Source texture width.
     * @param srcHeight Source texture height.
     * @param windowWidth Window width.
     * @param windowHeight Window height.
     * @param outX Output destination X position.
     * @param outY Output destination Y position.
     * @param outW Output destination width.
     * @param outH Output destination height.
     */
    void CalculatePresentRect(int srcWidth, int srcHeight, int windowWidth, int windowHeight,
                               int& outX, int& outY, int& outW, int& outH);

    //-------------------------------------------------------------------------
    // Format Conversion
    //-------------------------------------------------------------------------
    
    /**
     * @brief Converts TextureFormat enum to OpenGL internal format.
     */
    static unsigned int ToGLInternalFormat(TextureFormat format);
    
    /**
     * @brief Converts TextureFormat enum to OpenGL image format.
     */
    static unsigned int ToGLImageFormat(TextureFormat format);

    //-------------------------------------------------------------------------
    // Member Data
    //-------------------------------------------------------------------------
    
    SDL_Window* mWindow;
    SDL_GLContext mGLContext;
    std::map<int, unsigned int> mConstantBuffers;
    unsigned int mPresentFBO = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_OPENGL_CONTEXT_H_
