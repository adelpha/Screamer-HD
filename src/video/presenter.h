#pragma once
// Puts the finished picture (the 8-bit Framebuffer and its palette) in the window: scaled into
// a rectangle, with a backdrop behind widescreen menus, read back for screenshots, presented.
// Two ways to do it: SDL's 2D renderer (Direct3D on Windows; SDL's software renderer for
// headless runs) or our own OpenGL 3.3 context, which the GPU 3D renderer builds on.

#include "core/framebuffer.h"
#include "core/image.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>

namespace scr {
class WorkerPool;
class SceneSink;
}

namespace scr::video {

enum class Api { Sdl, OpenGL };

// OPTIONS > VIDEO > ADVANCED's picture quality (VideoSettings).
struct Quality {
    int filter = 0;     // the hardware renderer's textures: 0 nearest, 1 bilinear, 2 trilinear
    int aniso = 0;      // anisotropic filtering: 0 off, 1..4: 2x, 4x, 8x, 16x
    int msaa = 0;       // multisampling: 0 off, 1..3: 2x, 4x, 8x (and alpha to coverage)
    bool fog = false;   // the view distance's fade as fog towards the sky (off: the dither)
    int scaling = 0;    // the picture scaled up to the window: 0 sharp, 1 pixel, 2 smooth (3: nearest)
    bool operator==(const Quality&) const = default;
};
const char* api_name(Api api);                 // "sdl", "opengl"
bool parse_api(const std::string& s, Api& out);

class Presenter {
public:
    virtual ~Presenter() = default;
    // For logs: "direct3d11", "software", "opengl (NVIDIA ...)".
    virtual std::string name() const = 0;
    virtual Api api() const = 0;
    // The window's size in pixels (what draw() fills).
    virtual void output_size(int& w, int& h) const = 0;
    virtual void set_vsync(bool on) = 0;
    // A new picture to show (pool: threads for converting it, if that is needed).
    virtual void upload(const Framebuffer& fb, WorkerPool* pool) = 0;
    // The backdrop behind a widescreen menu: w x h RGBA pixels, stretched over the whole window
    // with a linear filter (null: none).
    virtual void set_backdrop(const uint8_t* rgba, int w, int h) = 0;
    // Clears the window to black, draws the backdrop (brightness 0..1 dims it; < 0: none) and
    // the last uploaded picture into dst (window pixels). A picture larger than dst is filtered
    // down: halved (2x2 averages) while it is at least twice the size, then linear; a smaller
    // one is scaled up with nearest-neighbour.
    virtual void draw(const SDL_FRect& dst, float backdrop_brightness) = 0;
    // What the last draw() made, at output_size().
    virtual SurfacePtr read_pixels() = 0;
    virtual void present() = 0;
    // The hardware renderer, where this presenter has one (null: the software renderer only).
    // A layered Framebuffer (Framebuffer::set_layered) is shown over the last scene drawn by it.
    virtual SceneSink* scene_sink() { return nullptr; }
    virtual void set_quality(const Quality& q) = 0;
    // The last uploaded picture as shown before scaling (the Framebuffer's size; with a layered
    // one, the scene and what is over it), or null where the picture is the Framebuffer itself.
    virtual SurfacePtr read_picture() { return nullptr; }
};

struct SdlOptions {
    std::string renderer;  // SDL renderer name (empty: SDL's choice)
    std::string upload;    // "index" | "rgba" | empty (index unless the renderer is "software" or the picture is filtered down)
    bool real_output = true;  // false for the offscreen headless driver: no filtering passes
};

// null (with the error logged) when the renderer cannot be made.
std::unique_ptr<Presenter> make_sdl_presenter(SDL_Window* window, const SdlOptions& o);
// The window must have been made with window_flags(Api::OpenGL).
std::unique_ptr<Presenter> make_gl_presenter(SDL_Window* window);

// Flags (and, for OpenGL, the context attributes) for a window the api will draw into.
SDL_WindowFlags window_flags(Api api);

} // namespace scr::video
