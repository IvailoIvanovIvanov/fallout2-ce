#ifndef FALLOUT_GPU_TEXTURE_H_
#define FALLOUT_GPU_TEXTURE_H_

#include <cstdint>

struct ID3D12Resource;
struct D3D12_CPU_DESCRIPTOR_HANDLE;
struct D3D12_GPU_DESCRIPTOR_HANDLE;

namespace fallout {

/**
 * @brief GPU texture format enumeration
 */
enum class GpuTextureFormat {
    ARGB8888,    // 32-bit ARGB (R8G8B8A8_UNORM)
    RGBA8888,    // 32-bit RGBA
};

/**
 * @brief GPU texture usage flags
 */
enum class GpuTextureUsage {
    DEFAULT = 0,
    RENDER_TARGET = 1 << 0,
    SHADER_RESOURCE = 1 << 1,
    UNORDERED_ACCESS = 1 << 2,
};

/**
 * @brief Opaque handle to GPU texture resource
 */
struct GpuTextureHandle {
    void* resource = nullptr;
};

/**
 * @brief Create a GPU texture resource
 * 
 * @param width Texture width in pixels
 * @param height Texture height in pixels
 * @param format Texture format (see GpuTextureFormat)
 * @param usage Usage flags (see GpuTextureUsage)
 * @return Valid GpuTextureHandle on success, null handle on failure
 */
GpuTextureHandle gpuTextureCreate(int width, int height, GpuTextureFormat format, int usage);

/**
 * @brief Release a GPU texture resource
 * 
 * @param handle Handle to texture resource
 */
void gpuTextureRelease(GpuTextureHandle handle);

/**
 * @brief Upload CPU buffer data to GPU texture
 * 
 * @param handle Texture resource handle
 * @param data CPU buffer with pixel data
 * @param dataSize Size of CPU buffer in bytes
 * @return true if upload succeeded, false otherwise
 */
bool gpuTextureUpload(GpuTextureHandle handle, const void* data, int dataSize);

/**
 * @brief Download GPU texture data to CPU buffer
 * 
 * @param handle Texture resource handle
 * @param outData Output CPU buffer
 * @param dataSize Size of output buffer in bytes
 * @return true if download succeeded, false otherwise
 */
bool gpuTextureDownload(GpuTextureHandle handle, void* outData, int dataSize);

/**
 * @brief Get GPU resource pointer from texture handle
 * 
 * @param handle Texture resource handle
 * @return Raw ID3D12Resource pointer, or nullptr if invalid
 */
ID3D12Resource* gpuTextureGetResource(GpuTextureHandle handle);

/**
 * @brief Get texture dimensions
 * 
 * @param handle Texture resource handle
 * @param outWidth Output width
 * @param outHeight Output height
 * @return true if handle is valid, false otherwise
 */
bool gpuTextureGetDimensions(GpuTextureHandle handle, int& outWidth, int& outHeight);

/**
 * @brief Get SRV or UAV descriptor handles for texture
 * 
 * Creates descriptors on a shared descriptor heap for texture access in shaders.
 * For SRV (Shader Resource View): read-only texture access in pixel/compute shaders
 * For UAV (Unordered Access View): read-write texture access in compute shaders
 * 
 * @param handle Texture resource handle
 * @param isSRV true for SRV (read-only), false for UAV (read-write)
 * @param outCPU Output CPU descriptor handle (for CreateShaderResourceView/CreateUnorderedAccessView)
 * @param outGPU Output GPU descriptor handle (for SetComputeRootDescriptorTable)
 * @return true if descriptor creation succeeded, false otherwise
 */
bool gpuTextureGetDescriptor(GpuTextureHandle handle, bool isSRV, void* outCPU, void* outGPU);

/**
 * @brief Create shared texture between SDL and compute
 * 
 * Useful for directly sharing rendered content with FSR2 compute.
 * 
 * @param width Texture width in pixels
 * @param height Texture height in pixels
 * @return Valid GpuTextureHandle on success, null handle on failure
 */
GpuTextureHandle gpuTextureCreateShared(int width, int height);

}  // namespace fallout

#endif // FALLOUT_GPU_TEXTURE_H_
