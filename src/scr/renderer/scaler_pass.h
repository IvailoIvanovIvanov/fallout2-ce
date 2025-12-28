#ifndef FALLOUT_RENDERER_SCALER_PASS_H
#define FALLOUT_RENDERER_SCALER_PASS_H

/**
 * @file scaler_pass.h
 * @brief Final scaling pass with aspect ratio preservation.
 *
 * ScalerPass handles the final step of the rendering pipeline:
 * scaling the processed image to the output resolution while
 * maintaining aspect ratio and adding letterboxing as needed.
 */

#include "shader_pass.h"
#include "shader.h"
#include <memory>

namespace fallout {
namespace renderer {

/**
 * @class ScalerPass
 * @brief Shader pass for aspect-ratio preserving image scaling.
 *
 * Uses a compute shader to scale the input image to the output dimensions
 * while maintaining the original aspect ratio. Black bars are added
 * horizontally or vertically as needed (letterboxing/pillarboxing).
 *
 * The scaling uses bilinear interpolation for smooth results.
 */
class ScalerPass : public ShaderPass {
public:
    ScalerPass();
    ~ScalerPass() override;

    //-------------------------------------------------------------------------
    // ShaderPass Interface
    //-------------------------------------------------------------------------

    /**
     * @brief Loads the scaler compute shader.
     * @param context GPU context for resource creation.
     * @param input Input surface (dimensions used for scale calculation).
     * @param output Output surface (target dimensions).
     * @return true if shader loaded successfully.
     */
    bool Init(GpuContext& context, const RenderSurface& input, const RenderSurface& output) override;

    /**
     * @brief Executes the scaling operation.
     *
     * Calculates optimal scale factor, computes letterbox offsets,
     * and dispatches the compute shader.
     */
    void Execute(GpuContext& context, const RenderSurface& input, const RenderSurface& output) override;

    /**
     * @brief Releases shader resources.
     */
    void Shutdown(GpuContext& context) override;

    std::string GetName() const override { return "ScalerPass"; }

private:
    std::unique_ptr<Shader> mShader;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_SCALER_PASS_H
