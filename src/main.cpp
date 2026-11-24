// Screamer recompilation: SDL3 frontend.
//
// Until the original game logic is reconstructed this shows the title screen from
// the user's game files (--scene title), a track from a fixed camera (--scene track)
// or a deterministic test pattern (--scene testpattern) through the full render path. The command line is designed for
// automated testing: --headless runs without a visible window (SDL "offscreen"
// video driver, software renderer, dummy audio) and --frames/--screenshot/
// --dump-fb produce reproducible images the test harness compares.

#include "core/animations.h"
#include "core/archive.h"
#include "core/car.h"
#include "core/cd_audio.h"
#include "core/dtr.h"
#include "core/obstacles.h"
#include "core/opponent.h"
#include "core/fm_synth.h"
#include "core/framebuffer.h"
#include "core/gamedata.h"
#include "core/hud.h"
#include "core/menu.h"
#include "core/image.h"
#include "core/pcx.h"
#include "core/race_audio.h"
#include "core/sound.h"
#include "core/render3d.h"
#include "core/track.h"
#include "core/sim_clock.h"
#include "core/testpattern.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

// Car shadows are tested against the depth buffer as if this much nearer (see Pose::depth_bias).
constexpr float kShadowBias = 64.0f;
// Obstacles (cones, barriers) likewise, so their lower half shows over the ground.
constexpr float kObstacleBias = 512.0f;

struct Options {
    std::string data_dir;
    bool headless = false;
    bool perf = false;              // print the 3D render time per frame at the end
    int threads = 0;                // renderer threads (0: one per hardware thread)
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
    std::string hold;
    int view = 2;                   // --scene drive: starting view 0..3 (F1..F4)
    bool mute = false;              // no audio device
    int sfx_volume = 100, music_volume = 70;  // 0..100
    bool music = true, speech = true;
    std::string music_source = "auto";  // cd | midi | auto (CD audio when the image is there)
    bool audio_smooth = true;       // linear interpolation (false: the driver's nearest sample)
    std::string menu_keys;
    int laps = 3;                   // a race from the menu: laps, opponent level ('A' + level)
    int level = 1;
    bool from_menu = false;         // Esc and the end of the race go back to the menu
    int game_mode = 1;              // 1/2 CPU cars, 4 time attack, 0x20 cone carnage, 0x40 slalom
    bool ghost = false;             // time attack against the saved ghost
    std::string ghost_dir;          // where the time attack ghosts are kept          // --scene menu: scripted keys "frame:key,..." (headless runs)
    std::string record_audio;       // write the mix to a .wav (frame-locked, works headless)
    bool audio_log = false;         // log every sample that starts
};

void print_usage() {
    std::printf(
        "screamer %s\n"
        "Usage: screamer [options]\n"
        "  --data DIR           original game directory (default: $SCREAMER_DATA or build-time path)\n"
        "  --headless           no visible window, software renderer, dummy audio\n"
        "  --perf               print how long the 3D view takes to draw\n"
        "  --threads N          threads drawing the 3D view (default: one per CPU thread)\n"
        "  --frames N           exit after N simulation frames (2 ticks of the game's 100 Hz clock each)\n"
        "  --scene NAME         menu (default when the game files are found), title, track, drive or testpattern\n"
        "  --menu-keys LIST     --scene menu: keys pressed at frames, e.g. 40:down,60:enter (headless runs)\n"
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
        "  --mute               no sound\n"
        "  --sfx-volume N       sound effects volume 0..100 (default 100)\n"
        "  --music-volume N     CD music volume 0..100 (default 70)\n"
        "  --no-music           no CD music (from the CD image: SCREAM.cue/.bin, or SCREAM.DAT/.gog as Steam and GOG ship it)\n"
        "  --no-speech          no announcer\n"
        "  --music-source SRC   cd, midi (the FM synth versions in DATA/MUSIC.DAT) or auto (default: CD if found)\n"
        "  --record-audio FILE  write the sound of the run to a 44.1 kHz .wav (also headless)\n"
        "  --audio-filter MODE  linear (default) or nearest (the original driver's crunchier resampling)\n"
        "  --version, --help\n",
        SCREAMER_VERSION);
}

// 16-bit stereo PCM .wav
bool write_wav(const std::string& path, const std::vector<float>& stereo, int rate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "cannot write %s", path.c_str());
        return false;
    }
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint32_t bytes = uint32_t(stereo.size() * 2);
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(2);
    u32(uint32_t(rate));
    u32(uint32_t(rate) * 4);
    u16(4);
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(bytes);
    for (float v : stereo) {
        const int16_t s = int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);
    return true;
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
        else if (a == "--perf") o.perf = true;
        else if (a == "--threads") o.threads = std::atoi(next("--threads"));
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
        else if (a == "--view") o.view = std::atoi(next("--view"));
        else if (a == "--mute") o.mute = true;
        else if (a == "--sfx-volume") o.sfx_volume = std::clamp(std::atoi(next("--sfx-volume")), 0, 100);
        else if (a == "--music-volume") o.music_volume = std::clamp(std::atoi(next("--music-volume")), 0, 100);
        else if (a == "--no-music") o.music = false;
        else if (a == "--no-speech") o.speech = false;
        else if (a == "--music-source") o.music_source = next("--music-source");
        else if (a == "--record-audio") o.record_audio = next("--record-audio");
        else if (a == "--menu-keys") o.menu_keys = next("--menu-keys");
        else if (a == "--game-mode") o.game_mode = int(std::strtol(next("--game-mode"), nullptr, 0));
        else if (a == "--audio-log") o.audio_log = true;
        else if (a == "--audio-filter") o.audio_smooth = std::string(next("--audio-filter")) != "nearest";
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

// State that outlives one scene: the menu (it keeps its selections) and what it asked for.
struct Shared {
    std::optional<scr::MenuAssets> menu_assets;
    std::optional<scr::Menu> menu;
    std::optional<scr::MenuRace> race;  // the menu chose a race
    struct Outcome {
        float best_lap = 10000, total_time = 0;
        bool finished = false, failed = false;
        int position = 0;
    };
    std::vector<Outcome> outcomes;      // the races since the menu (PLAY AGAIN repeats it)
    std::string choice_path;            // our copy of CONFIG/CHOICE.DAT
    bool back_to_menu = false;          // the race asks to return to the menu
    bool restart_race = false;          // PLAY AGAIN? yes
    uint64_t frames_run = 0;
};

// One scene (--scene ...): loads it and runs its loop until it ends.
int run_scene(Options& opt, SDL_Window* window, SDL_Renderer* renderer, const std::optional<std::string>& game_dir,
              std::optional<scr::PcxImage>& title, Shared& shared) {
    (void)window;
    // A track viewed from a fixed camera (the first step of the 3D renderer).
    std::optional<scr::Track> track;
    scr::Camera cam;
    scr::TrackAnimations anims;           // trucks, jets, trams... on their splines
    scr::Obstacles obstacles;             // cones and the like (MAPnOST.SPL in races)
    std::optional<std::string> gfx_path;  // GFX/MAPn?.DAT
    // Time attack: the ghost of the best lap (ghost_record_frame / ghost_update), kept as
    // <pref dir>/MAPn.ghost: int car, float lap time, then frames of
    // { x, y, z, pitch, yaw, roll, animation, reflection, time }.
    struct GhostFrame {
        int x, y, z;
        float pitch, yaw, roll;
        int32_t animation, reflection;
        float time;
    };
    struct Ghost {
        std::string path;
        int car = 0;
        float lap = 0;              // 0: none
        std::vector<GhostFrame> frames, recording;
        bool load() {
            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) return false;
            int32_t c = 0;
            float l = 0;
            bool ok = std::fread(&c, 4, 1, f) == 1 && std::fread(&l, 4, 1, f) == 1;
            GhostFrame g{};
            frames.clear();
            while (ok && std::fread(&g, sizeof g, 1, f) == 1) frames.push_back(g);
            std::fclose(f);
            car = c;
            lap = ok && !frames.empty() ? l : 0;
            return lap > 0;
        }
        void save(float lap_time) {
            std::FILE* f = std::fopen(path.c_str(), "wb");
            if (!f) return;
            const int32_t c = car;
            std::fwrite(&c, 4, 1, f);
            std::fwrite(&lap_time, 4, 1, f);
            for (const GhostFrame& g : recording) std::fwrite(&g, sizeof g, 1, f);
            std::fclose(f);
        }
    } ghost;
    std::vector<scr::Model> anim_models;  // per animation (an empty model draws nothing)
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
        // Moving scenery (track_load_animations).
        auto data = scr::resolve_game_file(*game_dir, "DATA/" + opt.track + ".DAT");
        auto gfx = scr::resolve_game_file(*game_dir, "GFX/" + opt.track + opt.detail + ".DAT");
        gfx_path = gfx;
        if (data && gfx) obstacles.load(*data, *gfx, opt.track, "OST", *track);
        if (data && gfx && anims.load(*data, opt.track)) {
            for (const scr::TrackAnimation& an : anims.list) {
                std::string obj = an.name;
                for (char& ch : obj) ch = char(std::toupper(static_cast<unsigned char>(ch)));
                auto m = scr::load_object(*gfx, obj + ".3DX");
                anim_models.push_back(m ? std::move(*m) : scr::Model{});
            }
            anims.start(0.02f);  // the first frame's time at 50 fps
            for (const scr::TrackAnimation& an : anims.list)
                SDL_Log("animation %s at %d,%d,%d speed %.0f", an.name.c_str(), an.x, an.y, an.z, double(an.speed));
        }
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
    std::optional<scr::Model> smoke_model;  // SMOKE.3DX, the player car's tyre smoke
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
        scr::Race race;                    // the player car (race.player) and the CPU cars
        bool cpu_cars = false;             // DATA/<track>.DAT had racing lines
        scr::Model model;
        std::vector<scr::Model> cpu_models;  // per CPU car
        scr::ViewCamera camera;            // F1..F4
        scr::EnvScroll env;                // glass reflection scroll of the player car
        std::vector<scr::EnvScroll> cpu_env;
        std::vector<std::array<float, 2>> cpu_shadow;  // shadow pitch and roll of each CPU car
    };
    std::optional<Player> player;
    std::optional<scr::Hud> hud;  // the race HUD (--scene drive with CPU cars)
    // Sound: the mixer always runs (deterministic, testable); the device only in a window.
    scr::SoundBank sound_bank;
    scr::Mixer mixer;
    scr::RaceAudio race_audio;
    scr::CdImage cd;
    int cd_track = 0;  // the CD track playing (0: MIDI or none)
    SDL_AudioStream* audio_stream = nullptr;
    std::vector<float> recorded;  // --record-audio
    mixer.sfx_volume = float(opt.sfx_volume) / 100.0f;
    mixer.music_volume = float(opt.music_volume) / 100.0f;
    mixer.smooth = opt.audio_smooth;
    int radar = 1;                // M cycles 0 (off) .. 3 (DAT_0011d2a0)
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
        smoke_model = scr::load_object(*path, "SMOKE.3DX");
        player.emplace();
        player->model = std::move(*model);
        const scr::CarSetup setup = scr::car_setup(rec, player->model);
        // CPU cars: the racing lines and OPPONENTS of DATA/<track>.DAT; each takes the next
        // CARS_DATA record, skipping the player's (opponent_car_load).
        auto data = scr::resolve_game_file(*game_dir, "DATA/" + opt.track + ".DAT");
        player->cpu_cars = opt.start.empty() && data && player->race.load(*data, opt.track, char('A' + opt.level));
        player->race.laps = opt.laps;
        player->race.mode = opt.game_mode;
        std::vector<scr::CarSetup> cpu_setups;
        const bool solo = opt.game_mode == 4 || opt.game_mode == 0x20 || opt.game_mode == 0x40;
        if (player->cpu_cars && solo) {
            // the special modes: no CPU cars, the first grid place, the mode's obstacles
            player->race.start(*track, setup, {});
            if (opt.game_mode == 0x40) player->race.loads_gates(*data, opt.track);
            if (gfx_path) obstacles.load(*data, *gfx_path, opt.track, opt.game_mode == 0x20 ? "CNG" : opt.game_mode == 0x40 ? "SLA" : "OST", *track);
            if (opt.game_mode == 4) {
                ghost.path = opt.ghost_dir + opt.track + ".ghost";
                ghost.car = int(idx);
                if (opt.ghost) ghost.load();
            }
        } else if (player->cpu_cars) {
            size_t next = 0;
            for (size_t i = 0; i < player->race.defs.size(); ++i) {
                if (next == idx) next = (next + 1) % n;
                const uint8_t* r = cars_data->data() + next * 0x40;
                next = (next + 1) % n;
                auto m = scr::load_object(*path, std::string(reinterpret_cast<const char*>(r)) + ".3DX");
                if (!m) break;
                cpu_setups.push_back(scr::car_setup(r, *m));
                player->cpu_models.push_back(std::move(*m));
            }
            player->race.start(*track, setup, cpu_setups);
        } else {
            int sx = 44000, sz = 4550;  // the intro demo's grid, MAP2
            if (!opt.start.empty()) std::sscanf(opt.start.c_str(), "%d,%d", &sx, &sz);
            player->race.player.reset(setup, *track, sx, sz);
        }
        SDL_Log("driving %s (CARS_DATA %zu), %zu CPU cars", name.c_str(), idx, player->race.opponents.size());
        // Sounds: SOUND.DAT samples with the SFXSBASE, track and announcer tables (sfx_load_tables).
        auto sound_dat = scr::resolve_game_file(*game_dir, "DATA/SOUND.DAT");
        if (gen && sound_dat && sound_bank.load(*gen, *sound_dat, data ? *data : std::string(), opt.track, opt.speech)) {
            mixer.set_bank(&sound_bank);
            race_audio.start(mixer, sound_bank, player->race, int(idx), 0, opt.speech);
        } else {
            SDL_Log("no sound effects (DATA/SOUND.DAT not found)");
        }
        if (opt.music && ((!opt.headless && !opt.mute) || !opt.record_audio.empty())) {
            const int map = opt.track.size() >= 4 ? std::atoi(opt.track.c_str() + 3) - 1 : 0;
            bool playing = false;
            if (opt.music_source != "midi") {
                auto cue = scr::find_cd_cue(*game_dir);
                if (cue && cd.open(*cue)) {
                    auto music = std::make_shared<scr::CdAudioPlayer>(cd, scr::cd_track_for_race(map));
                    if (music->ok()) {
                        mixer.set_music(music);
                        playing = true;
                        cd_track = scr::cd_track_for_race(map);
                    }
                } else {
                    SDL_Log("no CD music (no CD image: SCREAM.cue/.bin or SCREAM.DAT/.gog)");
                }
            }
            if (!playing && opt.music_source != "cd") {
                // The game's MIDI music on the FM synth, as it plays without the CD.
                auto music_dat = scr::resolve_game_file(*game_dir, "DATA/MUSIC.DAT");
                auto song = music_dat ? scr::load_hmp_music(*music_dat, scr::music_for_race(map)) : nullptr;
                if (song) mixer.set_music(song);
                else SDL_Log("no MIDI music (DATA/MUSIC.DAT)");
            }
        }
        if (!opt.headless && !opt.mute) {
            const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 44100};
            audio_stream = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                [](void* user, SDL_AudioStream* stream, int additional, int) {
                    auto* m = static_cast<scr::Mixer*>(user);
                    static std::vector<float> buf;
                    const int frames = additional / int(sizeof(float) * 2);
                    if (frames <= 0) return;
                    buf.resize(size_t(frames) * 2);
                    m->mix(buf.data(), frames, 44100);
                    SDL_PutAudioStreamData(stream, buf.data(), frames * int(sizeof(float) * 2));
                },
                &mixer);
            if (audio_stream) SDL_ResumeAudioStreamDevice(audio_stream);
            else SDL_Log("no audio device: %s", SDL_GetError());
        }
        player->camera.set(opt.view);
        if (player->cpu_cars) {
            auto game = scr::resolve_game_file(*game_dir, "GFX/GAME.DAT");
            hud.emplace();
            if (!game || !hud->load(*game)) hud.reset();
            if (auto local = scr::resolve_game_file(*game_dir, "DATA/LOCAL.DAT"); hud && local) hud->load_texts(*local);
            // The car's dashboard: COCK<nn><A|B>L.PKC and its layout COCK<nn>L in CARS.DTR.
            if (hud && name.size() >= 6) {
                const std::string nn = name.substr(4, 2);
                hud->load_cockpit(*game, nn + (setup.animation ? "B" : "A"),
                                  std::string(text->begin(), text->end()), "COCK" + nn + "L");
            }
        }
    }
    SDL_Gamepad* pad = nullptr;
    scr::Renderer renderer3d;
    renderer3d.set_textures(!opt.flat);
    renderer3d.set_threads(opt.threads);

    // --scene menu: the front end (kept by main across races)
    scr::Menu* menu = nullptr;
    scr::SoundBank menu_sounds;
    if (opt.scene == "menu") {
        if (!shared.menu_assets) {
            auto md = scr::resolve_game_file(*game_dir, "GFX/MENU.DAT");
            auto gd = scr::resolve_game_file(*game_dir, "GFX/GRAF.DAT");
            auto ld = scr::resolve_game_file(*game_dir, "DATA/LOCAL.DAT");
            shared.menu_assets.emplace();
            if (!md || !gd || !ld || !shared.menu_assets->load(*md, *gd, *ld)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "cannot load the menu from GFX/MENU.DAT and GFX/GRAF.DAT");
                return 1;
            }
            shared.menu.emplace(*shared.menu_assets);
            // The saved menu state: ours, else the game's CONFIG/CHOICE.DAT, else the defaults.
            scr::ChoiceFile choice;
            if (!choice.load(shared.choice_path))
                if (auto orig = scr::resolve_game_file(*game_dir, "CONFIG/CHOICE.DAT")) choice.load(*orig);
            shared.menu->load_choice(choice);
        }
        menu = &*shared.menu;
        menu->quit = false;
        if (!shared.outcomes.empty() && shared.race) {
            for (const auto& o : shared.outcomes)
                menu->race_result(shared.race->map, o.best_lap, o.total_time, shared.race->laps, o.finished);
            const auto& o = shared.outcomes.back();
            menu->after_race(o.finished, o.failed, o.position, o.total_time);
        } else if (shared.race && shared.race->game_mode == 2) {
            // a championship race left with Esc: it counts as lost
            menu->after_race(false, true, 10, 0);
        } else {
            menu->show_title();
        }
        shared.outcomes.clear();
        shared.race.reset();
        if (!shared.choice_path.empty()) menu->save_choice().save(shared.choice_path);
        // frontend_load: MENU.DTR's SOUND_FX (menu1..menu9)
        auto gen = scr::resolve_game_file(*game_dir, "DATA/GEN.DAT");
        auto snd = scr::resolve_game_file(*game_dir, "DATA/SOUND.DAT");
        if (gen && snd) {
            if (auto ga = scr::Archive::open(*gen)) {
                if (auto dtr = ga->read("MENU.DTR")) {
                    menu_sounds.fx = scr::parse_sound_fx(std::string(dtr->begin(), dtr->end()));
                    if (auto sa = scr::Archive::open(*snd))
                        for (auto& f : menu_sounds.fx)
                            if (auto d = sa->read(f.name + ".RAW")) f.data = std::move(*d);
                    menu_sounds.num_base = int(menu_sounds.fx.size());
                    mixer.set_bank(&menu_sounds);
                }
            }
        }
        if (opt.music && ((!opt.headless && !opt.mute) || !opt.record_audio.empty())) {
            // the menu tune: CD track 4 or MUSIC3
            bool playing = false;
            if (opt.music_source != "midi") {
                if (auto cue = scr::find_cd_cue(*game_dir); cue && cd.open(*cue)) {
                    auto music = std::make_shared<scr::CdAudioPlayer>(cd, scr::kCdTrackMenu);
                    if (music->ok()) {
                        mixer.set_music(music);
                        playing = true;
                    }
                }
            }
            if (!playing && opt.music_source != "cd")
                if (auto md2 = scr::resolve_game_file(*game_dir, "DATA/MUSIC.DAT"))
                    if (auto song = scr::load_hmp_music(*md2, scr::kMusicMenu)) mixer.set_music(song);
        }
        if (!opt.headless && !opt.mute) {
            const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 44100};
            audio_stream = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                [](void* user, SDL_AudioStream* stream, int additional, int) {
                    auto* m = static_cast<scr::Mixer*>(user);
                    static std::vector<float> buf;
                    const int frames = additional / int(sizeof(float) * 2);
                    if (frames <= 0) return;
                    buf.resize(size_t(frames) * 2);
                    m->mix(buf.data(), frames, 44100);
                    SDL_PutAudioStreamData(stream, buf.data(), frames * int(sizeof(float) * 2));
                },
                &mixer);
            if (audio_stream) SDL_ResumeAudioStreamDevice(audio_stream);
        }
    }
    scr::Framebuffer fb = (track || menu) ? scr::Framebuffer(opt.fb_width, opt.fb_height) : scr::Framebuffer();
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, fb.width(), fb.height());
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    uint64_t frame = 0;
    float end_timer = 0;
    bool screenshot_pending = false;      // F12
    double perf_total = 0, perf_max = 0;  // --perf
    int perf_frames = 0;
    // race_run's "PLAY AGAIN?" after a race from the menu (pause_menu_draw_scene): -1 not shown,
    // else the highlighted answer (0 yes, 1 no)
    int prompt = -1;
    bool prompt_keys[4]{};
    // P: the car-stereo panel (pause_menu); the race stands still and goes quiet meanwhile
    bool paused = false;
    scr::CdPanel panel;
    bool panel_keys[3]{};
    auto apply_volumes = [&]() {
        mixer.sfx_volume = float(panel.volume[1]) / float(0x6000);
        mixer.music_volume = float(cd_track ? panel.volume[0] : panel.volume[2]) / float(0x6000) * 0.7f;
    };
    panel.volume[0] = panel.volume[2] = int(float(opt.music_volume) / 100.0f * float(0x6000));
    panel.volume[1] = int(float(opt.sfx_volume) / 100.0f * float(0x6000));
    scr::Framebuffer frozen;
    auto start_prompt = [&]() {
        prompt = 0;
        frozen = fb;
        // three passes of draw_darkened_rect over the whole screen
        if (track) {
            scr::HudSurface hsurf(frozen);
            for (int i = 0; i < 3; ++i) hsurf.darken(0, 0, 320, 200, track->darken075);
        }
    };
    int listener_x = player ? player->race.player.pos_x : 0, listener_z = player ? player->race.player.pos_z : 0;
    if (opt.audio_log)
        mixer.on_play = [&](int s) {
            SDL_Log("frame %llu: sound %d %s", static_cast<unsigned long long>(frame), s,
                    size_t(s) < sound_bank.fx.size() ? sound_bank.fx[size_t(s)].name.c_str() : "?");
        };
    bool running = true;
    scr::SimClock clock;  // the original's frame clock: 100 Hz ticks, at least 2 per frame
    // F8: photo mode - the race stands still and goes quiet, the HUD goes away and a free
    // camera flies about (WASD, Q/E or Space/C up and down, mouse or arrow keys to look,
    // Shift faster, Ctrl slower). F8 or Esc puts everything back.
    bool freecam = false;
    float fc_x = 0, fc_y = 0, fc_z = 0, fc_yaw = 0, fc_pitch = 0;  // angles in 2048 units
    float fc_saved_sfx = 0, fc_saved_music = 0;
    int32_t fc_anim = 0;
    auto set_freecam = [&](bool on) {
        if (on == freecam) return;
        freecam = on;
        if (on) {
            fc_x = cam.x;
            fc_y = cam.y;
            fc_z = cam.z;
            fc_yaw = cam.yaw;
            fc_pitch = cam.pitch > 1024 ? cam.pitch - 2048 : cam.pitch;
            fc_anim = clock.anim_counter();
            fc_saved_sfx = mixer.sfx_volume;
            fc_saved_music = mixer.music_volume;
            mixer.sfx_volume = 0;
            mixer.music_volume = 0;
        } else {
            mixer.sfx_volume = fc_saved_sfx;
            mixer.music_volume = fc_saved_music;
        }
        if (window && !opt.headless) SDL_SetWindowRelativeMouseMode(window, on);
        float ignore_x, ignore_y;
        SDL_GetRelativeMouseState(&ignore_x, &ignore_y);  // movement from before
    };
    uint64_t last = SDL_GetTicksNS(), tick_acc = 0;
    clock.add_ticks(scr::SimClock::kMinTicksPerFrame);  // draw frame 0 immediately
    int exit_code = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                if (track && !menu && !paused && prompt < 0 && !ev.key.repeat &&
                    (ev.key.key == SDLK_F8 || (freecam && ev.key.key == SDLK_ESCAPE))) {
                    set_freecam(!freecam);
                    break;
                }
                if (freecam && ev.key.key != SDLK_F12) break;  // the race keys wait
                if (ev.key.key == SDLK_ESCAPE && !menu) {
                    if (paused) {
                        paused = false;  // Esc leaves the panel
                        apply_volumes();
                    } else if (opt.from_menu && player && opt.scene == "drive") {
                        // Esc ends the race (g_race_flags 0x220); in the question it means no
                        if (prompt < 0) {
                            start_prompt();
                        } else {
                            shared.back_to_menu = true;
                            running = false;
                        }
                    } else {
                        running = false;
                    }
                }
                else if (player && ev.key.key >= SDLK_F1 && ev.key.key <= SDLK_F4 && player->race.started)
                    player->camera.select(int(ev.key.key - SDLK_F1));
                else if (ev.key.key == SDLK_M && !ev.key.repeat) radar = radar >= 3 ? 0 : radar + 1;
                else if (ev.key.key == SDLK_P && !ev.key.repeat && player && hud && prompt < 0 && !paused) {
                    paused = true;
                    frozen = fb;
                    if (track) {
                        scr::HudSurface hsurf(frozen);
                        hsurf.darken(0, 0, 320, 200, track->darken075);  // draw_darkened_rect
                    }
                    panel.button = 0;
                    panel.track = cd_track ? cd_track : panel.track;
                    if (cd.tracks.size() > 1) {
                        panel.first_track = cd.tracks[1].number;
                        panel.last_track = cd.tracks.back().number;
                    }
                    mixer.sfx_volume = 0;
                    mixer.music_volume = 0;
                }
                else if (ev.key.key == SDLK_F12 && !ev.key.repeat) {
                    screenshot_pending = true;  // saved once the next frame is drawn
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
            if (player && hud && !paused && frame == 150 && opt.hold.find("pause") != std::string::npos) {
                // --hold pause: open the P panel at frame 150 (headless checks)
                paused = true;
                frozen = fb;
                scr::HudSurface hsurf(frozen);
                hsurf.darken(0, 0, 320, 200, track->darken075);
            }
            if (prompt >= 0) {
                fb = frozen;
                const bool* keys = SDL_GetKeyboardState(nullptr);
                const bool now[4] = {keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_LEFT],
                                     keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_RIGHT],
                                     keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER], false};
                if (now[0] && !prompt_keys[0] && prompt != 0) {
                    prompt = 0;
                    mixer.play(6);
                }
                if (now[1] && !prompt_keys[1] && prompt != 1) {
                    prompt = 1;
                    mixer.play(6);
                }
                if (now[2] && !prompt_keys[2]) {
                    if (prompt == 0) shared.restart_race = true;  // the same race again
                    else shared.back_to_menu = true;
                    running = false;
                }
                std::copy(now, now + 4, prompt_keys);
                if (hud && hud->texts.size() > 21) {
                    scr::HudSurface hsurf(fb);
                    auto centre = [&](const std::string& t, int y, int m) {
                        hsurf.text(hud->fonts, (320 - hsurf.measure(hud->fonts, t, 0)) >> 1, y, t, 0, m);
                    };
                    const bool blink = (frame & 8) != 0;
                    centre(hud->texts[19], 0x34, 2);
                    centre(hud->texts[20], 0x5c, prompt == 0 && blink ? 2 : 0xe);
                    centre(hud->texts[21], 0x7a, prompt == 1 && blink ? 2 : 0xe);
                }
            } else if (paused) {
                fb = frozen;
                if (opt.hold.find("pause") != std::string::npos) panel.button = 4;  // headless look at the volume bar
                const bool* keys = SDL_GetKeyboardState(nullptr);
                const bool now[3] = {bool(keys[SDL_SCANCODE_LEFT]), bool(keys[SDL_SCANCODE_RIGHT]),
                                     keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER]};
                if (now[0] && !panel_keys[0] && panel.button > 0) --panel.button;
                if (now[1] && !panel_keys[1] && panel.button < 6) ++panel.button;
                panel.pressed = now[2];
                if (now[2]) {
                    const float dt = clock.frame_time();
                    int& v = panel.volume[panel.mode];
                    if (panel.button == 4) v = std::max(0, int(float(v) - dt * 48000.0f));
                    if (panel.button == 5) v = std::min(0x7fff, int(dt * 48000.0f + float(v)));
                }
                if (now[2] && !panel_keys[2]) {
                    if (panel.button == 0) {
                        paused = false;  // play: back to the race
                        apply_volumes();
                    } else if ((panel.button == 2 || panel.button == 3) && cd_track) {
                        // previous / next CD track
                        const int t = std::clamp(cd_track + (panel.button == 3 ? 1 : -1), panel.first_track, panel.last_track);
                        if (t != cd_track) {
                            auto music = std::make_shared<scr::CdAudioPlayer>(cd, t);
                            if (music->ok()) {
                                mixer.set_music(music);
                                cd_track = t;
                                panel.track = t;
                            }
                        }
                    } else if (panel.button == 6) {
                        panel.mode = (panel.mode + 1) % 3;
                    }
                }
                std::copy(now, now + 3, panel_keys);
                panel.blink = (frame & 0x10) != 0;
                if (paused) hud->draw_cd_panel(fb, panel);
            } else if (track) {
                fb.palette() = track->palette;
                renderer3d.set_anim_counter(freecam ? fc_anim : clock.anim_counter());
                if (!freecam && player && frame == 150 && opt.hold.find("freecam") != std::string::npos)
                    set_freecam(true);  // --hold freecam: photo mode at frame 150, flying forward (headless checks)
                if (freecam) {
                    // photo mode: move the free camera, nothing else moves
                    const bool* keys = SDL_GetKeyboardState(nullptr);
                    const float dt = clock.frame_time();
                    float mx = 0, my = 0;
                    SDL_GetRelativeMouseState(&mx, &my);
                    if (opt.headless) mx = my = 0;
                    const float turn = 600.0f * dt;  // 2048 units per turn
                    fc_yaw -= mx * 1.5f;
                    fc_pitch -= my * 1.5f;
                    if (keys[SDL_SCANCODE_LEFT]) fc_yaw += turn;
                    if (keys[SDL_SCANCODE_RIGHT]) fc_yaw -= turn;
                    if (keys[SDL_SCANCODE_UP]) fc_pitch += turn;
                    if (keys[SDL_SCANCODE_DOWN]) fc_pitch -= turn;
                    fc_pitch = std::clamp(fc_pitch, -500.0f, 500.0f);
                    fc_yaw = std::fmod(fc_yaw + 2048.0f, 2048.0f);
                    float speed = 4000.0f * dt;
                    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) speed *= 5.0f;
                    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) speed *= 0.2f;
                    const float a = fc_yaw * 3.1415927f / 1024.0f, p = fc_pitch * 3.1415927f / 1024.0f;
                    // camera space looks down -z, +x is screen left (Renderer::begin)
                    const float fx = std::cos(p) * std::sin(a), fy = std::sin(p), fz = -std::cos(a) * std::cos(p);
                    const float rx = -std::cos(a), rz = -std::sin(a);
                    const float f = float(keys[SDL_SCANCODE_W] || opt.hold.find("freecam") != std::string::npos) -
                                    float(keys[SDL_SCANCODE_S]);
                    const float r = float(keys[SDL_SCANCODE_D]) - float(keys[SDL_SCANCODE_A]);
                    const float u = float(keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_SPACE]) -
                                    float(keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_C]);
                    fc_x += (fx * f + rx * r) * speed;
                    fc_y += (fy * f + u) * speed;
                    fc_z += (fz * f + rz * r) * speed;
                    cam.x = fc_x;
                    cam.y = fc_y;
                    cam.z = fc_z;
                    cam.yaw = fc_yaw;
                    cam.pitch = fc_pitch < 0 ? fc_pitch + 2048.0f : fc_pitch;
                    cam.roll = 0;
                } else if (player) {
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
                        if (lt > 1000 && !in.brake) {
                            in.brake = lt * 1023 / 32767;
                            in.analog_brake = true;
                        }
                        if (std::abs(lx) > 4000 && !in.steer_left && !in.steer_right) {
                            in.analog_steer = true;
                            in.steer = lx * 1023 / 32767;
                        }
                    }
                    if (player->cpu_cars)
                        player->race.update(*track, in, clock.frame_time(), clock.pow_table_row());
                    else
                        player->race.player.update(*track, in, clock.frame_time(), clock.pow_table_row());
                    race_audio.update(player->race, clock.frame_time());
                    if (player->cpu_cars && player->race.started && !player->race.finished && !player->race.failed) {
                        // car_check_obstacle_hits: in every race the player knocks cones and
                        // barriers flying (car_collide_vs_cpu in races, car_collide_obstacles in
                        // the special modes); Cone Carnage gives a second for each
                        scr::Race& rr = player->race;
                        const int hits = obstacles.collide(rr.player);
                        for (int i = 0; i < hits; ++i) mixer.play(5);
                        if (rr.mode == 0x20) rr.time_left += float(hits);
                    }
                    if (player->race.mode == 4 || player->race.mode == 0x20 || player->race.mode == 0x40) {
                        scr::Race& rr = player->race;
                        if (rr.gate_sound) mixer.play(9);
                        if (rr.mode == 4 && rr.timing_started && !rr.finished) {
                            const scr::Car& pc = rr.player;
                            ghost.recording.push_back({pc.pos_x, pc.pos_y + pc.body_lift, pc.pos_z, pc.pitch + pc.lean_pitch,
                                                       pc.yaw + pc.drift_angle, pc.roll + pc.lean_roll,
                                                       int32_t(pc.render_animation()), pc.surface_render_flag, rr.lap_time});
                        }
                        if (rr.finished && !rr.failed && rr.mode == 4 && !ghost.recording.empty() &&
                            (ghost.lap <= 0 || rr.best_lap < ghost.lap)) {
                            // race_run's attack_save: the new best lap becomes the ghost
                            ghost.save(rr.best_lap);
                            ghost.recording.clear();
                        }
                    }
                    // animations_update, heard from where the player was last frame.
                    anims.update(listener_x, listener_z, clock.frame_time());
                    race_audio.animations(anims);
                    listener_x = player->race.player.pos_x;
                    listener_z = player->race.player.pos_z;
                    player->camera.update(player->race.player, player->race.camera_hi, clock.frame_time(),
                                          clock.pow_table_row());
                    // engine_set_viewport(320, h, h * 0.65): the projection centre follows the view size.
                    cam.center_y = float(int(float(player->camera.viewport) * 0.65f));
                    constexpr float kToUnits = 1024.0f / 3.1415927410125732f;
                    cam.x = float(player->camera.x);
                    cam.y = float(player->camera.y);
                    cam.z = float(player->camera.z);
                    cam.pitch = float(int(player->camera.pitch * kToUnits) & 0x7ff);
                    cam.yaw = float(int(player->camera.yaw * kToUnits) & 0x7ff);
                    cam.roll = float(int(player->camera.roll * kToUnits) & 0x7ff);
                }
                const auto perf_t0 = std::chrono::steady_clock::now();
                renderer3d.begin(*track, cam, fb);
                renderer3d.draw_placements();
                // obstacles_update and the obstacles
                if (!freecam) obstacles.update(*track, clock.frame_time());
                for (const scr::Obstacle& o : obstacles.list) {
                    if (o.y == -20000 || size_t(o.model) >= obstacles.models.size()) continue;
                    constexpr float kObsUnits = 1024.0f / 3.1415927410125732f;
                    scr::Pose op;
                    op.x = float(o.x);
                    op.y = float(o.y);
                    // half of each obstacle is below the ground (it stands at the ground height);
                    // the original draws it over the ground all the same (painter's order)
                    op.depth_bias = kObstacleBias;
                    op.z = float(o.z);
                    op.ax = float(int(o.ax * kObsUnits) & 0x7ff);
                    op.ay = float(int(o.ay * kObsUnits) & 0x7ff);
                    op.az = float(int(o.az * kObsUnits) & 0x7ff);
                    renderer3d.draw_model(obstacles.models[size_t(o.model)], op);
                }
                // The moving scenery (instance angles: yaw only).
                if (!player && !freecam) anims.update(int(cam.x), int(cam.z), clock.frame_time());
                constexpr float kAnimToUnits = 1024.0f / 3.1415927410125732f;
                for (size_t i = 0; i < anims.list.size() && i < anim_models.size(); ++i) {
                    const scr::TrackAnimation& an = anims.list[i];
                    scr::Pose ap;
                    ap.x = float(an.x);
                    ap.y = float(an.y);
                    ap.z = float(an.z);
                    ap.ay = float(int(an.yaw * kAnimToUnits) & 0x7ff);
                    renderer3d.draw_model(anim_models[i], ap);
                }
                if (player) {
                    // car_sync_render_object: body at the car, angles in 2048 units; the
                    // shadow sits on the ground just below.
                    const scr::Car& c = player->race.player;
                    constexpr float kToUnits = 1024.0f / 3.1415927410125732f;
                    scr::Pose pose;
                    pose.x = float(c.pos_x);
                    pose.y = float(c.pos_y + c.body_lift);
                    pose.z = float(c.pos_z);
                    pose.ax = float(int((c.pitch + c.lean_pitch) * kToUnits) & 0x7ff);
                    pose.ay = float(int((c.yaw + c.drift_angle) * kToUnits) & 0x7ff);
                    pose.az = float(int((c.roll + c.lean_roll) * kToUnits) & 0x7ff);
                    pose.animation = int32_t(c.render_animation());
                    pose.reflection = c.surface_render_flag;
                    // car_apply_surface sets the (global) scroll rate from the player's surface.
                    const float env_rate = c.marker_flags == 0 ? 0.002f : 0.03f;
                    pose.env = &player->env;
                    pose.env_rate = env_rate;
                    // car_update_shadow_position: 20 units further from the camera, on the ground,
                    // tilted with the body.
                    scr::Pose sh = pose;
                    {
                        const double dx = double(cam.x) - c.pos_x, dz = double(cam.z) - c.pos_z;
                        const double len = std::sqrt(dx * dx + dz * dz);
                        if (len > 0) {
                            sh.x = float(std::lrint(double(c.pos_x) - dx / len * 20.0));
                            sh.z = float(std::lrint(double(c.pos_z) - dz / len * 20.0));
                        }
                    }
                    sh.y = float(c.ground_y - 2);
                    sh.env = nullptr;
                    sh.depth_bias = kShadowBias;
                    // the tyre smoke (car_sync_debris_objects: instance position and animation word)
                    if (smoke_model)
                        for (const scr::SmokePuff& sp : c.smoke) {
                            if (!sp.on) continue;
                            scr::Pose spose;
                            spose.x = float(sp.x);
                            spose.y = float(sp.y);
                            spose.z = float(sp.z);
                            spose.animation = sp.animation;
                            spose.depth_bias = kShadowBias;  // on the road, not in it
                            renderer3d.draw_model(*smoke_model, spose);
                        }
                    if (player->camera.car_visible || freecam) {
                        if (shadow) renderer3d.draw_model(*shadow, sh);
                        renderer3d.draw_model(player->model, pose);
                    }
                    // ghost_sync_render_object: where the ghost was at this lap time
                    if (player->race.mode == 4 && ghost.lap > 0 && !ghost.frames.empty() && player->race.timing_started) {
                        const float t = player->race.lap_time;
                        size_t gi = 0;
                        while (gi + 1 < ghost.frames.size() && ghost.frames[gi + 1].time <= t) ++gi;
                        const GhostFrame& g = ghost.frames[gi];
                        scr::Pose gp;
                        gp.x = float(g.x);
                        gp.y = float(g.y);
                        gp.z = float(g.z);
                        gp.ax = float(int(g.pitch * kToUnits) & 0x7ff);
                        gp.ay = float(int(g.yaw * kToUnits) & 0x7ff);
                        gp.az = float(int(g.roll * kToUnits) & 0x7ff);
                        gp.animation = g.animation;
                        gp.reflection = g.reflection;
                        renderer3d.draw_model(player->model, gp);
                    }
                    // opponent_sync_render_object
                    for (size_t i = 0; i < player->race.opponents.size(); ++i) {
                        const scr::Opponent& o = player->race.opponents[i];
                        scr::Pose op;
                        op.x = float(o.pos_x);
                        op.y = float(o.pos_y);
                        op.z = float(o.pos_z);
                        op.ax = float(int(o.pitch * kToUnits) & 0x7ff);
                        op.ay = float(int(o.draw_yaw * kToUnits) & 0x7ff);
                        op.az = float(int(o.roll * kToUnits) & 0x7ff);
                        op.animation = int32_t(uint32_t(o.setup.animation) | (o.catching_up ? 0u : 0x4000u) |
                                               (o.speed <= 4000.0f ? 0x2000u : 0u));
                        op.reflection = int32_t(o.render_flags);
                        if (player->cpu_env.size() < player->race.opponents.size())
                            player->cpu_env.resize(player->race.opponents.size());
                        op.env = &player->cpu_env[i];
                        op.env_rate = env_rate;
                        // the shadow on the ground, tilted with the car (kept as it was while it crashes)
                        if (player->cpu_shadow.size() < player->race.opponents.size())
                            player->cpu_shadow.resize(player->race.opponents.size());
                        if (o.crash_state == 0) player->cpu_shadow[i] = {op.ax, op.az};
                        scr::Pose osh = op;
                        osh.y = float(o.ground_y - 2);
                        osh.ax = player->cpu_shadow[i][0];
                        osh.az = player->cpu_shadow[i][1];
                        osh.env = nullptr;
                        osh.depth_bias = kShadowBias;
                        if (shadow) renderer3d.draw_model(*shadow, osh);
                        renderer3d.draw_model(player->cpu_models[i], op);
                    }
                }
                for (const auto& c : cars) {
                    if (shadow) renderer3d.draw_model(*shadow, c.pose);
                    renderer3d.draw_model(c.model, c.pose);
                }
                renderer3d.finish();
                {
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - perf_t0).count();
                    perf_total += ms;
                    perf_max = std::max(perf_max, ms);
                    ++perf_frames;
                }
                if (player && player->cpu_cars && hud && !freecam) {
                    const scr::Race& r = player->race;
                    const scr::Car& c = r.player;
                    scr::HudState hs;
                    hs.lap = c.lap;
                    hs.laps = r.laps;
                    hs.position = r.player_position;
                    hs.time_left = r.time_left;
                    hs.total_time = r.total_time;
                    hs.lap_time = r.lap_time;
                    hs.best_lap = r.best_lap_text;
                    hs.speed = c.speed;
                    hs.maxspd = c.setup.maxspd;
                    hs.rpm = c.engine_rpm;
                    hs.maxrpm = c.setup.maxrpm;
                    hs.gear = c.gear;
                    hs.countdown_stage = r.countdown_stage;
                    hs.end = r.finished ? 1 : r.failed ? 2 : 0;
                    if (hs.end && opt.from_menu) {
                        // race_end_screen: back to the menu five seconds after the end
                        end_timer += clock.frame_time();
                        if (5.0f <= end_timer && race_audio.speech_current() == -1 && prompt < 0) {
                            shared.outcomes.push_back(
                                Shared::Outcome{r.best_lap, r.total_time, r.finished, r.failed, r.player_position});
                            if (shared.race && shared.race->game_mode == 2) {
                                // the championship goes on in the menu
                                shared.back_to_menu = true;
                                running = false;
                            } else {
                                start_prompt();
                            }
                        }
                    }
                    hs.won = r.finished && r.player_position == 1;
                    hs.player_x = c.pos_x;
                    hs.player_z = c.pos_z;
                    hs.player_yaw = c.yaw;
                    for (const scr::Opponent& o : r.opponents) hs.cpu_cars.push_back({o.pos_x, o.pos_z});
                    // draw_map_objects: the cross-sections of the 17 x 17 cells around the player.
                    for (const scr::TrackSection& ts : track->sections)
                        if (std::abs(ts.x - c.pos_x) < 8 * 8192 + 4096 && std::abs(ts.z - c.pos_z) < 8 * 8192 + 4096)
                            hs.radar_lines.push_back({ts.left_x, ts.left_z, ts.right_x, ts.right_z});
                    hud->draw(fb, hs, track->darken075, clock.frame_time(), clock.pow_table_row(), player->camera.hud_view,
                              radar);
                }
            } else if (menu) {
                scr::MenuKeys mk;
                const bool* keys = SDL_GetKeyboardState(nullptr);
                mk.esc = keys[SDL_SCANCODE_ESCAPE];
                mk.enter = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER];
                mk.up = keys[SDL_SCANCODE_UP];
                mk.down = keys[SDL_SCANCODE_DOWN];
                mk.left = keys[SDL_SCANCODE_LEFT];
                mk.right = keys[SDL_SCANCODE_RIGHT];
                // scripted keys: "frame:key" held for that one frame
                for (size_t pos = 0; pos < opt.menu_keys.size();) {
                    size_t end = opt.menu_keys.find(',', pos);
                    if (end == std::string::npos) end = opt.menu_keys.size();
                    const std::string item = opt.menu_keys.substr(pos, end - pos);
                    pos = end + 1;
                    const size_t colon = item.find(':');
                    if (colon == std::string::npos || std::strtoull(item.c_str(), nullptr, 10) != frame) continue;
                    const std::string k = item.substr(colon + 1);
                    mk.esc |= k == "esc";
                    mk.enter |= k == "enter";
                    mk.up |= k == "up";
                    mk.down |= k == "down";
                    mk.left |= k == "left";
                    mk.right |= k == "right";
                }
                menu->update(mk, clock.frame_time());
                for (int sfx : menu->sounds) mixer.play(sfx);
                menu->draw(fb, renderer3d);
                if (menu->quit) {
                    running = false;
                    if (!shared.choice_path.empty()) menu->save_choice().save(shared.choice_path);
                }
                if (menu->race) {
                    SDL_Log("menu: race on MAP%d%s, car %d, %d laps, level %d, driver %s", menu->race->map,
                            menu->race->mirrored ? " (reversed)" : "", menu->race->car, menu->race->laps,
                            menu->race->level, menu->race->name.c_str());
                    shared.race = *menu->race;
                    menu->race.reset();
                    running = false;
                    if (!shared.choice_path.empty()) menu->save_choice().save(shared.choice_path);
                    // frontend_start_race's loading screen
                    menu->draw_loading(fb, shared.race->map);
                }
            } else if (title) {
                scr::blit_pcx(*title, fb);
            } else {
                scr::draw_test_pattern(fb, frame);
            }
            stepped = true;
            if (!opt.record_audio.empty()) {
                // One frame of sound per simulated frame (2 ticks = 1/50 s at 44.1 kHz).
                const int n = int(44100 * 2 / 100);
                const size_t at = recorded.size();
                recorded.resize(at + size_t(n) * 2);
                mixer.mix(recorded.data() + at, n, 44100);
            }
            if (opt.capture_every && frame % opt.capture_every == 0) {
                scr::SurfacePtr s = scr::framebuffer_to_surface(fb);
                char name[64];
                std::snprintf(name, sizeof name, "/fb_%06llu.png", static_cast<unsigned long long>(frame));
                save_or_log(s.get(), opt.capture_dir + name);
            }
            ++frame;
        }

        if (screenshot_pending && frame > 0) {
            // F12: the frame as drawn (render size, before scaling to the window), as a PNG in
            // Pictures/Screamer (or next to the program when there is no Pictures folder).
            screenshot_pending = false;
            std::string dir;
            if (const char* pics = SDL_GetUserFolder(SDL_FOLDER_PICTURES)) dir = std::string(pics) + "Screamer/";
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &now);
            char name[64];
            std::strftime(name, sizeof name, "screamer_%Y%m%d_%H%M%S", &tm);
            std::string path = dir + name + ".png";
            for (int i = 2; std::filesystem::exists(path); ++i) path = dir + name + "_" + std::to_string(i) + ".png";
            scr::SurfacePtr shot = scr::framebuffer_to_surface(fb);
            if (save_or_log(shot.get(), path)) {
                // where it was taken, to look at the same view again (--scene track --camera)
                if (std::FILE* f = std::fopen((path.substr(0, path.size() - 4) + ".txt").c_str(), "w")) {
                    std::fprintf(f, "track %s\n", opt.track.c_str());
                    std::fprintf(f, "--camera %d,%d,%d,%d,%d,%d\n", int(cam.x), int(cam.y), int(cam.z), int(cam.yaw),
                                 int(cam.pitch), int(cam.roll));
                    if (player) {
                        const scr::Car& c = player->race.player;
                        std::fprintf(f, "car %d,%d,%d yaw %.4f\n", c.pos_x, c.pos_y, c.pos_z, double(c.yaw));
                    }
                    std::fclose(f);
                }
            }
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
            const scr::Car& c = player->race.player;
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
    if (opt.perf && perf_frames)
        SDL_Log("render3d: %.2f ms average, %.2f ms worst over %d frames at %dx%d", perf_total / perf_frames, perf_max,
                perf_frames, fb.width(), fb.height());
    shared.frames_run = frame;
    if (!opt.record_audio.empty() && !write_wav(opt.record_audio, recorded, 44100)) exit_code = 1;
    if (audio_stream) SDL_DestroyAudioStream(audio_stream);
    race_audio.stop();
    mixer.set_music(nullptr);
    SDL_DestroyTexture(tex);
    return exit_code;
}

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

    // Without --scene the game starts in the menu (when the game files are there).
    if (opt.scene.empty() && game_dir) opt.scene = "menu";
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

    Shared shared;
    if (!opt.headless) {
        if (char* pref = SDL_GetPrefPath("", "ScreamerRecomp")) {
            shared.choice_path = std::string(pref) + "CHOICE.DAT";
            SDL_free(pref);
        }
    }
    int exit_code = 0;
    for (;;) {
        shared.back_to_menu = false;
        shared.restart_race = false;
        exit_code = run_scene(opt, window, renderer, game_dir, title, shared);
        if (exit_code != 0) break;
        if (opt.frames) {
            // --frames counts over all the scenes of the run
            if (shared.frames_run >= opt.frames) break;
            opt.frames -= shared.frames_run;
        }
        if (shared.back_to_menu) {
            // the race is over: the menu takes the result (shared.race / outcome)
            opt.scene = "menu";
            opt.hold.clear();
            continue;
        }
        if (shared.race) {
            // frontend_start_race: the race the menu chose, then back to the menu
            opt.scene = "drive";
            opt.track = "MAP" + std::to_string(shared.race->map);
            opt.car_index = shared.race->car;
            opt.laps = shared.race->laps;
            opt.level = shared.race->level;
            opt.detail = shared.race->high_detail ? "H" : "L";
            opt.speech = shared.race->speech;
            opt.music = shared.race->music;
            opt.from_menu = true;
            opt.game_mode = shared.race->game_mode;
            opt.ghost = shared.race->ghost;
            if (!shared.choice_path.empty())
                opt.ghost_dir = shared.choice_path.substr(0, shared.choice_path.size() - std::strlen("CHOICE.DAT"));
            opt.start.clear();
            opt.menu_keys.clear();
            continue;
        }
        break;
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
