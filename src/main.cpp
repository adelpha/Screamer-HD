// Screamer recompilation: SDL3 frontend.
//
// Until the original game logic is reconstructed this shows the title screen from
// the user's game files (--scene title), a track from a fixed camera (--scene track)
// or a deterministic test pattern (--scene testpattern) through the full render path. The command line is designed for
// automated testing: --headless runs without a visible window (SDL "offscreen"
// video driver, software renderer, dummy audio) and --frames/--screenshot/
// --dump-fb produce reproducible images the test harness compares.

#include "core/archive.h"
#include "core/car.h"
#include "core/dtr.h"
#include "core/framebuffer.h"
#include "core/gamedata.h"
#include "core/image.h"
#include "core/pcx.h"
#include "core/render3d.h"
#include "core/track.h"
#include "core/sim_clock.h"
#include "core/testpattern.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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
    std::string scene;              // title | track | testpattern; empty = title when game data is found
    std::string track = "MAP2";     // --scene track: which track
    std::string camera;             // --scene track: x,y,z,yaw,pitch[,roll]
    int fb_width = 320, fb_height = 200;  // internal render size (--render-size)
    bool flat = false;              // --scene track: no textures
    std::vector<std::string> cars;  // --scene track: NAME[,X,Y,Z,YAW] per car
    std::string detail = "H";
    int car_index = 0;              // --scene drive: CARS_DATA record (0..12)
    std::string start;              // --scene drive: X,Z start position
    std::string hold;               // --scene drive: keys held in every frame (accel,brake,left,right)       // --detail: H (high, GFX/MAPnH.DAT) or L (low, MAPnL.DAT)
};

void print_usage() {
    std::printf(
        "screamer %s\n"
        "Usage: screamer [options]\n"
        "  --data DIR           original game directory (default: $SCREAMER_DATA or build-time path)\n"
        "  --headless           no visible window, software renderer, dummy audio\n"
        "  --frames N           exit after N simulation frames (2 ticks of the game's 100 Hz clock each)\n"
        "  --scene NAME         title (default when the game files are found), track, drive or testpattern\n"
        "  --track NAME         track for --scene track / drive (MAP1..MAP6, default MAP2)\n"
        "  --car-index N        --scene drive: car from CARS_DATA (0..12, default 0)\n"
        "  --start X,Z          --scene drive: start position (default: the intro demo's grid)\n"
        "  --hold KEYS          --scene drive: controls held every frame, e.g. accel,left (headless runs)\n"
        "  --detail high|low    track detail: GFX/MAPnH.DAT (default, as the game's high detail) or MAPnL.DAT\n"
        "  --camera X,Y,Z,YAW,PITCH[,ROLL]  camera for --scene track (game units, angles 0..2047)\n"
        "  --render-size WxH    internal resolution for 3D scenes (default 320x200)\n"
        "  --flat               --scene track without textures\n"
        "  --car NAME[,X,Y,Z,YAW[,ANIM]]  add a car (e.g. AUTO00) to --scene track; repeatable; ANIM\n"
        "                       is its animation bits in hex (8000 = second paint scheme); without a\n"
        "                       position it stands 900 units in front of the camera\n"
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
        else if (a == "--track") o.track = next("--track");
        else if (a == "--camera") o.camera = next("--camera");
        else if (a == "--flat") o.flat = true;
        else if (a == "--car") o.cars.push_back(next("--car"));
        else if (a == "--car-index") o.car_index = std::atoi(next("--car-index"));
        else if (a == "--start") o.start = next("--start");
        else if (a == "--hold") o.hold = next("--hold");
        else if (a == "--detail") o.detail = std::string(next("--detail")) == "low" ? "L" : "H";
        else if (a == "--render-size") {
            if (std::sscanf(next("--render-size"), "%dx%d", &o.fb_width, &o.fb_height) != 2 || o.fb_width <= 0 || o.fb_height <= 0) {
                std::fprintf(stderr, "--render-size expects WxH\n");
                return false;
            }
        }
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

    // A track viewed from a fixed camera (the first step of the 3D renderer).
    std::optional<scr::Track> track;
    scr::Camera cam;
    const bool drive = opt.scene == "drive";
    if (opt.scene == "track" || drive) {
        if (game_dir) {
            if (auto path = scr::resolve_game_file(*game_dir, "GFX/" + opt.track + opt.detail + ".DAT")) track = scr::load_track(*path, opt.track);
        }
        if (!track) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "cannot load track %s from the game files", opt.track.c_str());
            return 1;
        }
        SDL_Log("track %s: %zu objects, %zu placements", opt.track.c_str(), track->objects.size(), track->placements.size());
        if (!opt.camera.empty()) {
            std::sscanf(opt.camera.c_str(), "%f,%f,%f,%f,%f,%f", &cam.x, &cam.y, &cam.z, &cam.yaw, &cam.pitch, &cam.roll);
        } else if (!track->placements.empty()) {
            const auto& p = track->placements.front();
            cam.x = float(p.x); cam.y = float(p.y) - 400.0f; cam.z = float(p.z); cam.yaw = float(p.ay);
        }
    }
    struct CarInstance {
        scr::Model model;
        scr::Pose pose;
    };
    std::vector<CarInstance> cars;
    std::optional<scr::Model> shadow;  // SHADOW.3DX, drawn under every car (translucent, MAPn.SMG)
    if (track && !opt.cars.empty()) {
        if (auto path = scr::resolve_game_file(*game_dir, "GFX/" + opt.track + opt.detail + ".DAT")) shadow = scr::load_object(*path, "SHADOW.3DX");
    }
    for (const std::string& spec : opt.cars) {
        if (!track) break;
        const auto comma = spec.find(',');
        const std::string name = spec.substr(0, comma);
        auto path = scr::resolve_game_file(*game_dir, "GFX/" + opt.track + opt.detail + ".DAT");
        auto model = path ? scr::load_object(*path, name + ".3DX") : std::nullopt;
        if (!model) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "cannot load car %s", name.c_str());
            return 1;
        }
        scr::Pose pose;
        unsigned animation = 0;
        if (comma == std::string::npos || std::sscanf(spec.c_str() + comma + 1, "%f,%f,%f,%f,%x", &pose.x, &pose.y, &pose.z, &pose.ay, &animation) < 4) {
            // In front of the camera, facing it, on the camera's ground level minus its height.
            const float a = cam.yaw * 6.28318530718f / 2048.0f;
            pose.x = cam.x + 900.0f * std::sin(a);
            pose.z = cam.z - 900.0f * std::cos(a);
            pose.y = cam.y - 196.0f;
            pose.ay = cam.yaw + 1024.0f - 256.0f;
        }
        pose.animation = int32_t(animation);
        cars.push_back({std::move(*model), pose});
    }
    // --scene drive: the player car with the ported physics and the chase camera.
    struct Player {
        scr::Car car;
        scr::Model model;
        scr::ChaseCamera camera;
    };
    std::optional<Player> player;
    if (drive) {
        auto gen = scr::resolve_game_file(*game_dir, "DATA/GEN.DAT");
        auto ga = gen ? scr::Archive::open(*gen) : std::nullopt;
        auto text = ga ? ga->read("CARS.DTR") : std::nullopt;
        size_t n = 0;
        auto cars_data = text ? scr::DtrFile(std::string(text->begin(), text->end())).pack_section("CARS_DATA", 0x40, 0x40, &n)
                              : std::nullopt;
        if (!cars_data || n == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "cannot read CARS_DATA from DATA/GEN.DAT");
            return 1;
        }
        const size_t idx = size_t(std::clamp(opt.car_index, 0, int(n) - 1));
        const uint8_t* rec = cars_data->data() + idx * 0x40;
        const std::string name(reinterpret_cast<const char*>(rec));
        auto path = scr::resolve_game_file(*game_dir, "GFX/" + opt.track + opt.detail + ".DAT");
        auto model = path ? scr::load_object(*path, name + ".3DX") : std::nullopt;
        if (!model) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "cannot load car %s", name.c_str());
            return 1;
        }
        if (!shadow) shadow = scr::load_object(*path, "SHADOW.3DX");
        player.emplace();
        player->model = std::move(*model);
        int sx = 44000, sz = 4550;  // the intro demo's grid, MAP2
        if (!opt.start.empty()) std::sscanf(opt.start.c_str(), "%d,%d", &sx, &sz);
        player->car.reset(scr::car_setup(rec, player->model), *track, sx, sz);
        SDL_Log("driving %s (CARS_DATA %zu)", name.c_str(), idx);
    }
    SDL_Gamepad* pad = nullptr;
    scr::Renderer renderer3d;
    renderer3d.set_textures(!opt.flat);

    scr::Framebuffer fb = track ? scr::Framebuffer(opt.fb_width, opt.fb_height) : scr::Framebuffer();
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
                if (SDL_Gamepad* p = SDL_OpenGamepad(ev.gdevice.which)) {
                    SDL_Log("gamepad connected: %s", SDL_GetGamepadName(p));
                    if (!pad) pad = p;
                }
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
            if (track) {
                fb.palette() = track->palette;
                renderer3d.set_anim_counter(clock.anim_counter());
                if (player) {
                    scr::CarInput in;
                    const bool* keys = SDL_GetKeyboardState(nullptr);
                    auto held = [&](const char* k) { return opt.hold.find(k) != std::string::npos; };
                    if (keys[SDL_SCANCODE_UP] || held("accel")) in.accel = 1023;
                    if (keys[SDL_SCANCODE_DOWN] || held("brake")) in.brake = 1023;
                    if (keys[SDL_SCANCODE_LEFT] || held("left")) in.steer_left = 1023;
                    if (keys[SDL_SCANCODE_RIGHT] || held("right")) in.steer_right = 1023;
                    if (pad) {
                        const int rt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
                        const int lt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
                        const int lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
                        if (rt > 1000) in.accel = std::max(in.accel, rt * 1023 / 32767);
                        if (lt > 1000) in.brake = std::max(in.brake, lt * 1023 / 32767);
                        if (std::abs(lx) > 4000 && !in.steer_left && !in.steer_right) {
                            in.analog_steer = true;
                            in.steer = lx * 1023 / 32767;
                        }
                    }
                    player->car.update(*track, in, clock.frame_time(), clock.pow_table_row());
                    player->camera.update(player->car, clock.frame_time(), clock.pow_table_row());
                    constexpr float kToUnits = 1024.0f / 3.1415927410125732f;
                    cam.x = float(player->camera.x);
                    cam.y = float(player->camera.y);
                    cam.z = float(player->camera.z);
                    cam.pitch = float(int(player->camera.pitch * kToUnits) & 0x7ff);
                    cam.yaw = float(int(player->camera.yaw * kToUnits) & 0x7ff);
                    cam.roll = float(int(player->camera.roll * kToUnits) & 0x7ff);
                }
                renderer3d.begin(*track, cam, fb);
                renderer3d.draw_placements();
                if (player) {
                    // car_sync_render_object: body at the car, angles in 2048 units; the
                    // shadow sits on the ground just below.
                    const scr::Car& c = player->car;
                    constexpr float kToUnits = 1024.0f / 3.1415927410125732f;
                    scr::Pose pose;
                    pose.x = float(c.pos_x);
                    pose.y = float(c.pos_y + c.body_lift);
                    pose.z = float(c.pos_z);
                    pose.ax = float(int((c.pitch + c.lean_pitch) * kToUnits) & 0x7ff);
                    pose.ay = float(int((c.yaw + c.drift_angle) * kToUnits) & 0x7ff);
                    pose.az = float(int((c.roll + c.lean_roll) * kToUnits) & 0x7ff);
                    pose.animation = int32_t(c.render_animation());
                    scr::Pose sh = pose;
                    sh.y = float(c.ground_y - 2);
                    if (shadow) renderer3d.draw_model(*shadow, sh);
                    renderer3d.draw_model(player->model, pose);
                }
                for (const auto& c : cars) {
                    if (shadow) renderer3d.draw_model(*shadow, c.pose);
                    renderer3d.draw_model(c.model, c.pose);
                }
                renderer3d.finish();
            } else if (title) {
                scr::blit_pcx(*title, fb);
            } else {
                scr::draw_test_pattern(fb, frame);
            }
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
        if (last_frame && player) {
            const scr::Car& c = player->car;
            SDL_Log("car: pos %d,%d,%d yaw %.3f speed %.0f gear %d rpm %.0f wall %d", c.pos_x, c.pos_y, c.pos_z, double(c.yaw),
                    double(c.speed), c.gear, double(c.engine_rpm), int(c.wall_contact));
        }
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
