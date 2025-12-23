#ifndef FALLOUT_OPENGL_CONTEXT_H_
#define FALLOUT_OPENGL_CONTEXT_H_

#include "gpu_context.h"
#include <SDL.h>
#include <map>

namespace fallout {
namespace renderer {

class OpenGLContext : public GpuContext {
public:
    OpenGLContext(SDL_Window* window);
    ~OpenGLContext() override;

    bool Init() override;
    void Shutdown() override;

    void* CreateTexture(const TextureDesc& desc, const void* initialData = nullptr) override;
    void DestroyTexture(void* textureHandle) override;

    bool CreateComputeShader(const std::string& source, void** outShader) override;
    void Dispatch(void* shader, int x, int y, int z) override;

    void BindTexture(int slot, void* textureHandle) override;
    void BindUnorderedAccessView(int slot, void* textureHandle) override;
    void SetConstants(int slot, const void* data, int size) override;

    void BeginFrame() override;
    void EndFrame() override;

    void UpdateTexture(void* textureHandle, const void* data, int width, int height) override;
    void ReadbackTexture(void* textureHandle, void* data, int size) override;

private:
    SDL_Window* mWindow;
    SDL_GLContext mGLContext;
    std::map<int, unsigned int> mConstantBuffers;
    
    // Helper to compile GLSL
    unsigned int CompileShader(unsigned int type, const std::string& source);
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_OPENGL_CONTEXT_H_
