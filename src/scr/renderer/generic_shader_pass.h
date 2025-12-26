#ifndef FALLOUT_RENDERER_GENERIC_SHADER_PASS_H
#define FALLOUT_RENDERER_GENERIC_SHADER_PASS_H

/**
 * @file generic_shader_pass.h
 * @brief Flexible shader pass supporting both standard compute shaders and MPV format.
 *
 * GenericShaderPass provides a unified interface for executing:
 * - Standard GLSL compute shaders
 * - MPV-format shaders (Anime4K, FSRCNNX, etc.)
 *
 * MPV shaders are multi-pass shaders that can define intermediate textures
 * and complex processing chains within a single file.
 */

#include "shader_pass.h"
#include "shader.h"
#include "mpv_shader_parser.h"
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace fallout {
namespace renderer {

/**
 * @class GenericShaderPass
 * @brief Executes standard or MPV-format shaders with automatic resource management.
 *
 * This class handles the complexity of MPV shader format, which can include:
 * - Multiple render passes (HOOK directives)
 * - Intermediate textures with configurable formats
 * - Size calculations relative to input/output dimensions
 * - Automatic texture binding and uniform setup
 *
 * For standard compute shaders, it provides simple bind-and-dispatch execution.
 */
class GenericShaderPass : public ShaderPass {
public:
    /**
     * @brief Constructs a shader pass from a file path.
     * @param shaderPath Path to the shader file (.glsl or MPV format).
     */
    explicit GenericShaderPass(const std::string& shaderPath);
    ~GenericShaderPass() override = default;

    //-------------------------------------------------------------------------
    // ShaderPass Interface
    //-------------------------------------------------------------------------

    bool Init(GpuContext& context, const RenderSurface& input, const RenderSurface& output) override;
    void Execute(GpuContext& context, const RenderSurface& input, const RenderSurface& output) override;
    void Shutdown(GpuContext& context) override;
    std::string GetName() const override;

private:
    //-------------------------------------------------------------------------
    // Initialization Helpers
    //-------------------------------------------------------------------------

    /**
     * @brief Attempts to open shader file from multiple search paths.
     * @param[out] source The loaded shader source code.
     * @return true if file was found and loaded.
     */
    bool LoadShaderSource(std::string& source);

    /**
     * @brief Initializes MPV-format shader pipeline.
     */
    bool InitMPVShader(GpuContext& context, const std::string& source,
                       const RenderSurface& input, const RenderSurface& output);

    /**
     * @brief Initializes standard compute shader.
     */
    bool InitStandardShader(const std::string& source);

    //-------------------------------------------------------------------------
    // Execution Helpers
    //-------------------------------------------------------------------------

    /**
     * @brief Executes a standard (non-MPV) compute shader.
     */
    void ExecuteStandardShader(GpuContext& context, const RenderSurface& input, 
                                const RenderSurface& output);

    /**
     * @brief Executes the full MPV shader chain.
     */
    void ExecuteMPVChain(GpuContext& context, const RenderSurface& input, 
                          const RenderSurface& output);

    /**
     * @brief Executes a single MPV pass.
     */
    void ExecuteMPVPass(GpuContext& context, const MPVPass& pass,
                         const RenderSurface& input, const RenderSurface& output,
                         void*& currentHooked, std::map<std::string, std::pair<int, int>>& sizes);

    /**
     * @brief Resolves the output texture for an MPV pass.
     * @return Pointer to the output texture handle.
     */
    void* ResolvePassOutput(GpuContext& context, const MPVPass& pass,
                            const RenderSurface& output,
                            std::map<std::string, std::pair<int, int>>& sizes,
                            TextureFormat& outFormat, bool& needsBlit);

    /**
     * @brief Binds input textures and sets uniforms for an MPV pass.
     */
    void BindPassInputs(GpuContext& context, const MPVPass& pass,
                         const RenderSurface& input, void* currentHooked,
                         const std::map<std::string, std::pair<int, int>>& sizes);

    /**
     * @brief Performs final blit if MPV output doesn't match target size.
     */
    void BlitToOutput(GpuContext& context, void* source, 
                       int srcWidth, int srcHeight,
                       const RenderSurface& output);

    //-------------------------------------------------------------------------
    // Texture Management
    //-------------------------------------------------------------------------

    /**
     * @brief Retrieves a named texture from the cache.
     */
    void* GetTexture(const std::string& name);

    /**
     * @brief Creates and caches a new texture.
     */
    void CreateTexture(GpuContext& context, const std::string& name, int width, int height);

    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------

    std::string mShaderPath;

    /// Standard shader (used when mIsMPV is false)
    std::unique_ptr<Shader> mShader;

    /// MPV mode flag
    bool mIsMPV = false;

    /// Parsed MPV passes
    std::vector<MPVPass> mPasses;

    /// Named texture cache for MPV intermediate buffers
    std::map<std::string, void*> mTextures;

    /// Blit shader for final resize if needed
    std::unique_ptr<Shader> mBlitShader;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_GENERIC_SHADER_PASS_H
