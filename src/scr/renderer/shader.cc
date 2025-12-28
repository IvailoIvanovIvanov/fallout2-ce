#include "shader.h"
#include "gl_bindings.h"
#include "logger.h"
#include <fstream>
#include <sstream>

namespace fallout {
namespace renderer {

//-----------------------------------------------------------------------------
// Constructors and Factory
//-----------------------------------------------------------------------------

Shader::Shader(const std::string& computePath) : mProgramId(0), ID(0) {
    std::string computeCode = ReadFile(computePath);
    if (!computeCode.empty()) {
        LoadFromSource(computeCode);
    }
}

Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath) 
    : mProgramId(0), ID(0) {
    std::string vertexCode = ReadFile(vertexPath);
    std::string fragmentCode = ReadFile(fragmentPath);

    if (vertexCode.empty() || fragmentCode.empty()) {
        return;
    }

    unsigned int vertex = CompileShader(GL_VERTEX_SHADER, vertexCode);
    unsigned int fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentCode);

    if (vertex == 0 || fragment == 0) {
        return;
    }

    mProgramId = glCreateProgram();
    glAttachShader(mProgramId, vertex);
    glAttachShader(mProgramId, fragment);
    glLinkProgram(mProgramId);
    CheckCompileErrors(mProgramId, "PROGRAM");

    glDeleteShader(vertex);
    glDeleteShader(fragment);
    
    ID = mProgramId;  // Sync legacy member
}

std::unique_ptr<Shader> Shader::CreateFromSource(const std::string& computeSource) {
    std::unique_ptr<Shader> shader(new Shader());
    shader->LoadFromSource(computeSource);
    return shader;
}

void Shader::LoadFromSource(const std::string& computeCode) {
    unsigned int compute = CompileShader(GL_COMPUTE_SHADER, computeCode);
    if (compute == 0) {
        return;
    }

    mProgramId = glCreateProgram();
    glAttachShader(mProgramId, compute);
    glLinkProgram(mProgramId);
    CheckCompileErrors(mProgramId, "PROGRAM");

    glDeleteShader(compute);
    ID = mProgramId;  // Sync legacy member
}

//-----------------------------------------------------------------------------
// Destructor and Move Operations
//-----------------------------------------------------------------------------

Shader::~Shader() {
    // Note: GL context must still be valid for cleanup
    // Typically Shutdown() handles resource ordering
}

Shader::Shader(Shader&& other) noexcept 
    : mProgramId(other.mProgramId), ID(other.ID) {
    other.mProgramId = 0;
    other.ID = 0;
}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        mProgramId = other.mProgramId;
        ID = other.ID;
        other.mProgramId = 0;
        other.ID = 0;
    }
    return *this;
}

//-----------------------------------------------------------------------------
// State Management
//-----------------------------------------------------------------------------

void Shader::Use() const {
    if (mProgramId != 0) {
        glUseProgram(mProgramId);
    }
}

//-----------------------------------------------------------------------------
// Uniform Setters
//-----------------------------------------------------------------------------

int Shader::GetUniformLocation(const std::string& name) const {
    return glGetUniformLocation(mProgramId, name.c_str());
}

void Shader::SetBool(const std::string& name, bool value) const {
    if (mProgramId == 0) return;
    glUniform1i(GetUniformLocation(name), static_cast<int>(value));
}

void Shader::SetInt(const std::string& name, int value) const {
    if (mProgramId == 0) return;
    glUniform1i(GetUniformLocation(name), value);
}

void Shader::SetFloat(const std::string& name, float value) const {
    if (mProgramId == 0) return;
    glUniform1f(GetUniformLocation(name), value);
}

void Shader::SetVec2(const std::string& name, float x, float y) const {
    if (mProgramId == 0) return;
    glUniform2f(GetUniformLocation(name), x, y);
}

void Shader::SetVec3(const std::string& name, float x, float y, float z) const {
    if (mProgramId == 0) return;
    if (glUniform3fv) {
        float v[3] = {x, y, z};
        glUniform3fv(GetUniformLocation(name), 1, v);
    }
}

void Shader::SetMat4(const std::string& name, const float* mat) const {
    if (mProgramId == 0) return;
    if (glUniformMatrix4fv) {
        glUniformMatrix4fv(GetUniformLocation(name), 1, GL_FALSE, mat);
    }
}

//-----------------------------------------------------------------------------
// Compute Dispatch
//-----------------------------------------------------------------------------

void Shader::Dispatch(int x, int y, int z) const {
    if (mProgramId == 0) return;
    Use();
    glDispatchCompute(x, y, z);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

//-----------------------------------------------------------------------------
// File and Compilation Helpers
//-----------------------------------------------------------------------------

std::string Shader::ReadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::Log(LogLevel::Error, "Failed to open shader file: %s", path.c_str());
        return "";
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

unsigned int Shader::CompileShader(unsigned int type, const std::string& source) {
    unsigned int id = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(id, 1, &src, nullptr);
    glCompileShader(id);
    
    const char* typeName = (type == GL_VERTEX_SHADER) ? "VERTEX" : 
                           (type == GL_FRAGMENT_SHADER) ? "FRAGMENT" : "COMPUTE";
    CheckCompileErrors(id, typeName);
    return id;
}

void Shader::CheckCompileErrors(unsigned int shader, const std::string& type) {
    int success;
    char infoLog[1024];
    
    if (type != "PROGRAM") {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(shader, 1024, nullptr, infoLog);
            Logger::Log(LogLevel::Error, "SHADER_COMPILATION_ERROR of type: %s\n%s", 
                        type.c_str(), infoLog);
        }
    } else {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(shader, 1024, nullptr, infoLog);
            Logger::Log(LogLevel::Error, "PROGRAM_LINKING_ERROR of type: %s\n%s", 
                        type.c_str(), infoLog);
        }
    }
}

} // namespace renderer
} // namespace fallout
