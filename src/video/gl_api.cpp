#include "video/gl_api.h"

#include <SDL3/SDL.h>

#include <string>

namespace scr::gl {

#define SCR_GL_DEFINE(type, name) type name = nullptr;
SCR_GL_FUNCTIONS(SCR_GL_DEFINE)
#undef SCR_GL_DEFINE

bool load() {
    bool ok = true;
#define SCR_GL_LOAD(type, name)                                                           \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name));                     \
    if (!name) {                                                                          \
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "OpenGL: the driver has no gl" #name);       \
        ok = false;                                                                       \
    }
    SCR_GL_FUNCTIONS(SCR_GL_LOAD)
#undef SCR_GL_LOAD
    return ok;
}

namespace {

GLuint compile(const char* name, GLenum type, const char* src) {
    const GLuint s = CreateShader(type);
    ShaderSource(s, 1, &src, nullptr);
    CompileShader(s);
    GLint ok = 0;
    GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        GetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(size_t(len > 0 ? len : 1), '\0');
        GetShaderInfoLog(s, GLsizei(log.size()), nullptr, log.data());
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "OpenGL: %s %s shader: %s", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment",
                     log.c_str());
        DeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

GLuint make_program(const char* name, const char* vertex_src, const char* fragment_src) {
    const GLuint vs = compile(name, GL_VERTEX_SHADER, vertex_src);
    const GLuint fs = vs ? compile(name, GL_FRAGMENT_SHADER, fragment_src) : 0;
    if (!fs) {
        if (vs) DeleteShader(vs);
        return 0;
    }
    const GLuint p = CreateProgram();
    AttachShader(p, vs);
    AttachShader(p, fs);
    LinkProgram(p);
    DeleteShader(vs);
    DeleteShader(fs);
    GLint ok = 0;
    GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        GetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(size_t(len > 0 ? len : 1), '\0');
        GetProgramInfoLog(p, GLsizei(log.size()), nullptr, log.data());
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "OpenGL: %s program: %s", name, log.c_str());
        DeleteProgram(p);
        return 0;
    }
    return p;
}

} // namespace scr::gl
