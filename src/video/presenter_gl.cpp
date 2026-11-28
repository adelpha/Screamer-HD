// The picture through our own OpenGL 3.3 core context: the palette is looked up in a shader,
// and the scaling does what the SDL path does (the same picture), so the two can be compared.

#include "video/gl_api.h"
#include "video/gl_scene.h"
#include "video/presenter.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace scr::video {
namespace {

// A rectangle (window or target pixels, given in NDC by u_dst) showing u_src of a texture.
// Four vertices as a triangle strip, made from gl_VertexID: no vertex buffer.
constexpr const char* kQuadVs = R"(#version 330 core
uniform vec4 u_dst;  // x0, y0, x1, y1 in NDC (x0, y0: the picture's top left)
uniform vec4 u_src;  // u0, v0, u1, v1 (v0: the top row)
out vec2 v_uv;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(mix(u_dst.xy, u_dst.zw, c), 0.0, 1.0);
    v_uv = mix(u_src.xy, u_src.zw, c);
}
)";

// The picture in colour, one texel a pixel: its palette indices (R8) through the palette
// (256 x 1 RGBA); a layered picture (the hardware renderer) over the 3D scene where its cover
// byte says so (Framebuffer::kCovered), the scene darkened 0.75 times per step elsewhere.
constexpr const char* kComposeFs = R"(#version 330 core
uniform sampler2D u_index;
uniform sampler2D u_palette;
uniform sampler2D u_cover;
uniform sampler2D u_scene;
uniform int u_layered;
uniform float u_bright;
uniform ivec2 u_flip;
out vec4 o_color;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int i = int(texelFetch(u_index, p, 0).r * 255.0 + 0.5);
    vec3 c = texelFetch(u_palette, ivec2(i, 0), 0).rgb;
    if (u_layered != 0) {
        int cover = int(texelFetch(u_cover, p, 0).r * 255.0 + 0.5);
        if (cover != 255) {
            ivec2 size = textureSize(u_scene, 0);
            ivec2 q = p;
            if (u_flip.x != 0) q.x = size.x - 1 - q.x;
            if (u_flip.y != 0) q.y = size.y - 1 - q.y;
            c = texelFetch(u_scene, clamp(q, ivec2(0), size - 1), 0).rgb * u_bright * pow(0.75, float(cover));
        }
    }
    o_color = vec4(c, 1.0);
}
)";

constexpr const char* kRgbaFs = R"(#version 330 core
uniform sampler2D u_tex;
uniform float u_mul;
in vec2 v_uv;
out vec4 o_color;
void main() {
    o_color = vec4(texture(u_tex, v_uv).rgb * u_mul, 1.0);
}
)";

// SCALE SHARP: a picture scaled up by a factor that is not whole, as nearest-neighbour would
// but with the seams between its pixels blended over one window pixel (the linear filter's
// weights squeezed towards each texel's edges), so every pixel keeps the same size.
constexpr const char* kSharpFs = R"(#version 330 core
uniform sampler2D u_tex;
uniform vec2 u_size;   // the picture's pixels
uniform vec2 u_scale;  // window pixels per picture pixel
in vec2 v_uv;
out vec4 o_color;
void main() {
    vec2 t = v_uv * u_size;
    vec2 i = floor(t);
    vec2 d = t - i - 0.5;
    vec2 region = 0.5 - 0.5 / u_scale;
    vec2 f = (d - clamp(d, -region, region)) * u_scale + 0.5;
    o_color = vec4(texture(u_tex, (i + f) / u_size).rgb, 1.0);
}
)";

struct Target {
    GLuint fbo = 0, tex = 0;
    int w = 0, h = 0;
    void destroy() {
        if (fbo) gl::DeleteFramebuffers(1, &fbo);
        if (tex) glDeleteTextures(1, &tex);
        fbo = tex = 0;
        w = h = 0;
    }
    bool ensure(int nw, int nh) {
        if (fbo && w == nw && h == nh) return true;
        destroy();
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, nw, nh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl::GenFramebuffers(1, &fbo);
        gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        const bool ok = gl::CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!ok) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "OpenGL: cannot render into a %dx%d texture", nw, nh);
            destroy();
            return false;
        }
        w = nw;
        h = nh;
        return true;
    }
};

void set_filter(GLuint tex, GLint filter) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

class GlPresenter final : public Presenter {
public:
    GlPresenter(SDL_Window* window, SDL_GLContext ctx) : window_(window), ctx_(ctx) {}
    ~GlPresenter() override {
        SDL_GL_MakeCurrent(window_, ctx_);
        scene_.reset();
        if (index_tex_) glDeleteTextures(1, &index_tex_);
        if (cover_tex_) glDeleteTextures(1, &cover_tex_);
        if (palette_tex_) glDeleteTextures(1, &palette_tex_);
        if (backdrop_tex_) glDeleteTextures(1, &backdrop_tex_);
        rgba_.destroy();
        readback_.destroy();
        for (Target& t : halves_) t.destroy();
        if (compose_prog_) gl::DeleteProgram(compose_prog_);
        if (rgba_prog_) gl::DeleteProgram(rgba_prog_);
        if (sharp_prog_) gl::DeleteProgram(sharp_prog_);
        if (vao_) gl::DeleteVertexArrays(1, &vao_);
        SDL_GL_DestroyContext(ctx_);
    }

    bool init() {
        compose_prog_ = gl::make_program("picture", kQuadVs, kComposeFs);
        rgba_prog_ = gl::make_program("rgba", kQuadVs, kRgbaFs);
        sharp_prog_ = gl::make_program("sharp", kQuadVs, kSharpFs);
        if (!sharp_prog_) return false;
        gl::UseProgram(sharp_prog_);
        gl::Uniform1i(gl::GetUniformLocation(sharp_prog_, "u_tex"), 0);
        sharp_dst_ = gl::GetUniformLocation(sharp_prog_, "u_dst");
        sharp_src_ = gl::GetUniformLocation(sharp_prog_, "u_src");
        sharp_size_ = gl::GetUniformLocation(sharp_prog_, "u_size");
        sharp_scale_ = gl::GetUniformLocation(sharp_prog_, "u_scale");
        if (!compose_prog_ || !rgba_prog_) return false;
        gl::UseProgram(compose_prog_);
        gl::Uniform1i(gl::GetUniformLocation(compose_prog_, "u_index"), 0);
        gl::Uniform1i(gl::GetUniformLocation(compose_prog_, "u_palette"), 1);
        gl::Uniform1i(gl::GetUniformLocation(compose_prog_, "u_cover"), 2);
        gl::Uniform1i(gl::GetUniformLocation(compose_prog_, "u_scene"), 3);
        cmp_dst_ = gl::GetUniformLocation(compose_prog_, "u_dst");
        cmp_src_ = gl::GetUniformLocation(compose_prog_, "u_src");
        cmp_layered_ = gl::GetUniformLocation(compose_prog_, "u_layered");
        cmp_bright_ = gl::GetUniformLocation(compose_prog_, "u_bright");
        cmp_flip_ = gl::GetUniformLocation(compose_prog_, "u_flip");
        gl::UseProgram(rgba_prog_);
        gl::Uniform1i(gl::GetUniformLocation(rgba_prog_, "u_tex"), 0);
        rgba_dst_ = gl::GetUniformLocation(rgba_prog_, "u_dst");
        rgba_src_ = gl::GetUniformLocation(rgba_prog_, "u_src");
        rgba_mul_ = gl::GetUniformLocation(rgba_prog_, "u_mul");
        gl::GenVertexArrays(1, &vao_);

        glGenTextures(1, &palette_tex_);
        glBindTexture(GL_TEXTURE_2D, palette_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        if (glGetError() != GL_NO_ERROR) return false;
        scene_ = std::make_unique<GlScene>();
        if (!scene_->init()) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "OpenGL: no hardware renderer (its shaders did not build)");
            scene_.reset();
        }
        return true;
    }

    std::string name() const override {
        const auto* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const auto* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        return std::string("opengl (") + (r ? r : "?") + ", " + (v ? v : "?") + ")";
    }
    Api api() const override { return Api::OpenGL; }
    void output_size(int& w, int& h) const override { SDL_GetWindowSizeInPixels(window_, &w, &h); }
    void set_vsync(bool on) override { SDL_GL_SetSwapInterval(on ? 1 : 0); }
    SceneSink* scene_sink() override { return scene_.get(); }
    void set_quality(const Quality& q) override {
        quality_ = q;
        if (scene_) scene_->set_quality(q);
    }

    void upload(const Framebuffer& fb, WorkerPool*) override {
        SDL_GL_MakeCurrent(window_, ctx_);
        if (!index_tex_ || fb.width() != tex_w_ || fb.height() != tex_h_) {
            if (index_tex_) glDeleteTextures(1, &index_tex_);
            if (cover_tex_) glDeleteTextures(1, &cover_tex_);
            tex_w_ = fb.width();
            tex_h_ = fb.height();
            index_tex_ = make_r8(tex_w_, tex_h_);
            cover_tex_ = make_r8(tex_w_, tex_h_);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, index_tex_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_w_, tex_h_, GL_RED, GL_UNSIGNED_BYTE, fb.pixels());
        const GLuint scene = scene_ ? scene_->texture() : 0;
        const bool layered = fb.layered() && scene;
        if (layered) {
            glBindTexture(GL_TEXTURE_2D, cover_tex_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_w_, tex_h_, GL_RED, GL_UNSIGNED_BYTE, fb.cover());
        }
        std::array<uint8_t, 256 * 4> colors;
        for (size_t i = 0; i < 256; ++i) {
            colors[i * 4 + 0] = fb.palette()[i].r;
            colors[i * 4 + 1] = fb.palette()[i].g;
            colors[i * 4 + 2] = fb.palette()[i].b;
            colors[i * 4 + 3] = 255;
        }
        if (!colors_set_ || colors != colors_) {
            glBindTexture(GL_TEXTURE_2D, palette_tex_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, colors.data());
            colors_ = colors;
            colors_set_ = true;
        }
        // the picture in colour, one texel a pixel
        if (!rgba_.ensure(tex_w_, tex_h_)) return;
        gl::BindVertexArray(vao_);
        gl::BindFramebuffer(GL_FRAMEBUFFER, rgba_.fbo);
        glViewport(0, 0, tex_w_, tex_h_);
        gl::UseProgram(compose_prog_);
        gl::Uniform1i(cmp_layered_, layered ? 1 : 0);
        gl::Uniform1f(cmp_bright_, fb.scene_brightness);
        gl::Uniform2i(cmp_flip_, fb.scene_flip_x ? 1 : 0, fb.scene_flip_y ? 1 : 0);
        gl::ActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, layered ? scene : 0);
        gl::ActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, cover_tex_);
        gl::ActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, palette_tex_);
        gl::ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, index_tex_);
        quad(cmp_dst_, cmp_src_, 0, 0, float(tex_w_), float(tex_h_), tex_w_, tex_h_, false);
        gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
        composed_ = true;
    }

    void set_backdrop(const uint8_t* rgba, int w, int h) override {
        SDL_GL_MakeCurrent(window_, ctx_);
        if (backdrop_tex_) glDeleteTextures(1, &backdrop_tex_);
        backdrop_tex_ = 0;
        if (!rgba || w <= 0 || h <= 0) return;
        glGenTextures(1, &backdrop_tex_);
        glBindTexture(GL_TEXTURE_2D, backdrop_tex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    void draw(const SDL_FRect& dst, float backdrop_brightness) override {
        last_dst_ = dst;
        last_brightness_ = backdrop_brightness;
        drawn_ = true;
        draw_to(0, dst, backdrop_brightness);
    }

    SurfacePtr read_pixels() override {
        int w = 0, h = 0;
        output_size(w, h);
        if (!drawn_ || w <= 0 || h <= 0 || !readback_.ensure(w, h)) return nullptr;
        // (drawn again into a texture: a hidden or covered window's own pixels are undefined)
        draw_to(readback_.fbo, last_dst_, last_brightness_);
        return read_target(readback_.fbo, w, h);
    }

    SurfacePtr read_picture() override {
        if (!composed_) return nullptr;
        SDL_GL_MakeCurrent(window_, ctx_);
        return read_target(rgba_.fbo, tex_w_, tex_h_);
    }

    void present() override { SDL_GL_SwapWindow(window_); }

private:
    static GLuint make_r8(int w, int h) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return t;
    }

    // A target of ours (rows top down) as a surface.
    static SurfacePtr read_target(GLuint fbo, int w, int h) {
        SurfacePtr s(SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32));
        if (!s) return nullptr;
        gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
        for (int y = 0; y < h; ++y)
            std::memcpy(static_cast<uint8_t*>(s->pixels) + size_t(y) * size_t(s->pitch), px.data() + size_t(y) * size_t(w) * 4,
                        size_t(w) * 4);
        return s;
    }

    // Draws a quad into the bound target (tw x th pixels): dst in target pixels, top left
    // first. The window's own framebuffer has its rows bottom up, our textures top down.
    void quad(GLint dst_loc, GLint src_loc, float x0, float y0, float x1, float y1, int tw, int th, bool window,
              float u1 = 1.0f, float v1 = 1.0f) {
        const float nx0 = x0 / float(tw) * 2 - 1, nx1 = x1 / float(tw) * 2 - 1;
        float ny0 = y0 / float(th) * 2 - 1, ny1 = y1 / float(th) * 2 - 1;
        if (window) {
            ny0 = -ny0;
            ny1 = -ny1;
        }
        gl::Uniform4f(dst_loc, nx0, ny0, nx1, ny1);
        gl::Uniform4f(src_loc, 0, 0, u1, v1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    void draw_to(GLuint fbo, const SDL_FRect& dst, float backdrop_brightness) {
        SDL_GL_MakeCurrent(window_, ctx_);
        int ow = 0, oh = 0;
        output_size(ow, oh);
        gl::BindVertexArray(vao_);
        gl::ActiveTexture(GL_TEXTURE0);

        // What is shown: the picture in colour (upload), scaled up with nearest-neighbour, or,
        // when it is larger than the window, halved and filtered as the SDL path does.
        GLuint shown = composed_ ? rgba_.tex : 0;
        int cw = tex_w_, ch = tex_h_;
        const bool larger = shown && ow > 0 && (tex_w_ > ow || tex_h_ > oh);
        gl::UseProgram(rgba_prog_);
        gl::Uniform1f(rgba_mul_, 1.0f);
        if (larger) {
            for (size_t level = 0; cw >= 2 * int(dst.w) && ch >= 2 * int(dst.h) && cw >= 2 && ch >= 2; ++level) {
                const int nw = cw / 2, nh = ch / 2;
                if (halves_.size() <= level) halves_.emplace_back();
                Target& t = halves_[level];
                if (!t.ensure(nw, nh)) break;
                gl::BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
                glViewport(0, 0, nw, nh);
                set_filter(shown, GL_LINEAR);
                // (source: the whole even-sized part, so 2x2 blocks line up)
                quad(rgba_dst_, rgba_src_, 0, 0, float(nw), float(nh), nw, nh, false, float(nw * 2) / float(cw),
                     float(nh * 2) / float(ch));
                shown = t.tex;
                cw = nw;
                ch = nh;
            }
        }
        // (scaled up: as SCALE says; sharp only where the factor is not whole)
        const bool up = !larger || dst.w >= float(cw);
        const bool sharp = up && quality_.scaling == 0 && shown &&
                           (std::fabs(dst.w / float(cw) - std::round(dst.w / float(cw))) > 1e-3f ||
                            std::fabs(dst.h / float(ch) - std::round(dst.h / float(ch))) > 1e-3f);
        if (shown) set_filter(shown, (larger && dst.w < float(cw)) || sharp || (up && quality_.scaling == 2) ? GL_LINEAR : GL_NEAREST);

        gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, ow, oh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        const bool window = fbo == 0;
        if (backdrop_tex_ && backdrop_brightness >= 0) {
            // (as SDL's colour modulation: the brightness in whole 255ths)
            const float m = float(int(SDL_clamp(backdrop_brightness, 0.0f, 1.0f) * 255.0f)) / 255.0f;
            gl::Uniform1f(rgba_mul_, m);
            glBindTexture(GL_TEXTURE_2D, backdrop_tex_);
            quad(rgba_dst_, rgba_src_, 0, 0, float(ow), float(oh), ow, oh, window);
            gl::Uniform1f(rgba_mul_, 1.0f);
        }
        if (shown && sharp) {
            gl::UseProgram(sharp_prog_);
            gl::Uniform2f(sharp_size_, float(cw), float(ch));
            gl::Uniform2f(sharp_scale_, dst.w / float(cw), dst.h / float(ch));
            glBindTexture(GL_TEXTURE_2D, shown);
            quad(sharp_dst_, sharp_src_, dst.x, dst.y, dst.x + dst.w, dst.y + dst.h, ow, oh, window);
        } else if (shown) {
            glBindTexture(GL_TEXTURE_2D, shown);
            quad(rgba_dst_, rgba_src_, dst.x, dst.y, dst.x + dst.w, dst.y + dst.h, ow, oh, window);
        }
    }

    SDL_Window* window_;
    SDL_GLContext ctx_;
    std::unique_ptr<GlScene> scene_;
    GLuint compose_prog_ = 0, rgba_prog_ = 0, vao_ = 0;
    GLint cmp_dst_ = -1, cmp_src_ = -1, cmp_layered_ = -1, cmp_bright_ = -1, cmp_flip_ = -1;
    GLint rgba_dst_ = -1, rgba_src_ = -1, rgba_mul_ = -1;
    GLuint sharp_prog_ = 0;
    GLint sharp_dst_ = -1, sharp_src_ = -1, sharp_size_ = -1, sharp_scale_ = -1;
    Quality quality_;
    GLuint index_tex_ = 0, cover_tex_ = 0, palette_tex_ = 0, backdrop_tex_ = 0;
    int tex_w_ = 0, tex_h_ = 0;
    std::array<uint8_t, 256 * 4> colors_{};
    bool colors_set_ = false;
    bool composed_ = false;
    Target rgba_, readback_;
    std::vector<Target> halves_;
    SDL_FRect last_dst_{};
    float last_brightness_ = -1;
    bool drawn_ = false;
};

} // namespace

std::unique_ptr<Presenter> make_gl_presenter(SDL_Window* window) {
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "OpenGL 3.3: %s", SDL_GetError());
        return nullptr;
    }
    SDL_GL_MakeCurrent(window, ctx);
    if (!gl::load()) {
        SDL_GL_DestroyContext(ctx);
        return nullptr;
    }
    auto p = std::make_unique<GlPresenter>(window, ctx);
    if (!p->init()) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "OpenGL: setting up failed");
        return nullptr;
    }
    return p;
}

} // namespace scr::video
