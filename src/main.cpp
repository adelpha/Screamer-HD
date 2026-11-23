// Screamer recompilation: SDL3 frontend.
//
// Until the original game logic is reconstructed this shows either the title
// screen from the user's game files (--scene title) or a deterministic test
// pattern (--scene testpattern) through the full render path. The command line is designed for
// automated testing: --headless runs without a visible window (SDL "offscreen"
// video driver, software renderer, dummy audio) and --frames/--screenshot/
// --dump-fb produce reproducible images the test harness compares.

#include "core/archive.h"
#include "core/framebuffer.h"
#include "core/gamedata.h"
#include "core/image.h"
#include "core/pcx.h"
#include "core/sim_clock.h"
#include "core/testpattern.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace {

struct Options {
    std::string data_dir;
    bool headless = false;
    bool fullscreen = false;
    uint64_t frames = 0;            // 0 = run until quit
    std::string screenshot;         // rendered output after the last frame
    std::string dump_fb;            // native-resolution framebuffer after the last frame
    uint64_t capture_every = 0;     // dump framebuffer every N frames into capture_dir
    std::string capture_dir = "artifacts/capture";
    int width = 0, height = 0;      // output size; 0 = default
    std::string renderer;           // SDL renderer name, empty = default
    std::string aspect = "4:3";     // 4:3 | stretch | square
    std::string scene;              // title | testpattern; empty = title when game data is found
};

void print_usage() {
    std::printf(
        "screamer %s\n"
        "Usage: screamer [options]\n"
        "  --data DIR           original game directory (default: $SCREAMER_DATA or build-time path)\n"
        "  --headless           no visible window, software renderer, dummy audio\n"
        "  --frames N           exit after N simulation frames (2 ticks of the game's 100 Hz clock each)\n"
        "  --scene NAME         title (needs the game files; default when found) or testpattern\n"
        "  --screenshot FILE    save the rendered window output after the last frame (.png/.bmp)\n"
        "  --dump-fb FILE       save the native 320x200 framebuffer after the last frame\n"
        "  --capture-every N    save the framebuffer every N frames\n"
        "  --capture-dir DIR    directory for --capture-every output (default artifacts/capture)\n"
        "  --size WxH           window / output size\n"
        "  --fullscreen         borderless fullscreen\n"
        "  --renderer NAME      SDL renderer (direct3d11, direct3d12, vulkan, opengl, software, gpu)\n"
        "  --aspect MODE        4:3 (default), stretch, square\n"
        "  --version, --help\n",
        SCREAMER_VERSION);
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); std::exit(2); }
            return argv[++i];
        };
        if (a == "--data") o.data_dir = next("--data");
        else if (a == "--headless") o.headless = true;
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a == "--frames") o.frames = std::strtoull(next("--frames"), nullptr, 10);
        else if (a == "--screenshot") o.screenshot = next("--screenshot");
        else if (a == "--dump-fb") o.dump_fb = next("--dump-fb");
        else if (a == "--capture-every") o.capture_every = std::strtoull(next("--capture-every"), nullptr, 10);
        else if (a == "--capture-dir") o.capture_dir = next("--capture-dir");
        else if (a == "--renderer") o.renderer = next("--renderer");
        else if (a == "--aspect") o.aspect = next("--aspect");
        else if (a == "--scene") o.scene = next("--scene");
        else if (a == "--size") {
            if (std::sscanf(next("--size"), "%dx%d", &o.width, &o.height) != 2 || o.width <= 0 || o.height <= 0) {
                std::fprintf(stderr, "--size expects WxH\n");
                return false;
            }
        } else if (a == "--version") { std::printf("screamer %s\n", SCREAMER_VERSION); std::exit(0); }
        else if (a == "--help" || a == "-h") { print_usage(); std::exit(0); }
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); print_usage(); return false; }
    }
    if (o.width == 0) { o.width = o.headless ? 640 : 1280; o.height = o.headless ? 480 : 960; }
    return true;
}

void ensure_parent_dir(const std::string& file) {
    std::error_code ec;
    const auto parent = std::filesystem::path(file).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
}

bool save_or_log(SDL_Surface* s, const std::string& path) {
    ensure_parent_dir(path);
    if (!scr::save_image(s, path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "failed to save %s: %s", path.c_str(), SDL_GetError());
        return false;
    }
    SDL_Log("saved %s", path.c_str());
    return true;
}

SDL_FRect present_rect(int out_w, int out_h, const std::string& aspect, int fb_w, int fb_h) {
    double target;
    if (aspect == "stretch") return {0, 0, float(out_w), float(out_h)};
    if (aspect == "square") target = double(fb_w) / fb_h;
    else target = 4.0 / 3.0;
    double w = out_w, h = out_w / target;
    if (h > out_h) { h = out_h; w = out_h * target; }
    return {float((out_w - w) / 2), float((out_h - h) / 2), float(w), float(h)};
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) return 2;

    if (opt.headless) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        if (opt.renderer.empty()) opt.renderer = "software";
    }

    SDL_InitFlags flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO;
    if (!opt.headless) flags |= SDL_INIT_GAMEPAD;
    if (!SDL_Init(flags)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    const auto game_dir = scr::find_game_dir(opt.data_dir);
    if (game_dir) SDL_Log("game data: %s", game_dir->c_str());
    else SDL_Log("game data: not found (pass --data or set SCREAMER_DATA)");

    // The title screen as frontend_show_title draws it: menu1l.pcx from gfx\graf.dat.
    std::optional<scr::PcxImage> title;
    if (opt.scene.empty() || opt.scene == "title") {
        if (game_dir) {
            if (auto path = scr::resolve_game_file(*game_dir, "GFX/GRAF.DAT")) {
                if (auto archive = scr::Archive::open(*path)) {
                    if (auto file = archive->read("menu1l.pcx")) title = scr::decode_pcx(*file);
                }
            }
        }
        if (!title && opt.scene == "title") {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "--scene title needs the original game files");
            return 1;
        }
    }

    SDL_WindowFlags wflags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (opt.fullscreen) wflags |= SDL_WINDOW_FULLSCREEN;
    SDL_Window* window = SDL_CreateWindow("Screamer", opt.width, opt.height, wflags);
    if (!window) { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "window: %s", SDL_GetError()); return 1; }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, opt.renderer.empty() ? nullptr : opt.renderer.c_str());
    if (!renderer) { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "renderer: %s", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(renderer, opt.headless ? 0 : 1);
    SDL_Log("video driver: %s, renderer: %s", SDL_GetCurrentVideoDriver(), SDL_GetRendererName(renderer));

    scr::Framebuffer fb;
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, fb.width(), fb.height());
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    uint64_t frame = 0;
    bool running = true;
    scr::SimClock clock;  // the original's frame clock: 100 Hz ticks, at least 2 per frame
    uint64_t last = SDL_GetTicksNS(), tick_acc = 0;
    clock.add_ticks(scr::SimClock::kMinTicksPerFrame);  // draw frame 0 immediately
    int exit_code = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE) running = false;
                else if (ev.key.key == SDLK_F12) {
                    scr::SurfacePtr s(SDL_RenderReadPixels(renderer, nullptr));
                    save_or_log(s.get(), "artifacts/screenshots/frame_" + std::to_string(frame) + ".png");
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad* pad = SDL_OpenGamepad(ev.gdevice.which)) SDL_Log("gamepad connected: %s", SDL_GetGamepadName(pad));
                break;
            default: break;
            }
        }
        if (!running) break;

        // Headless runs advance exactly 2 ticks per loop, so output depends only on the
        // frame count. Interactive runs count real 10 ms ticks, like the timer interrupt.
        bool stepped = false;
        if (opt.headless) {
            if (frame > 0) clock.add_ticks(scr::SimClock::kMinTicksPerFrame);
        } else {
            const uint64_t now = SDL_GetTicksNS();
            tick_acc += now - last;
            last = now;
            clock.add_ticks(static_cast<uint32_t>(tick_acc / scr::SimClock::kTickNs));
            tick_acc %= scr::SimClock::kTickNs;
        }
        if (clock.frame_ready()) {
            clock.begin_frame();
            if (title) scr::blit_pcx(*title, fb);
            else scr::draw_test_pattern(fb, frame);
            stepped = true;
            if (opt.capture_every && frame % opt.capture_every == 0) {
                scr::SurfacePtr s = scr::framebuffer_to_surface(fb);
                char name[64];
                std::snprintf(name, sizeof name, "/fb_%06llu.png", static_cast<unsigned long long>(frame));
                save_or_log(s.get(), opt.capture_dir + name);
            }
            ++frame;
        }

        if (stepped) {
            void* pixels; int pitch;
            if (SDL_LockTexture(tex, nullptr, &pixels, &pitch)) {
                fb.to_rgba(static_cast<uint8_t*>(pixels), pitch);
                SDL_UnlockTexture(tex);
            }
        }

        int ow, oh;
        SDL_GetRenderOutputSize(renderer, &ow, &oh);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        const SDL_FRect dst = present_rect(ow, oh, opt.aspect, fb.width(), fb.height());
        SDL_RenderTexture(renderer, tex, nullptr, &dst);

        const bool last_frame = opt.frames && frame >= opt.frames;
        if (last_frame) {
            if (!opt.screenshot.empty()) {
                scr::SurfacePtr s(SDL_RenderReadPixels(renderer, nullptr));
                if (!s || !save_or_log(s.get(), opt.screenshot)) exit_code = 1;
            }
            if (!opt.dump_fb.empty()) {
                scr::SurfacePtr s = scr::framebuffer_to_surface(fb);
                if (!save_or_log(s.get(), opt.dump_fb)) exit_code = 1;
            }
            running = false;
        }
        SDL_RenderPresent(renderer);
    }

    SDL_Log("frames=%llu", static_cast<unsigned long long>(frame));
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
