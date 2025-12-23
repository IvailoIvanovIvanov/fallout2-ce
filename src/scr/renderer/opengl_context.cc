#include "opengl_context.h"
#include "logger.h"
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>
#include <vector>
#include <iostream>

namespace fallout {
namespace renderer {

// Function pointers
static PFNGLCREATESHADERPROC glCreateShader = nullptr;
static PFNGLSHADERSOURCEPROC glShaderSource = nullptr;
static PFNGLCOMPILESHADERPROC glCompileShader = nullptr;
static PFNGLGETSHADERIVPROC glGetShaderiv = nullptr;
static PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = nullptr;
static PFNGLDELETESHADERPROC glDeleteShader = nullptr;
static PFNGLCREATEPROGRAMPROC glCreateProgram = nullptr;
static PFNGLATTACHSHADERPROC glAttachShader = nullptr;
static PFNGLLINKPROGRAMPROC glLinkProgram = nullptr;
static PFNGLGETPROGRAMIVPROC glGetProgramiv = nullptr;
static PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = nullptr;
static PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
static PFNGLDISPATCHCOMPUTEPROC glDispatchCompute = nullptr;
static PFNGLBINDIMAGETEXTUREPROC glBindImageTexture = nullptr;
static PFNGLTEXSTORAGE2DPROC glTexStorage2D = nullptr;
static PFNGLMEMORYBARRIERPROC glMemoryBarrier = nullptr;
static PFNGLUNIFORM1IPROC glUniform1i = nullptr;
static PFNGLUNIFORM1FPROC glUniform1f = nullptr;
static PFNGLUNIFORM2FPROC glUniform2f = nullptr;
static PFNGLUNIFORM2IPROC glUniform2i = nullptr;
static PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = nullptr;
static PFNGLGENBUFFERSPROC glGenBuffers = nullptr;
static PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
static PFNGLBUFFERDATAPROC glBufferData = nullptr;
static PFNGLBINDBUFFERBASEPROC glBindBufferBase = nullptr;
static PFNGLDELETEBUFFERSPROC glDeleteBuffers = nullptr;
static PFNGLACTIVETEXTUREPROC glActiveTexture = nullptr;
static PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers = nullptr;
static PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D = nullptr;
static PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer = nullptr;
static PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers = nullptr;

#define LOAD_GL_FUNC(name, type) \
    name = (type)SDL_GL_GetProcAddress(#name); \
    if (!name) { \
        Logger::Log(LogLevel::Error, "OpenGL: Failed to load " #name); \
        return false; \
    }

OpenGLContext::OpenGLContext(SDL_Window* window) : mWindow(window), mGLContext(nullptr) {}

OpenGLContext::~OpenGLContext() {
    Shutdown();
}

bool OpenGLContext::Init() {
    // Request OpenGL 4.3 context for Compute Shaders
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    mGLContext = SDL_GL_CreateContext(mWindow);
    if (!mGLContext) {
        Logger::Log(LogLevel::Error, "OpenGL: Failed to create OpenGL context");
        return false;
    }

    SDL_GL_MakeCurrent(mWindow, mGLContext);

    // Load extensions
    LOAD_GL_FUNC(glCreateShader, PFNGLCREATESHADERPROC);
    LOAD_GL_FUNC(glShaderSource, PFNGLSHADERSOURCEPROC);
    LOAD_GL_FUNC(glCompileShader, PFNGLCOMPILESHADERPROC);
    LOAD_GL_FUNC(glGetShaderiv, PFNGLGETSHADERIVPROC);
    LOAD_GL_FUNC(glGetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC);
    LOAD_GL_FUNC(glDeleteShader, PFNGLDELETESHADERPROC);
    LOAD_GL_FUNC(glCreateProgram, PFNGLCREATEPROGRAMPROC);
    LOAD_GL_FUNC(glAttachShader, PFNGLATTACHSHADERPROC);
    LOAD_GL_FUNC(glLinkProgram, PFNGLLINKPROGRAMPROC);
    LOAD_GL_FUNC(glGetProgramiv, PFNGLGETPROGRAMIVPROC);
    LOAD_GL_FUNC(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC);
    LOAD_GL_FUNC(glUseProgram, PFNGLUSEPROGRAMPROC);
    LOAD_GL_FUNC(glDispatchCompute, PFNGLDISPATCHCOMPUTEPROC);
    LOAD_GL_FUNC(glBindImageTexture, PFNGLBINDIMAGETEXTUREPROC);
    LOAD_GL_FUNC(glTexStorage2D, PFNGLTEXSTORAGE2DPROC);
    LOAD_GL_FUNC(glMemoryBarrier, PFNGLMEMORYBARRIERPROC);
    LOAD_GL_FUNC(glUniform1i, PFNGLUNIFORM1IPROC);
    LOAD_GL_FUNC(glUniform1f, PFNGLUNIFORM1FPROC);
    LOAD_GL_FUNC(glUniform2f, PFNGLUNIFORM2FPROC);
    LOAD_GL_FUNC(glUniform2i, PFNGLUNIFORM2IPROC);
    LOAD_GL_FUNC(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC);
    LOAD_GL_FUNC(glGenBuffers, PFNGLGENBUFFERSPROC);
    LOAD_GL_FUNC(glBindBuffer, PFNGLBINDBUFFERPROC);
    LOAD_GL_FUNC(glBufferData, PFNGLBUFFERDATAPROC);
    LOAD_GL_FUNC(glBindBufferBase, PFNGLBINDBUFFERBASEPROC);
    LOAD_GL_FUNC(glDeleteBuffers, PFNGLDELETEBUFFERSPROC);
    LOAD_GL_FUNC(glActiveTexture, PFNGLACTIVETEXTUREPROC);
    LOAD_GL_FUNC(glGenFramebuffers, PFNGLGENFRAMEBUFFERSPROC);
    LOAD_GL_FUNC(glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC);
    LOAD_GL_FUNC(glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC);
    LOAD_GL_FUNC(glBlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC);
    LOAD_GL_FUNC(glDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC);

    // Create FBO for presentation
    glGenFramebuffers(1, &mPresentFBO);

    Logger::Log(LogLevel::Info, "OpenGL: OpenGL 4.3 Context Initialized");
    return true;
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

void* OpenGLContext::CreateTexture(const TextureDesc& desc, const void* initialData) {
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    GLenum internalFormat = GL_RGBA8;
    if (desc.format == TextureFormat::RGBA16F) internalFormat = GL_RGBA16F;
    if (desc.format == TextureFormat::RGBA32F) internalFormat = GL_RGBA32F;

    glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, desc.width, desc.height);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (initialData) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, desc.width, desc.height, GL_RGBA, GL_UNSIGNED_BYTE, initialData);
    }

    return (void*)(uintptr_t)texture;
}

void OpenGLContext::DestroyTexture(void* textureHandle) {
    GLuint texture = (GLuint)(uintptr_t)textureHandle;
    glDeleteTextures(1, &texture);
}

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
    *outShader = (void*)(uintptr_t)program;
    return true;
}

void OpenGLContext::Dispatch(void* shader, int x, int y, int z) {
    GLuint program = (GLuint)(uintptr_t)shader;
    glUseProgram(program);
    glDispatchCompute(x, y, z);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

void OpenGLContext::BindTexture(int slot, void* textureHandle) {
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)textureHandle);
}

void OpenGLContext::BindUnorderedAccessView(int slot, void* textureHandle) {
    // Bind as image for compute write
    glBindImageTexture(slot, (GLuint)(uintptr_t)textureHandle, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
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

void OpenGLContext::BeginFrame() {
    // No explicit frame start needed for OpenGL compute currently
}

void OpenGLContext::EndFrame() {
    // Ensure all commands are submitted
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
}

void OpenGLContext::Present(void* textureHandle, int width, int height) {
    GLuint texture = (GLuint)(uintptr_t)textureHandle;

    // Bind the texture to our FBO
    glBindFramebuffer(GL_READ_FRAMEBUFFER, mPresentFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    // Bind default framebuffer as draw target
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    // Blit!
    // Flip vertically by swapping srcY0 and srcY1
    // Source texture is top-down (0,0 is top-left of image data), but OpenGL treats 0,0 as bottom-left.
    // So the image is upside down in the texture. We flip it back during blit.
    glBlitFramebuffer(0, height, width, 0, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // Cleanup
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void OpenGLContext::UpdateTexture(void* textureHandle, const void* data, int width, int height) {
    GLuint texture = (GLuint)(uintptr_t)textureHandle;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLContext::ReadbackTexture(void* textureHandle, void* data, int size) {
    GLuint texture = (GLuint)(uintptr_t)textureHandle;
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace renderer
} // namespace fallout
