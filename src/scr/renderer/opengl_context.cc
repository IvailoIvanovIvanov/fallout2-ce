#include "opengl_context.h"
#include "logger.h"
#include "gl_bindings.h"
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>
#include <vector>

namespace fallout {
namespace renderer {

//-----------------------------------------------------------------------------
// Construction / Destruction
//-----------------------------------------------------------------------------

OpenGLContext::OpenGLContext(SDL_Window* window) 
    : mWindow(window), mGLContext(nullptr) {}

OpenGLContext::~OpenGLContext() {
    Shutdown();
}

//-----------------------------------------------------------------------------
// Lifecycle
//-----------------------------------------------------------------------------

bool OpenGLContext::Init() {
    SetContextAttributes();
    
    if (!CreateContext()) {
        return false;
    }

    if (!LoadGLFunctions()) {
        Logger::Log(LogLevel::Error, "OpenGL: Failed to load GL functions");
        return false;
    }

    InitPresentationResources();
    Logger::Log(LogLevel::Info, "OpenGL: OpenGL 4.3 Context Initialized");
    return true;
}

void OpenGLContext::SetContextAttributes() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
}

bool OpenGLContext::CreateContext() {
    mGLContext = SDL_GL_CreateContext(mWindow);
    if (!mGLContext) {
        Logger::Log(LogLevel::Error, "OpenGL: Failed to create OpenGL context");
        return false;
    }
    SDL_GL_MakeCurrent(mWindow, mGLContext);
    return true;
}

void OpenGLContext::InitPresentationResources() {
    glGenFramebuffers(1, &mPresentFBO);
}

void OpenGLContext::Shutdown() {
    if (mPresentFBO) {
        glDeleteFramebuffers(1, &mPresentFBO);
        mPresentFBO = 0;
    }

    for (auto& pair : mConstantBuffers) {
        glDeleteBuffers(1, &pair.second);
    }
    mConstantBuffers.clear();

    if (mGLContext) {
        SDL_GL_DeleteContext(mGLContext);
        mGLContext = nullptr;
    }
}

//-----------------------------------------------------------------------------
// Texture Management
//-----------------------------------------------------------------------------

void* OpenGLContext::CreateTexture(const TextureDesc& desc, const void* initialData) {
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    GLenum internalFormat = ToGLInternalFormat(desc.format);
    glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, desc.width, desc.height);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (initialData) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, desc.width, desc.height, 
                        GL_RGBA, GL_UNSIGNED_BYTE, initialData);
    }

    return reinterpret_cast<void*>(static_cast<uintptr_t>(texture));
}

void OpenGLContext::DestroyTexture(void* textureHandle) {
    GLuint texture = static_cast<GLuint>(reinterpret_cast<uintptr_t>(textureHandle));
    glDeleteTextures(1, &texture);
}

//-----------------------------------------------------------------------------
// Shader Operations
//-----------------------------------------------------------------------------

bool OpenGLContext::CreateComputeShader(const std::string& source, void** outShader) {
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        Logger::Log(LogLevel::Error, "OpenGL: Shader compilation failed: %s", infoLog);
        return false;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        Logger::Log(LogLevel::Error, "OpenGL: Program linking failed: %s", infoLog);
        return false;
    }

    glDeleteShader(shader);
    *outShader = reinterpret_cast<void*>(static_cast<uintptr_t>(program));
    return true;
}

void OpenGLContext::Dispatch(void* shader, int x, int y, int z) {
    GLuint program = static_cast<GLuint>(reinterpret_cast<uintptr_t>(shader));
    glUseProgram(program);
    glDispatchCompute(x, y, z);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

//-----------------------------------------------------------------------------
// State Binding
//-----------------------------------------------------------------------------

void OpenGLContext::BindTexture(int slot, void* textureHandle) {
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(textureHandle)));
}

void OpenGLContext::BindUnorderedAccessView(int slot, void* textureHandle, TextureFormat format) {
    GLenum glFormat = ToGLImageFormat(format);
    GLuint texture = static_cast<GLuint>(reinterpret_cast<uintptr_t>(textureHandle));
    glBindImageTexture(slot, texture, 0, GL_FALSE, 0, GL_READ_WRITE, glFormat);
}

void OpenGLContext::SetConstants(int slot, const void* data, int size) {
    GLuint ubo = 0;
    auto it = mConstantBuffers.find(slot);
    if (it == mConstantBuffers.end()) {
        glGenBuffers(1, &ubo);
        mConstantBuffers[slot] = ubo;
    } else {
        ubo = it->second;
    }

    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, size, data, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, slot, ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

//-----------------------------------------------------------------------------
// Frame Management
//-----------------------------------------------------------------------------

void OpenGLContext::BeginFrame() {
    // No explicit frame start needed for OpenGL compute
}

void OpenGLContext::EndFrame() {
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
}

//-----------------------------------------------------------------------------
// Presentation
//-----------------------------------------------------------------------------

void OpenGLContext::Present(void* textureHandle, int srcWidth, int srcHeight, 
                             int windowWidth, int windowHeight) {
    GLuint texture = static_cast<GLuint>(reinterpret_cast<uintptr_t>(textureHandle));

    glBindFramebuffer(GL_READ_FRAMEBUFFER, mPresentFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    int dstX, dstY, dstW, dstH;
    CalculatePresentRect(srcWidth, srcHeight, windowWidth, windowHeight, dstX, dstY, dstW, dstH);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Flip vertically by swapping srcY0 and srcY1
    glBlitFramebuffer(0, srcHeight, srcWidth, 0, 
                      dstX, dstY, dstX + dstW, dstY + dstH, 
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void OpenGLContext::CalculatePresentRect(int srcWidth, int srcHeight, 
                                          int windowWidth, int windowHeight,
                                          int& outX, int& outY, int& outW, int& outH) {
    float srcAspect = static_cast<float>(srcWidth) / srcHeight;
    float winAspect = static_cast<float>(windowWidth) / windowHeight;

    if (winAspect > srcAspect) {
        // Window wider than source (pillarbox)
        outW = static_cast<int>(windowHeight * srcAspect);
        outH = windowHeight;
        outX = (windowWidth - outW) / 2;
        outY = 0;
    } else {
        // Window taller than source (letterbox)
        outW = windowWidth;
        outH = static_cast<int>(windowWidth / srcAspect);
        outX = 0;
        outY = (windowHeight - outH) / 2;
    }
}

//-----------------------------------------------------------------------------
// Data Transfer
//-----------------------------------------------------------------------------

void OpenGLContext::UpdateTexture(void* textureHandle, const void* data, int width, int height) {
    GLuint texture = static_cast<GLuint>(reinterpret_cast<uintptr_t>(textureHandle));
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLContext::ReadbackTexture(void* textureHandle, void* data, int size) {
    GLuint texture = static_cast<GLuint>(reinterpret_cast<uintptr_t>(textureHandle));
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

//-----------------------------------------------------------------------------
// Format Conversion Helpers
//-----------------------------------------------------------------------------

unsigned int OpenGLContext::ToGLInternalFormat(TextureFormat format) {
    switch (format) {
        case TextureFormat::R8_UNORM: return GL_R8;
        case TextureFormat::RGBA16F: return GL_RGBA16F;
        case TextureFormat::RGBA32F: return GL_RGBA32F;
        default: return GL_RGBA8;
    }
}

unsigned int OpenGLContext::ToGLImageFormat(TextureFormat format) {
    switch (format) {
        case TextureFormat::R8_UNORM: return GL_R8;
        case TextureFormat::RGBA16F: return GL_RGBA16F;
        case TextureFormat::RGBA32F: return GL_RGBA32F;
        default: return GL_RGBA8;
    }
}

} // namespace renderer
} // namespace fallout
