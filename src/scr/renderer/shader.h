#ifndef FALLOUT_RENDERER_SHADER_H
#define FALLOUT_RENDERER_SHADER_H

/**
 * @file shader.h
 * @brief OpenGL shader program wrapper class.
 *
 * Provides RAII-style management of OpenGL shader programs,
 * supporting both compute shaders and vertex/fragment shader pairs.
 */

#include <string>
#include <memory>

namespace fallout {
namespace renderer {

/**
 * @class Shader
 * @brief Encapsulates an OpenGL shader program.
 *
 * Manages shader compilation, linking, uniform setting, and compute dispatch.
 * Follows RAII principles - shader resources are freed on destruction.
 *
 * Usage:
 * @code
 *   auto shader = Shader::CreateFromSource(computeCode);
 *   if (shader && shader->IsValid()) {
 *       shader->Dispatch(groupsX, groupsY, 1);
 *   }
 * @endcode
 */
class Shader {
public:
    //-------------------------------------------------------------------------
    // Constructors
    //-------------------------------------------------------------------------

    /**
     * @brief Creates a compute shader from a file path.
     * @param computePath Path to the compute shader source file.
     */
    explicit Shader(const std::string& computePath);
    
    /**
     * @brief Creates a vertex/fragment shader pair from file paths.
     * @param vertexPath Path to the vertex shader source file.
     * @param fragmentPath Path to the fragment shader source file.
     */
    Shader(const std::string& vertexPath, const std::string& fragmentPath);

    /**
     * @brief Creates a shader from source code string.
     * @param computeSource Compute shader GLSL source code.
     * @return Unique pointer to the created shader, or nullptr on failure.
     */
    static std::unique_ptr<Shader> CreateFromSource(const std::string& computeSource);

    ~Shader();

    // Non-copyable (prevents double deletion of GL program)
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    // Movable
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    //-------------------------------------------------------------------------
    // State Management
    //-------------------------------------------------------------------------

    /**
     * @brief Activates this shader program for subsequent GL operations.
     */
    void Use() const;

    /**
     * @brief Checks if the shader was compiled and linked successfully.
     */
    bool IsValid() const { return mProgramId != 0; }

    //-------------------------------------------------------------------------
    // Uniform Setters
    //-------------------------------------------------------------------------

    /**
     * @brief Sets a boolean uniform value.
     */
    void SetBool(const std::string& name, bool value) const;

    /**
     * @brief Sets an integer uniform value.
     */
    void SetInt(const std::string& name, int value) const;

    /**
     * @brief Sets a float uniform value.
     */
    void SetFloat(const std::string& name, float value) const;

    /**
     * @brief Sets a vec2 uniform value.
     */
    void SetVec2(const std::string& name, float x, float y) const;

    /**
     * @brief Sets a vec3 uniform value.
     */
    void SetVec3(const std::string& name, float x, float y, float z) const;

    /**
     * @brief Sets a mat4 uniform value.
     */
    void SetMat4(const std::string& name, const float* mat) const;

    //-------------------------------------------------------------------------
    // Compute Dispatch
    //-------------------------------------------------------------------------

    /**
     * @brief Dispatches the compute shader with specified work group counts.
     * @param x Number of work groups in X dimension.
     * @param y Number of work groups in Y dimension.
     * @param z Number of work groups in Z dimension.
     */
    void Dispatch(int x, int y, int z) const;

    // Legacy public member for backward compatibility
    unsigned int ID = 0;

private:
    Shader() : mProgramId(0) {}  // Private default constructor for CreateFromSource

    void LoadFromSource(const std::string& computeSource);
    std::string ReadFile(const std::string& path);
    unsigned int CompileShader(unsigned int type, const std::string& source);
    void CheckCompileErrors(unsigned int shader, const std::string& type);
    int GetUniformLocation(const std::string& name) const;

    unsigned int mProgramId = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_SHADER_H
