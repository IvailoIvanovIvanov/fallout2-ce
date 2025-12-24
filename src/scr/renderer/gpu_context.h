#ifndef FALLOUT_GPU_CONTEXT_H_
#define FALLOUT_GPU_CONTEXT_H_

#include <cstdint>
#include <string>
#include <vector>
#include "render_types.h"

namespace fallout {
namespace renderer {

class GpuContext {
public:
    virtual ~GpuContext() = default;

    virtual bool Init() = 0;
    virtual void Shutdown() = 0;

    // Resource Creation
    virtual void* CreateTexture(const TextureDesc& desc, const void* initialData = nullptr) = 0;
    virtual void DestroyTexture(void* textureHandle) = 0;

    // Compute
    virtual bool CreateComputeShader(const std::string& source, void** outShader) = 0;
    virtual void Dispatch(void* shader, int x, int y, int z) = 0;

    // State
    virtual void BindTexture(int slot, void* textureHandle) = 0;
    virtual void BindUnorderedAccessView(int slot, void* textureHandle, TextureFormat format = TextureFormat::RGBA8) = 0;
    virtual void SetConstants(int slot, const void* data, int size) = 0;

    // Frame Management
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void Present(void* textureHandle, int srcWidth, int srcHeight, int windowWidth, int windowHeight) = 0;

    // Data Transfer
    virtual void UpdateTexture(void* textureHandle, const void* data, int width, int height) = 0;
    virtual void ReadbackTexture(void* textureHandle, void* data, int size) = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_GPU_CONTEXT_H_
