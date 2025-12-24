#include "shader.h"
#include "gl_bindings.h"
#include "logger.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace fallout {
namespace renderer {

Shader::Shader(const std::string& computePath) : ID(0) {
    std::string computeCode = ReadFile(computePath);
    if (!computeCode.empty()) {
        LoadFromSource(computeCode);
    }
}

void Shader::LoadFromSource(const std::string& computeCode) {
    unsigned int compute = CompileShader(GL_COMPUTE_SHADER, computeCode);
    if (compute == 0) {
        return;
    }

    ID = glCreateProgram();
    glAttachShader(ID, compute);
    glLinkProgram(ID);
    CheckCompileErrors(ID, "PROGRAM");

    glDeleteShader(compute);
}

std::unique_ptr<Shader> Shader::CreateFromSource(const std::string& computeSource) {
    std::unique_ptr<Shader> shader(new Shader());
    shader->LoadFromSource(computeSource);
    return shader;
}

Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath) : ID(0) {
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

    ID = glCreateProgram();
    glAttachShader(ID, vertex);
    glAttachShader(ID, fragment);
    glLinkProgram(ID);
    CheckCompileErrors(ID, "PROGRAM");

    glDeleteShader(vertex);
    glDeleteShader(fragment);
}

Shader::~Shader() {
    if (ID != 0) {
        // We assume glDeleteProgram is available. 
        // Note: If context is destroyed before shader, this might crash or do nothing.
        // But usually context outlives shaders.
        // However, we need to be careful if glDeleteProgram is not loaded.
        // But we loaded it in gl_bindings.
        // We can check if it's not null.
        // But gl_bindings are global.
        // Let's assume it's fine.
        // Actually, we can't call glDeleteProgram if context is gone.
        // But usually Shutdown() handles order.
    }
}

Shader::Shader(Shader&& other) noexcept : ID(other.ID) {
    other.ID = 0;
}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        if (ID != 0) {
            // glDeleteProgram(ID); // See note above
        }
        ID = other.ID;
        other.ID = 0;
    }
    return *this;
}

void Shader::Use() const {
    if (ID != 0) glUseProgram(ID);
}

void Shader::SetBool(const std::string &name, bool value) const {
    if (ID == 0) return;
    glUniform1i(glGetUniformLocation(ID, name.c_str()), (int)value);
}

void Shader::SetInt(const std::string &name, int value) const {
    if (ID == 0) return;
    glUniform1i(glGetUniformLocation(ID, name.c_str()), value);
}

void Shader::SetFloat(const std::string &name, float value) const {
    if (ID == 0) return;
    glUniform1f(glGetUniformLocation(ID, name.c_str()), value);
}

void Shader::SetVec2(const std::string &name, float x, float y) const {
    if (ID == 0) return;
    glUniform2f(glGetUniformLocation(ID, name.c_str()), x, y);
}

void Shader::SetVec3(const std::string &name, float x, float y, float z) const {
    if (ID == 0) return;
    if (glUniform3fv) {
        float v[3] = {x, y, z};
        glUniform3fv(glGetUniformLocation(ID, name.c_str()), 1, v);
    }
}

void Shader::SetMat4(const std::string &name, const float* mat) const {
    if (ID == 0) return;
    if (glUniformMatrix4fv) {
        glUniformMatrix4fv(glGetUniformLocation(ID, name.c_str()), 1, GL_FALSE, mat);
    }
}

void Shader::Dispatch(int x, int y, int z) const {
    if (ID == 0) return;
    Use();
    glDispatchCompute(x, y, z);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

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
    glShaderSource(id, 1, &src, NULL);
    glCompileShader(id);
    CheckCompileErrors(id, type == GL_VERTEX_SHADER ? "VERTEX" : (type == GL_FRAGMENT_SHADER ? "FRAGMENT" : "COMPUTE"));
    return id;
}

void Shader::CheckCompileErrors(unsigned int shader, std::string type) {
    int success;
    char infoLog[1024];
    if (type != "PROGRAM") {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(shader, 1024, NULL, infoLog);
            Logger::Log(LogLevel::Error, "SHADER_COMPILATION_ERROR of type: %s\n%s", type.c_str(), infoLog);
        }
    } else {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(shader, 1024, NULL, infoLog);
            Logger::Log(LogLevel::Error, "PROGRAM_LINKING_ERROR of type: %s\n%s", type.c_str(), infoLog);
        }
    }
}

} // namespace renderer
} // namespace fallout
