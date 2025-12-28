#include "gl_bindings.h"
#include <SDL.h>
#include "logger.h"

namespace fallout {
namespace renderer {

PFNGLCREATESHADERPROC glCreateShader = nullptr;
PFNGLSHADERSOURCEPROC glShaderSource = nullptr;
PFNGLCOMPILESHADERPROC glCompileShader = nullptr;
PFNGLGETSHADERIVPROC glGetShaderiv = nullptr;
PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = nullptr;
PFNGLDELETESHADERPROC glDeleteShader = nullptr;
PFNGLCREATEPROGRAMPROC glCreateProgram = nullptr;
PFNGLATTACHSHADERPROC glAttachShader = nullptr;
PFNGLLINKPROGRAMPROC glLinkProgram = nullptr;
PFNGLGETPROGRAMIVPROC glGetProgramiv = nullptr;
PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = nullptr;
PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
PFNGLDISPATCHCOMPUTEPROC glDispatchCompute = nullptr;
PFNGLBINDIMAGETEXTUREPROC glBindImageTexture = nullptr;
PFNGLTEXSTORAGE2DPROC glTexStorage2D = nullptr;
PFNGLMEMORYBARRIERPROC glMemoryBarrier = nullptr;
PFNGLUNIFORM1IPROC glUniform1i = nullptr;
PFNGLUNIFORM1FPROC glUniform1f = nullptr;
PFNGLUNIFORM2FPROC glUniform2f = nullptr;
PFNGLUNIFORM2IPROC glUniform2i = nullptr;
PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = nullptr;
PFNGLGENBUFFERSPROC glGenBuffers = nullptr;
PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
PFNGLBUFFERDATAPROC glBufferData = nullptr;
PFNGLBINDBUFFERBASEPROC glBindBufferBase = nullptr;
PFNGLDELETEBUFFERSPROC glDeleteBuffers = nullptr;
PFNGLACTIVETEXTUREPROC glActiveTexture = nullptr;
PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers = nullptr;
PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D = nullptr;
PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer = nullptr;
PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers = nullptr;
PFNGLUNIFORM3FVPROC glUniform3fv = nullptr;
PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv = nullptr;

#define LOAD_GL_FUNC(name, type) \
    name = (type)SDL_GL_GetProcAddress(#name); \
    if (!name) { \
        Logger::Log(LogLevel::Error, "Failed to load %s", #name); \
        return false; \
    }

bool LoadGLFunctions() {
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
    LOAD_GL_FUNC(glGenFramebuffers, PFNGLGENFRAMEBUFFERSPROC);
    LOAD_GL_FUNC(glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC);
    LOAD_GL_FUNC(glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC);
    LOAD_GL_FUNC(glBlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC);
    LOAD_GL_FUNC(glDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC);
    LOAD_GL_FUNC(glBindBuffer, PFNGLBINDBUFFERPROC);
    LOAD_GL_FUNC(glBufferData, PFNGLBUFFERDATAPROC);
    LOAD_GL_FUNC(glBindBufferBase, PFNGLBINDBUFFERBASEPROC);
    LOAD_GL_FUNC(glDeleteBuffers, PFNGLDELETEBUFFERSPROC);
    LOAD_GL_FUNC(glActiveTexture, PFNGLACTIVETEXTUREPROC);
    
    // Optional ones (might not be present in all contexts, but we try)
    glUniform3fv = (PFNGLUNIFORM3FVPROC)SDL_GL_GetProcAddress("glUniform3fv");
    glUniformMatrix4fv = (PFNGLUNIFORMMATRIX4FVPROC)SDL_GL_GetProcAddress("glUniformMatrix4fv");

    return true;
}

} // namespace renderer
} // namespace fallout
