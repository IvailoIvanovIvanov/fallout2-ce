#ifndef FALLOUT_UPSCALER_ML_H
#define FALLOUT_UPSCALER_ML_H

#include <d3d12.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>

// Forward declarations to avoid including ONNX headers here
namespace Ort { struct Env; struct Session; struct SessionOptions; struct Value; }

namespace fallout {

/**
 * @brief Machine Learning Upscaler Backend
 * 
 * Wraps ONNX Runtime with DirectML execution provider to run
 * deep learning models (like Real-ESRGAN) on the GPU.
 */
class UpscalerML {
public:
    UpscalerML();
    ~UpscalerML();

    /**
     * @brief Initialize the ML engine
     * @param device The D3D12 device to use for inference
     * @param modelPath Path to the .onnx model file
     * @param logger Optional callback for logging diagnostics
     * @return true if initialization succeeded
     */
    bool init(ID3D12Device* device, const std::string& modelPath, 
              std::function<void(const char*)> logger = nullptr);

    /**
     * @brief Execute the upscaling model (CPU buffer interface)
     * 
     * @param inputBuffer Pointer to input RGBA data (width * height * 4 bytes)
     * @param outputBuffer Pointer to output RGBA data (width * height * 4 bytes * scale^2)
     * @param width Input width
     * @param height Input height
     * @return true if dispatch succeeded
     */
    bool dispatch(const uint32_t* inputBuffer, uint32_t* outputBuffer, int width, int height);

    /**
     * @brief Execute the upscaling model (GPU resource interface)
     * 
     * @param inputResource D3D12 resource containing input image (Planar Float32)
     * @param outputBuffer Pointer to CPU buffer to receive upscaled image (RGBA8)
     * @param width Input width
     * @param height Input height
     * @return true if dispatch succeeded
     */
    bool dispatchGPU(ID3D12Resource* inputResource, uint32_t* outputBuffer, int width, int height);

    /**
     * @brief Execute the upscaling model (GPU-to-GPU interface)
     * 
     * @param inputResource D3D12 resource containing input image (Planar Float32)
     * @param outputResource D3D12 resource to receive upscaled image (Planar Float32)
     * @param width Input width
     * @param height Input height
     * @return true if dispatch succeeded
     */
    bool dispatchGpuToGpu(ID3D12Resource* inputResource, ID3D12Resource* outputResource, int width, int height);

    /**
     * @brief Check if ML upscaling is supported on this hardware
     */
    bool isSupported() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
    
    // Helper to manage tensor shapes and bindings
    void updateBindings(int inputW, int inputH);
};

} // namespace fallout

#endif // FALLOUT_UPSCALER_ML_H
