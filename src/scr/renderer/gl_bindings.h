#ifndef FALLOUT_RENDERER_GL_BINDINGS_H
#define FALLOUT_RENDERER_GL_BINDINGS_H

/**
 * @file gl_bindings.h
 * @brief OpenGL function pointer declarations and loader.
 *
 * This file declares function pointers for OpenGL 4.3+ functions
 * that are not part of the core library on Windows. These must be
 * loaded at runtime using SDL_GL_GetProcAddress().
 *
 * The LoadGLFunctions() function must be called after creating
 * an OpenGL context but before using any of the declared functions.
 */

#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>

namespace fallout {
namespace renderer {

//-----------------------------------------------------------------------------
// Shader Functions
//-----------------------------------------------------------------------------

extern PFNGLCREATESHADERPROC glCreateShader;
extern PFNGLSHADERSOURCEPROC glShaderSource;
extern PFNGLCOMPILESHADERPROC glCompileShader;
extern PFNGLGETSHADERIVPROC glGetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog;
extern PFNGLDELETESHADERPROC glDeleteShader;

//-----------------------------------------------------------------------------
// Program Functions
//-----------------------------------------------------------------------------

extern PFNGLCREATEPROGRAMPROC glCreateProgram;
extern PFNGLATTACHSHADERPROC glAttachShader;
extern PFNGLLINKPROGRAMPROC glLinkProgram;
extern PFNGLGETPROGRAMIVPROC glGetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog;
extern PFNGLUSEPROGRAMPROC glUseProgram;

//-----------------------------------------------------------------------------
// Compute Shader Functions
//-----------------------------------------------------------------------------

extern PFNGLDISPATCHCOMPUTEPROC glDispatchCompute;
extern PFNGLMEMORYBARRIERPROC glMemoryBarrier;

//-----------------------------------------------------------------------------
// Texture Functions
//-----------------------------------------------------------------------------

extern PFNGLBINDIMAGETEXTUREPROC glBindImageTexture;
extern PFNGLTEXSTORAGE2DPROC glTexStorage2D;
extern PFNGLACTIVETEXTUREPROC glActiveTexture;

//-----------------------------------------------------------------------------
// Uniform Functions
//-----------------------------------------------------------------------------

extern PFNGLUNIFORM1IPROC glUniform1i;
extern PFNGLUNIFORM1FPROC glUniform1f;
extern PFNGLUNIFORM2FPROC glUniform2f;
extern PFNGLUNIFORM2IPROC glUniform2i;
extern PFNGLUNIFORM3FVPROC glUniform3fv;
extern PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv;
extern PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation;

//-----------------------------------------------------------------------------
// Buffer Functions
//-----------------------------------------------------------------------------

extern PFNGLGENBUFFERSPROC glGenBuffers;
extern PFNGLBINDBUFFERPROC glBindBuffer;
extern PFNGLBUFFERDATAPROC glBufferData;
extern PFNGLBINDBUFFERBASEPROC glBindBufferBase;
extern PFNGLDELETEBUFFERSPROC glDeleteBuffers;

//-----------------------------------------------------------------------------
// Framebuffer Functions
//-----------------------------------------------------------------------------

extern PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
extern PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
extern PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer;
extern PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;

//-----------------------------------------------------------------------------
// Initialization
//-----------------------------------------------------------------------------

/**
 * @brief Loads all OpenGL extension function pointers.
 *
 * Must be called after creating an OpenGL context (via SDL_GL_CreateContext)
 * but before using any of the declared OpenGL functions.
 *
 * @return true if all required functions were loaded successfully.
 */
bool LoadGLFunctions();

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_GL_BINDINGS_H
