#ifndef FALLOUT_RENDERER_SHADER_PASS_H
#define FALLOUT_RENDERER_SHADER_PASS_H

/**
 * @file shader_pass.h
 * @brief Abstract interface for render pipeline shader passes.
 *
 * Defines the ShaderPass interface which represents a single step
 * in the rendering pipeline. Each pass transforms input surfaces
 * to output surfaces using GPU shaders.
 */

#include "gpu_context.h"
#include "render_types.h"
#include <string>

namespace fallout {
namespace renderer {

/**
 * @class ShaderPass
 * @brief Abstract base class for render pipeline passes.
 *
 * A shader pass encapsulates a single rendering operation that:
 * - Reads from one or more input surfaces
 * - Executes shader processing
 * - Writes to an output surface
 *
 * Implementations include ScalerPass, GenericShaderPass, etc.
 */
class ShaderPass {
public:
    virtual ~ShaderPass() = default;

    /**
     * @brief Initializes the shader pass with input/output specifications.
     * @param context GPU context for resource creation.
     * @param input Input surface specification (dimensions, format).
     * @param output Output surface specification (dimensions, format).
     * @return true if initialization succeeded, false otherwise.
     */
    virtual bool Init(GpuContext& context, const RenderSurface& input, 
                      const RenderSurface& output) = 0;

    /**
     * @brief Executes the shader pass, transforming input to output.
     * @param context GPU context for shader execution.
     * @param input Input surface to read from.
     * @param output Output surface to write to.
     */
    virtual void Execute(GpuContext& context, const RenderSurface& input, 
                         const RenderSurface& output) = 0;

    /**
     * @brief Releases GPU resources used by this pass.
     * @param context GPU context for resource destruction.
     */
    virtual void Shutdown(GpuContext& context) = 0;

    /**
     * @brief Returns a human-readable name for this pass.
     * @return Pass name for logging and diagnostics.
     */
    virtual std::string GetName() const = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_SHADER_PASS_H
