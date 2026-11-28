// The picture through SDL's 2D renderer (what every build before the OpenGL path used).

#include "video/presenter.h"

#include <array>
#include <cstring>
#include <vector>

namespace scr::video {

const char* api_name(Api api) { return api == Api::OpenGL ? "opengl" : "sdl"; }

bool parse_api(const std::string& s, Api& out) {
    if (s == "sdl") out = Api::Sdl;
    else if (s == "opengl" || s == "gl") out = Api::OpenGL;
    else return false;
    return true;
}

SDL_WindowFlags window_flags(Api api) {
    if (api != Api::OpenGL) return 0;
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // (the window's own buffer only receives finished pictures: no depth, no multisampling)
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    return SDL_WINDOW_OPENGL;
}

namespace {

class SdlPresenter final : public Presenter {
public:
    SdlPresenter(SDL_Renderer* r, const SdlOptions& o) : r_(r), opt_(o) {
        // SDL's software renderer (headless runs) scales RGBA much faster than indexed pictures
        if (opt_.upload.empty() && std::strcmp(SDL_GetRendererName(r_), "software") == 0) opt_.upload = "rgba";
    }
    ~SdlPresenter() override {
        destroy_texture();
        for (SDL_Texture* t : halves_)
            if (t) SDL_DestroyTexture(t);
        if (backdrop_) SDL_DestroyTexture(backdrop_);
        SDL_DestroyRenderer(r_);
    }

    std::string name() const override { return SDL_GetRendererName(r_); }
    Api api() const override { return Api::Sdl; }
    void output_size(int& w, int& h) const override { SDL_GetRenderOutputSize(r_, &w, &h); }
    void set_vsync(bool on) override { SDL_SetRenderVSync(r_, on ? 1 : 0); }
    // (no hardware renderer: only the scaling counts; SHARP is nearest-neighbour here)
    void set_quality(const Quality& q) override { smooth_ = q.scaling == 2; }

    void upload(const Framebuffer& fb, WorkerPool* pool) override {
        // The picture goes to the renderer as it is, one byte a pixel, with its palette (the
        // graphics card looks the colours up); renderers that cannot get it converted to RGBA,
        // and so does a picture larger than the window: it is filtered down, which takes colours.
        int out_w = 0, out_h = 0;
        SDL_GetRenderOutputSize(r_, &out_w, &out_h);
        const bool larger = opt_.real_output && out_w > 0 && (fb.width() > out_w || fb.height() > out_h);
        const bool want_index = opt_.upload != "rgba" && !(opt_.upload.empty() && larger);
        if (!tex_ || fb.width() != tex_w_ || fb.height() != tex_h_ || want_index != want_index_) make_texture(fb, want_index);
        if (!tex_) return;
        if (palette_) {
            std::array<SDL_Color, 256> colors;
            for (size_t i = 0; i < 256; ++i) colors[i] = {fb.palette()[i].r, fb.palette()[i].g, fb.palette()[i].b, 255};
            if (!colors_set_ || std::memcmp(colors.data(), colors_.data(), sizeof colors) != 0) {
                SDL_SetPaletteColors(palette_, colors.data(), 0, 256);
                colors_ = colors;
                colors_set_ = true;
            }
            SDL_UpdateTexture(tex_, nullptr, fb.pixels(), fb.width());
        } else {
            void* pixels;
            int pitch;
            if (SDL_LockTexture(tex_, nullptr, &pixels, &pitch)) {
                fb.to_rgba(static_cast<uint8_t*>(pixels), pitch, pool);
                SDL_UnlockTexture(tex_);
            }
        }
    }

    void set_backdrop(const uint8_t* rgba, int w, int h) override {
        if (backdrop_) SDL_DestroyTexture(backdrop_);
        backdrop_ = nullptr;
        if (!rgba || w <= 0 || h <= 0) return;
        backdrop_ = SDL_CreateTexture(r_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
        if (!backdrop_) return;
        SDL_UpdateTexture(backdrop_, nullptr, rgba, w * 4);
        SDL_SetTextureScaleMode(backdrop_, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(backdrop_, SDL_BLENDMODE_NONE);
    }

    void draw(const SDL_FRect& dst, float backdrop_brightness) override {
        // A picture at least twice the size it is shown at (a render size above the screen's) is
        // halved on the graphics card until it is less than that: each pass of the linear filter
        // at exactly half the size averages 2x2 pixels, so every pixel counts (a box filter, as
        // supersampling wants); the last step is the linear filter's.
        SDL_Texture* shown = tex_;
        if (shown && !palette_ && opt_.real_output) {
            int cw = tex_w_, ch = tex_h_;
            for (size_t level = 0; cw >= 2 * int(dst.w) && ch >= 2 * int(dst.h) && cw >= 2 && ch >= 2; ++level) {
                const int nw = cw / 2, nh = ch / 2;
                if (halves_.size() <= level) halves_.push_back(nullptr);
                SDL_Texture*& t = halves_[level];
                float tw = 0, th = 0;
                if (t) SDL_GetTextureSize(t, &tw, &th);
                if (!t || int(tw) != nw || int(th) != nh) {
                    if (t) SDL_DestroyTexture(t);
                    t = SDL_CreateTexture(r_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, nw, nh);
                    if (!t) break;
                    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
                }
                SDL_SetTextureScaleMode(shown, SDL_SCALEMODE_LINEAR);
                if (!SDL_SetRenderTarget(r_, t)) break;
                // (source: the whole even-sized part, so 2x2 blocks line up)
                const SDL_FRect src{0, 0, float(nw * 2), float(nh * 2)};
                SDL_RenderTexture(r_, shown, &src, nullptr);
                shown = t;
                cw = nw;
                ch = nh;
            }
            SDL_SetRenderTarget(r_, nullptr);
            SDL_SetTextureScaleMode(shown, dst.w < float(cw) ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        }
        SDL_SetRenderDrawColor(r_, 0, 0, 0, 255);
        SDL_RenderClear(r_);
        if (backdrop_ && backdrop_brightness >= 0) {
            const auto m = uint8_t(SDL_clamp(backdrop_brightness, 0.0f, 1.0f) * 255.0f);
            SDL_SetTextureColorMod(backdrop_, m, m, m);
            SDL_RenderTexture(r_, backdrop_, nullptr, nullptr);
        }
        if (shown && shown == tex_ && dst.w >= float(tex_w_))
            SDL_SetTextureScaleMode(shown, smooth_ ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        if (shown) SDL_RenderTexture(r_, shown, nullptr, &dst);
    }

    SurfacePtr read_pixels() override { return SurfacePtr(SDL_RenderReadPixels(r_, nullptr)); }
    void present() override { SDL_RenderPresent(r_); }

private:
    void destroy_texture() {
        if (tex_) SDL_DestroyTexture(tex_);
        if (palette_) SDL_DestroyPalette(palette_);
        tex_ = nullptr;
        palette_ = nullptr;
        colors_set_ = false;
    }
    void make_texture(const Framebuffer& fb, bool want_index) {
        destroy_texture();
        tex_w_ = fb.width();
        tex_h_ = fb.height();
        want_index_ = want_index;
        if (want_index) {
            tex_ = SDL_CreateTexture(r_, SDL_PIXELFORMAT_INDEX8, SDL_TEXTUREACCESS_STREAMING, tex_w_, tex_h_);
            if (tex_) palette_ = SDL_CreatePalette(256);
            if (!palette_ || !SDL_SetTexturePalette(tex_, palette_) || !SDL_SetTextureBlendMode(tex_, SDL_BLENDMODE_NONE)) destroy_texture();
        }
        if (!tex_) {
            tex_ = SDL_CreateTexture(r_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, tex_w_, tex_h_);
            if (tex_) SDL_SetTextureBlendMode(tex_, SDL_BLENDMODE_NONE);  // (every pixel is opaque)
        }
        if (tex_) SDL_SetTextureScaleMode(tex_, SDL_SCALEMODE_NEAREST);
    }

    SDL_Renderer* r_;
    SdlOptions opt_;
    SDL_Texture* tex_ = nullptr;
    int tex_w_ = 0, tex_h_ = 0;
    bool want_index_ = false;
    SDL_Palette* palette_ = nullptr;
    std::array<SDL_Color, 256> colors_{};
    bool colors_set_ = false;
    std::vector<SDL_Texture*> halves_;  // the picture halved, for showing it much smaller
    SDL_Texture* backdrop_ = nullptr;
    bool smooth_ = false;
};

} // namespace

std::unique_ptr<Presenter> make_sdl_presenter(SDL_Window* window, const SdlOptions& o) {
    SDL_Renderer* r = SDL_CreateRenderer(window, o.renderer.empty() ? nullptr : o.renderer.c_str());
    if (!r) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "renderer: %s", SDL_GetError());
        return nullptr;
    }
    return std::make_unique<SdlPresenter>(r, o);
}

} // namespace scr::video
