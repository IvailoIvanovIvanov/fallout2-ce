#ifndef FALLOUT_RENDERER_MPV_SHADER_PARSER_H
#define FALLOUT_RENDERER_MPV_SHADER_PARSER_H

/**
 * @file mpv_shader_parser.h
 * @brief Parser for MPV-compatible shader format (Anime4K, FSRCNNX, etc.).
 *
 * MPV shaders use a special comment-based directive syntax that allows
 * defining multi-pass processing chains, intermediate textures, and
 * dimension calculations within a single GLSL file.
 *
 * Supported Directives:
 * - //!HOOK <target>     - Specifies which texture this pass hooks into
 * - //!BIND <texture>    - Binds an input texture
 * - //!SAVE <name>       - Saves output to a named texture
 * - //!WIDTH <expr>      - Output width expression
 * - //!HEIGHT <expr>     - Output height expression
 * - //!DESC <text>       - Human-readable description
 */

#include "shader.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace fallout {
namespace renderer {

/**
 * @struct MPVPass
 * @brief Represents a single processing pass from an MPV shader.
 *
 * Each pass has its own compiled shader, input texture bindings,
 * output destination, and computed output dimensions.
 */
struct MPVPass {
    /// Compiled compute shader for this pass
    std::shared_ptr<Shader> shader;

    /// Names of textures to bind as inputs
    std::vector<std::string> inputTextures;

    /// Name of output texture (empty or "MAIN" for final output)
    std::string outputTexture;

    /// Computed output dimensions
    int width = 0;
    int height = 0;
};

/**
 * @class MPVShaderParser
 * @brief Parses MPV-format shaders into executable passes.
 *
 * Takes an MPV shader source and produces a sequence of MPVPass
 * objects that can be executed in order to produce the final result.
 *
 * The parser handles:
 * - Splitting multi-pass shaders into individual passes
 * - Computing output dimensions based on expressions
 * - Wrapping GLSL fragment code into compute shaders
 * - Managing texture binding requirements
 */
class MPVShaderParser {
public:
    /**
     * @brief Parses MPV shader source into executable passes.
     * @param source Complete MPV shader source code.
     * @param inputWidth Input texture width.
     * @param inputHeight Input texture height.
     * @param outputWidth Target output width.
     * @param outputHeight Target output height.
     * @return Vector of passes to execute in order.
     */
    static std::vector<MPVPass> Parse(const std::string& source,
                                       int inputWidth, int inputHeight,
                                       int outputWidth, int outputHeight);

private:
    /**
     * @brief Parses a dimension expression from an MPV directive.
     * @param line The directive line containing the expression.
     * @param defaultVal Default value if parsing fails.
     * @param isWidth true for width, false for height.
     * @param textureSizes Map of known texture names to their sizes.
     * @return Computed dimension value.
     */
    static int ParseDimension(const std::string& line, int defaultVal, bool isWidth,
                               const std::map<std::string, std::pair<int, int>>& textureSizes);
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_MPV_SHADER_PARSER_H
