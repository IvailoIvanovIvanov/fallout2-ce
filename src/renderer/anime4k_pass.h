#ifndef FALLOUT_RENDERER_ANIME4K_PASS_H
#define FALLOUT_RENDERER_ANIME4K_PASS_H

#include "shader_pass.h"
#include <vector>
#include <string>
#include <map>

namespace fallout {
namespace renderer {

class Anime4kPass : public ShaderPass {
public:
    Anime4kPass();
    ~Anime4kPass() override;

    bool Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) override;
    void Execute(GpuContext& context, void* input, void* output) override;
    void Shutdown(GpuContext& context) override;

    enum class Version {
        V3_2,
        GAN_X4_UUL
    };

    void SetVersion(Version version);
    void SetStrength(float strength) { mStrength = strength; }

private:
    struct Pass {
        void* shader;
        std::vector<std::string> inputTextures;
        std::string outputTexture;
        int width;
        int height;
    };

    bool LoadShader(GpuContext& context);
    bool ParseMPVShader(GpuContext& context, const std::string& source);
    void* GetTexture(const std::string& name);
    void CreateTexture(GpuContext& context, const std::string& name, int width, int height);

    std::vector<Pass> mPasses;
    std::map<std::string, void*> mTextures;
    
    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
    float mStrength = 0.5f;
    Version mVersion = Version::V3_2;
    bool mDirty = false;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_ANIME4K_PASS_H
