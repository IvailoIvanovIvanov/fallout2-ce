#ifndef FALLOUT_RENDERER_MPV_SHADER_PARSER_H
#define FALLOUT_RENDERER_MPV_SHADER_PARSER_H

#include "shader.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace fallout {
namespace renderer {

struct MPVPass {
    std::shared_ptr<Shader> shader;
    std::vector<std::string> inputTextures;
    std::string outputTexture;
    int width;
    int height;
};

class MPVShaderParser {
public:
    static std::vector<MPVPass> Parse(const std::string& source, int inputWidth, int inputHeight, int outputWidth, int outputHeight);

private:
    static int ParseDimension(const std::string& line, int defaultVal, bool isWidth, const std::map<std::string, std::pair<int, int>>& textureSizes);
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_MPV_SHADER_PARSER_H
