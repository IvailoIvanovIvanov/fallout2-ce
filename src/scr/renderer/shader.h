#ifndef FALLOUT_RENDERER_SHADER_H
#define FALLOUT_RENDERER_SHADER_H

#include <string>
#include <memory>

namespace fallout {
namespace renderer {

class Shader {
public:
    unsigned int ID;

    // Constructor for Compute Shader
    Shader(const std::string& computePath);
    
    // Constructor for Vertex/Fragment Shader
    Shader(const std::string& vertexPath, const std::string& fragmentPath);

    static std::unique_ptr<Shader> CreateFromSource(const std::string& computeSource);

    ~Shader();

    // Disable copy to prevent double deletion of program
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    // Allow move
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    void Use() const;

    // Uniforms
    void SetBool(const std::string &name, bool value) const;
    void SetInt(const std::string &name, int value) const;
    void SetFloat(const std::string &name, float value) const;
    void SetVec2(const std::string &name, float x, float y) const;
    void SetVec3(const std::string &name, float x, float y, float z) const;
    void SetMat4(const std::string &name, const float* mat) const;

    void Dispatch(int x, int y, int z) const;

    bool IsValid() const { return ID != 0; }

private:
    Shader() : ID(0) {} // Private constructor for CreateFromSource
    void LoadFromSource(const std::string& computeSource);

    void CheckCompileErrors(unsigned int shader, std::string type);
    std::string ReadFile(const std::string& path);
    unsigned int CompileShader(unsigned int type, const std::string& source);
};

} // namespace renderer
} // namespace fallout

#endif
