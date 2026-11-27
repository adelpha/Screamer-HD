#pragma once
// OpenGL 3.3 core functions, loaded through SDL_GL_GetProcAddress once a context is current.
// The OpenGL 1.1 functions come straight from the system's GL library (opengl32 on Windows);
// everything newer is called through the pointers declared here, as gl::Name(...).

#include <SDL3/SDL_opengl.h>

namespace scr::gl {

#define SCR_GL_FUNCTIONS(X)                                                   \
    X(PFNGLACTIVETEXTUREPROC, ActiveTexture)                                  \
    X(PFNGLATTACHSHADERPROC, AttachShader)                                    \
    X(PFNGLBINDBUFFERPROC, BindBuffer)                                        \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer)                              \
    X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray)                              \
    X(PFNGLBLITFRAMEBUFFERPROC, BlitFramebuffer)                              \
    X(PFNGLBUFFERDATAPROC, BufferData)                                        \
    X(PFNGLBUFFERSUBDATAPROC, BufferSubData)                                  \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus)                \
    X(PFNGLCOMPILESHADERPROC, CompileShader)                                  \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram)                                  \
    X(PFNGLCREATESHADERPROC, CreateShader)                                    \
    X(PFNGLDELETEBUFFERSPROC, DeleteBuffers)                                  \
    X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers)                        \
    X(PFNGLDELETEPROGRAMPROC, DeleteProgram)                                  \
    X(PFNGLDELETESHADERPROC, DeleteShader)                                    \
    X(PFNGLDELETEVERTEXARRAYSPROC, DeleteVertexArrays)                        \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray)              \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D)                    \
    X(PFNGLGENBUFFERSPROC, GenBuffers)                                        \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers)                              \
    X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays)                              \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog)                          \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv)                                    \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog)                            \
    X(PFNGLGETSHADERIVPROC, GetShaderiv)                                      \
    X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation)                        \
    X(PFNGLLINKPROGRAMPROC, LinkProgram)                                      \
    X(PFNGLSHADERSOURCEPROC, ShaderSource)                                    \
    X(PFNGLUNIFORM1IPROC, Uniform1i)                                          \
    X(PFNGLUNIFORM1FPROC, Uniform1f)                                          \
    X(PFNGLUNIFORM2FPROC, Uniform2f)                                          \
    X(PFNGLUNIFORM4FPROC, Uniform4f)                                          \
    X(PFNGLUNIFORMMATRIX4FVPROC, UniformMatrix4fv)                            \
    X(PFNGLUSEPROGRAMPROC, UseProgram)                                        \
    X(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer)                      \
    X(PFNGLVERTEXATTRIBIPOINTERPROC, VertexAttribIPointer)

#define SCR_GL_DECLARE(type, name) extern type name;
SCR_GL_FUNCTIONS(SCR_GL_DECLARE)
#undef SCR_GL_DECLARE

// Loads the pointers for the current context; false (with the missing name logged) when the
// driver lacks one.
bool load();

// Compiles and links a vertex + fragment shader pair; 0 (with the log written) on failure.
GLuint make_program(const char* name, const char* vertex_src, const char* fragment_src);

} // namespace scr::gl
