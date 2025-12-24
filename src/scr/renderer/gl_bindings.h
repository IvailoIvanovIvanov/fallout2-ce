#ifndef FALLOUT_RENDERER_GL_BINDINGS_H
#define FALLOUT_RENDERER_GL_BINDINGS_H

#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>

namespace fallout {
namespace renderer {

// Declare function pointers as extern
extern PFNGLCREATESHADERPROC glCreateShader;
extern PFNGLSHADERSOURCEPROC glShaderSource;
extern PFNGLCOMPILESHADERPROC glCompileShader;
extern PFNGLGETSHADERIVPROC glGetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog;
extern PFNGLDELETESHADERPROC glDeleteShader;
extern PFNGLCREATEPROGRAMPROC glCreateProgram;
extern PFNGLATTACHSHADERPROC glAttachShader;
extern PFNGLLINKPROGRAMPROC glLinkProgram;
extern PFNGLGETPROGRAMIVPROC glGetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog;
extern PFNGLUSEPROGRAMPROC glUseProgram;
extern PFNGLDISPATCHCOMPUTEPROC glDispatchCompute;
extern PFNGLBINDIMAGETEXTUREPROC glBindImageTexture;
extern PFNGLTEXSTORAGE2DPROC glTexStorage2D;
extern PFNGLMEMORYBARRIERPROC glMemoryBarrier;
extern PFNGLUNIFORM1IPROC glUniform1i;
extern PFNGLUNIFORM1FPROC glUniform1f;
extern PFNGLUNIFORM2FPROC glUniform2f;
extern PFNGLUNIFORM2IPROC glUniform2i;
extern PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation;
extern PFNGLGENBUFFERSPROC glGenBuffers;
extern PFNGLBINDBUFFERPROC glBindBuffer;
extern PFNGLBUFFERDATAPROC glBufferData;
extern PFNGLBINDBUFFERBASEPROC glBindBufferBase;
extern PFNGLDELETEBUFFERSPROC glDeleteBuffers;
extern PFNGLACTIVETEXTUREPROC glActiveTexture;
extern PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
extern PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
extern PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer;
extern PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
extern PFNGLUNIFORM3FVPROC glUniform3fv;
extern PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv;

bool LoadGLFunctions();

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_GL_BINDINGS_H
