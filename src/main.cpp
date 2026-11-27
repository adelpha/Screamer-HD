// Screamer HD: SDL3 frontend.
//
// Until the original game logic is reconstructed this shows the title screen from
// the user's game files (--scene title), a track from a fixed camera (--scene track)
// or a deterministic test pattern (--scene testpattern) through the full render path. The command line is designed for
// automated testing: --headless runs without a visible window (SDL "offscreen"
// video driver, software renderer, dummy audio) and --frames/--screenshot/
// --dump-fb produce reproducible images the test harness compares.

#include "core/animations.h"
#include "core/attract.h"
#include "core/profiler.h"
#include "core/replay.h"
#include "core/tv_camera.h"
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
#include "core/track_edit.h"
#include "core/sim_clock.h"
#include "core/testpattern.h"
#include "debug_report.h"
#include "input.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <chrono>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif
#include <cstdio>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <array>
#include <cstring>
#include <vector>

namespace {

// Car shadows are tested against the depth buffer as if this much nearer (see Pose::depth_bias).
constexpr float kShadowBias = 64.0f;
// Obstacles (cones, barriers) likewise, so their lower half shows over the ground.
constexpr float kObstacleBias = 512.0f;

struct Options {
    std::string data_dir;
    bool headless = false;
    bool hidden = false;            // --hidden: as --headless, but the real video driver and renderer on a window that is never shown
    std::string upload;             // --upload: how the picture gets to the renderer (index | rgba; empty: index when the renderer can)
    bool perf = false;              // print the 3D render time per frame at the end
    bool lod = false;               // the original's distant versions of objects (off: full detail)
    bool mipmaps = true;            // --mipmaps on|off: squeezed textures from their smaller versions (ours; F10 in a race)
    float mip_sharpness = 1.4f;     // --mip-sharpness
    bool clamp_edges = true;        // --texture-edges clamp|wrap
    bool seal_all = false;          // --seal-edges all|open
    bool weld_joints = true;        // --weld-joints on|off
    bool track_edits = true;        // --track-edits on|off: the hand-made corner moves of TRACKS\<track>.TXT (ours)
    std::string track_edits_file;   // --track-edits-file: that file instead (also in headless runs)
    bool sky_tilt = true;           // --sky-tilt on|off
    bool face_lists = true;         // --face-lists on|off
    bool folded_faces = true;       // --folded-faces on|off
    bool ground_obstacles = true;   // --ground-obstacles on|off
    int threads = 0;                // renderer threads (0: one per hardware thread)
    std::string fill;               // --fill: the pixel filler's build (sse2 | avx2; empty: the widest the CPU has)
    bool fullscreen = false;
    bool vsync = true;              // --vsync on|off (interactive runs)
    int frame_limit = 0;            // --frame-limit N: pictures a second at most (0: none)
    bool native_res = false;        // the render size is the window's (OPTIONS > VIDEO's RES NATIVE)
    std::vector<std::string> given; // the options given on the command line (they win over the saved ones)
    bool was_given(const char* name) const { return std::find(given.begin(), given.end(), name) != given.end(); }
    uint64_t frames = 0;            // 0 = run until quit
    std::string screenshot;         // rendered output after the last frame
    std::string dump_fb;            // native-resolution framebuffer after the last frame
    uint64_t capture_every = 0;     // dump framebuffer every N frames into capture_dir
    std::string capture_dir = "artifacts/capture";
    int width = 0, height = 0;      // output size; 0 = default
    std::string renderer;           // SDL renderer name, empty = default
    std::string aspect = "4:3";     // 4:3 | stretch | square | wide
    bool fb_set = false;            // --render-size was given
    bool smooth_turn = true;        // --smooth-turn: the camera's angles as they are, not in 2048ths of a turn
    std::string scene;              // title | track | testpattern; empty = title when game data is found
    std::string track = "MAP2";     // --scene track: which track
    bool reversed = false;          // tracks 7-12: the track driven the other way (DATA/MAPnI.DAT)
    bool attract = false;           // the intro demo (attract_mode_run): CPU cars, TV camera, credits
    std::string replay_file;        // play this .RPL back (the race runs again from its inputs)
    float view_distance = 2.0f;     // --view-distance: times the original's far clip (43,808); 0: no limit
    uint64_t capture_from = 0;      // --capture-from: --capture-every starts at this frame
    bool tween_test = false;        // --tween-test: headless, one tick a loop, so every other picture is drawn between steps
    std::string save_dir;           // --save-dir: our saves here instead of the pref path
    std::string record_file;        // --record: save the race's replay here when the run ends
    std::string positions_file;     // --positions: player and CPU positions every frame (vs tools/dosbox/record-replay.ps1)
    bool instant_replay = false;    // the replay right after a single race: PLAY AGAIN? at its end
    bool moving_scenery = true;     // track_load_animations runs above LOW detail (DAT_0010cf54)
    int race_options = 0;           // g_race_options: the cheats (obstacle column, clock, mirror, upside down)
    std::string camera;             // --scene track: x,y,z,yaw,pitch[,roll]
    int fb_width = 320, fb_height = 200;  // internal render size (--render-size)
    std::string ui = "auto";            // --ui: the HUD and menu graphics, low (320x200), high (640x400) or auto
    bool flat = false;              // --scene track: no textures
    std::vector<std::string> cars;  // --scene track: NAME[,X,Y,Z,YAW] per car
    std::string detail = "H";
    int car_index = 0;              // --scene drive: CARS_DATA record (0..12)
    std::string start;              // --scene drive: X,Z start position
    std::string hold;
    int view = 2;                   // --scene drive: starting view 0..3 (F1..F4)
    bool mute = false;              // no audio device
    int sfx_volume = 100, music_volume = 70;  // 0..100
    std::string volumes;            // --volumes A,E,F,M: the SOUND page's settings for this run
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
        "  --data DIR           original game directory (default: $SCREAMER_DATA, the build's path in\n"
        "                       developer builds, a SCREAMER folder next to screamer.exe, screamer.exe's folder)\n"
        "  --headless           no visible window, software renderer, dummy audio\n"
        "  --hidden             like --headless, but with the real video driver and renderer (the graphics card) on a\n"
        "                       window that is never shown; for checking and timing what a real run does\n"
        "  --upload index|rgba  how the picture is handed to the renderer: as palette indices with the palette\n"
        "                       (default when the renderer can) or converted to RGBA here\n"
        "  --perf               print how long the 3D view takes to draw\n"
        "  --lod                draw distant objects in the original's simpler versions (default: full detail)\n"
        "  --threads N          threads drawing the 3D view (default: one per CPU thread)\n"
        "  --mipmaps on|off     draw squeezed (distant) textures from averaged smaller versions: steadier, softer\n"
        "                       (default on; F10 switches in a race). --mip-sharpness X: 1 sharper .. 2 softer (1.4)\n"
        "  --weld-joints off    road pieces where the track file puts them (their lines step at every joint)\n"
        "  --track-edits off    leave out the corners moved in photo mode's edit mode (TRACKS\\MAPnH.TXT beside\n"
        "                       CHOICE.DAT); --track-edits-file F: those of file F (drawing only, never the physics)\n"
        "  --face-lists off     draw every face that faces the eye, not only those in the part's draw order\n"
        "  --folded-faces off   test faces that are not flat by their one normal, as the original (holes)\n"
        "  --view-distance X    times the original's far clip (1..8, 0: no limit; default 2, or as OPTIONS > VIDEO)\n"
        "  --level N            the CPU cars' level, 0-4 (default 1; 3: the championship's BULLET league)\n"
        "  --sky-tilt off       the sky stays level when the camera rolls, as before\n"
        "  --ground-obstacles off  cones, barriers and signs where the original draws them, half below the\n"
        "                       road (their base then slides over the road as the camera nears)\n"
        "  --smooth-turn off    camera, car and sky angles in whole 2048ths of a turn, as the original\n"
        "                       (large pictures show the steps in slow turns)\n"
        "  --seal-edges all     road pieces reach past every face edge, as before (the painted lines break up)\n"
        "  --texture-edges wrap texel coordinates wrap at the edges of every polygon, as before\n"
        "  --fill sse2|avx2     the build of the pixel filler (default: avx2 when the CPU has it; same picture)\n"
        "  --frames N           exit after N simulation frames (2 ticks of the game's 100 Hz clock each)\n"
        "  --scene NAME         menu (default when the game files are found), title, track, drive or testpattern\n"
        "  --menu-keys LIST     --scene menu: keys pressed at frames, e.g. 40:down,60:enter (headless runs)\n"
        "  --track NAME         track for --scene track / drive (MAP1..MAP6, default MAP2)\n"
        "  --reversed           drive the track the other way (tracks 7-12 of the menu)\n"
        "  --replay FILE        play a .RPL replay back (its track is --track)\n"
        "  --record FILE        --scene drive: save the race as a .RPL replay when the run ends\n"
        "  --save-dir DIR       keep CHOICE.DAT and the replays in DIR (default: the pref path; none headless)\n"
        "  --race-options N     the cheats' g_race_options: 1/2 other obstacles, 4 clock stopped, 0x10 mirror, 0x20 upside down\n"
        "  --car-index N        --scene drive: car from CARS_DATA (0..12, default 0)\n"
        "  --view N             --scene drive: starting view 0..3 (F1..F4, default 2)\n"
        "  --game-mode N        --scene drive: 1 normal, 4 time attack, 0x20 cone carnage, 0x40 slalom\n"
        "  --positions FILE     --scene drive: write the player's and CPU cars' positions every frame\n"
        "  --start X,Z          --scene drive: start position (default: the intro demo's grid)\n"
        "  --hold KEYS          --scene drive: controls held every frame, e.g. accel,left (headless runs); finish ends\n"
        "                       the race at frame 700, promptno answers PLAY AGAIN? with no\n"
        "  --detail high|low    track detail: GFX/MAPnH.DAT (default, as the game's high detail) or MAPnL.DAT\n"
        "  --camera X,Y,Z,YAW,PITCH[,ROLL]  camera for --scene track (game units, angles 0..2047)\n"
        "  --render-size WxH    internal resolution for 3D scenes (default 320x200)\n"
        "  --ui low|high|auto   HUD and menu graphics: the original's 320x200 set, STARTH.EXE's 640x400 set,\n"
        "                       or auto (default): 640x400 when the render size has 400 lines or more\n"
        "  --flat               --scene track without textures\n"
        "  --car NAME[,X,Y,Z,YAW[,ANIM]]  add a car (e.g. AUTO00) to --scene track; repeatable; ANIM\n"
        "                       is its animation bits in hex (8000 = second paint scheme); without a\n"
        "                       position it stands 900 units in front of the camera\n"
        "  --screenshot FILE    save the rendered window output after the last frame (.png/.bmp)\n"
        "  --dump-fb FILE       save the native 320x200 framebuffer after the last frame\n"
        "  --capture-every N    save the framebuffer every N frames\n"
        "  --capture-dir DIR    directory for --capture-every output (default artifacts/capture)\n"
        "  --capture-from N     --capture-every starts at frame N\n"
        "  --tween-test         headless: one tick a loop, so every other picture is drawn between game steps\n"
        "  --size WxH           window / output size\n"
        "  --fullscreen         borderless fullscreen (--windowed: in a window; default: as OPTIONS > VIDEO)\n"
        "  --vsync on|off       wait for the screen's refresh (default on, or as OPTIONS > VIDEO)\n"
        "  --frame-limit N      at most N pictures a second (default 0: no limit)\n"
        "  --renderer NAME      SDL renderer (direct3d11, direct3d12, vulkan, opengl, software, gpu)\n"
        "  --aspect MODE        4:3 (default), stretch, square, or wide: widescreen. The picture fills a\n"
        "                       window of any shape with square pixels; races show more to the sides,\n"
        "                       the HUD keeps to the edges, menus stay 4:3 in the middle. Without\n"
        "                       --render-size the picture is then drawn at the window's size.\n"
        "  --mute               no sound\n"
        "  --sfx-volume N       sound effects volume 0..100 (default 100)\n"
        "  --music-volume N     CD music volume 0..100 (default 70)\n"
        "  --volumes A,E,F,M    the SOUND options (0..10 each): announcer, your engine, other effects, music\n"
        "                       (default: as saved, else 10)\n"
        "  --no-music           no CD music (from the CD image: SCREAM.cue/.bin, or SCREAM.DAT/.gog as Steam and GOG ship it)\n"
        "  --no-speech          no announcer\n"
        "  --music-source SRC   cd, midi (the FM synth versions in DATA/MUSIC.DAT) or auto (CD if found);\n"
        "                       default: OPTIONS > SOUND's CD MUSIC (auto) / MIDI MUSIC\n"
        "  --record-audio FILE  write the sound of the run to a 44.1 kHz .wav (also headless)\n"
        "  --audio-filter MODE  linear (default) or nearest (the original driver's crunchier resampling)\n"
        "  --audio-log          log every sound that starts\n"
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

// Drawing between game steps: the picture is blended from the previous step's state to the
// current one (rendering only; the game itself always runs in whole 1/50 s steps).
float tween_angle(float a, float b, float t, float turn) {
    float d = std::fmod(b - a, turn);
    if (d > turn * 0.5f) d -= turn;
    if (d < -turn * 0.5f) d += turn;
    return a + d * t;
}
int tween_int(int a, int b, float t) { return a + int(std::lround(double(b - a) * double(t))); }
constexpr float kTwoPiF = 6.2831853071795862f;
void tween_car(scr::Car& out, const scr::Car& a, const scr::Car& b, float t) {
    out = b;
    if (std::abs(b.pos_x - a.pos_x) + std::abs(b.pos_z - a.pos_z) > 8000) return;  // put back on the track
    out.pos_x = tween_int(a.pos_x, b.pos_x, t);
    out.pos_y = tween_int(a.pos_y, b.pos_y, t);
    out.pos_z = tween_int(a.pos_z, b.pos_z, t);
    out.ground_y = tween_int(a.ground_y, b.ground_y, t);
    out.body_ofs_y = tween_int(a.body_ofs_y, b.body_ofs_y, t);
    out.pitch = tween_angle(a.pitch, b.pitch, t, kTwoPiF);
    out.yaw = tween_angle(a.yaw, b.yaw, t, kTwoPiF);
    out.roll = tween_angle(a.roll, b.roll, t, kTwoPiF);
    out.drift_angle = tween_angle(a.drift_angle, b.drift_angle, t, kTwoPiF);
    out.body_pitch_ofs = tween_angle(a.body_pitch_ofs, b.body_pitch_ofs, t, kTwoPiF);
    out.body_roll_ofs = tween_angle(a.body_roll_ofs, b.body_roll_ofs, t, kTwoPiF);
}
void tween_opponent(scr::Opponent& out, const scr::Opponent& a, const scr::Opponent& b, float t) {
    out = b;
    if (std::abs(b.pos_x - a.pos_x) + std::abs(b.pos_z - a.pos_z) > 8000) return;
    out.pos_x = tween_int(a.pos_x, b.pos_x, t);
    out.pos_y = tween_int(a.pos_y, b.pos_y, t);
    out.pos_z = tween_int(a.pos_z, b.pos_z, t);
    out.ground_y = tween_int(a.ground_y, b.ground_y, t);
    out.pitch = tween_angle(a.pitch, b.pitch, t, kTwoPiF);
    out.draw_yaw = tween_angle(a.draw_yaw, b.draw_yaw, t, kTwoPiF);
    out.roll = tween_angle(a.roll, b.roll, t, kTwoPiF);
}
scr::Camera tween_camera(const scr::Camera& a, const scr::Camera& b, float t) {
    scr::Camera c = b;
    // a cut (another camera, a view switch with no blend): no blending across it
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    auto turn = [](float p, float q) { float d = std::fmod(std::fabs(q - p), 2048.0f); return std::min(d, 2048.0f - d); };
    if (dx * dx + dy * dy + dz * dz > 5000.0f * 5000.0f || turn(a.yaw, b.yaw) > 256 || turn(a.pitch, b.pitch) > 256) return c;
    c.x = a.x + dx * t;
    c.y = a.y + dy * t;
    c.z = a.z + dz * t;
    c.yaw = std::fmod(tween_angle(a.yaw, b.yaw, t, 2048.0f) + 2048.0f, 2048.0f);
    c.pitch = std::fmod(tween_angle(a.pitch, b.pitch, t, 2048.0f) + 2048.0f, 2048.0f);
    c.roll = std::fmod(tween_angle(a.roll, b.roll, t, 2048.0f) + 2048.0f, 2048.0f);
    return c;
}

// replay_load: the race a .RPL was recorded in (car, laps, level, mode, options)
bool apply_replay_header(Options& opt) {
    scr::Replay r;
    if (!r.load(opt.replay_file)) return false;
    opt.car_index = r.car;
    opt.laps = r.laps;
    opt.level = r.level;
    opt.game_mode = r.game_mode & 0x7f;
    opt.race_options = r.race_options & 0xff;
    opt.reversed = (r.race_options & scr::Replay::kReversed) != 0;
    return true;
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        o.given.push_back(a);
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); std::exit(2); }
            return argv[++i];
        };
        if (a == "--data") o.data_dir = next("--data");
        else if (a == "--headless") o.headless = true;
        else if (a == "--hidden") o.headless = o.hidden = true;
        else if (a == "--upload") o.upload = next("--upload");
        else if (a == "--perf") o.perf = true;
        else if (a == "--lod") o.lod = true;
        else if (a == "--mipmaps") o.mipmaps = std::string(next("--mipmaps")) != "off";
        else if (a == "--texture-edges") o.clamp_edges = std::string(next("--texture-edges")) != "wrap";
        else if (a == "--seal-edges") o.seal_all = std::string(next("--seal-edges")) == "all";
        else if (a == "--sky-tilt") o.sky_tilt = std::string(next("--sky-tilt")) != "off";
        else if (a == "--face-lists") o.face_lists = std::string(next("--face-lists")) != "off";
        else if (a == "--folded-faces") o.folded_faces = std::string(next("--folded-faces")) != "off";
        else if (a == "--ground-obstacles") o.ground_obstacles = std::string(next("--ground-obstacles")) != "off";
        else if (a == "--smooth-turn") o.smooth_turn = std::string(next("--smooth-turn")) != "off";
        else if (a == "--weld-joints") o.weld_joints = std::string(next("--weld-joints")) != "off";
        else if (a == "--track-edits") o.track_edits = std::string(next("--track-edits")) != "off";
        else if (a == "--track-edits-file") o.track_edits_file = next("--track-edits-file");
        else if (a == "--mip-sharpness") o.mip_sharpness = float(std::atof(next("--mip-sharpness")));
        else if (a == "--threads") o.threads = std::atoi(next("--threads"));
        else if (a == "--fill") o.fill = next("--fill");
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a == "--windowed") o.fullscreen = false;
        else if (a == "--vsync") o.vsync = std::string(next("--vsync")) != "off";
        else if (a == "--frame-limit") o.frame_limit = std::max(0, std::atoi(next("--frame-limit")));
        else if (a == "--frames") o.frames = std::strtoull(next("--frames"), nullptr, 10);
        else if (a == "--screenshot") o.screenshot = next("--screenshot");
        else if (a == "--dump-fb") o.dump_fb = next("--dump-fb");
        else if (a == "--capture-every") o.capture_every = std::strtoull(next("--capture-every"), nullptr, 10);
        else if (a == "--capture-dir") o.capture_dir = next("--capture-dir");
        else if (a == "--renderer") o.renderer = next("--renderer");
        else if (a == "--aspect") o.aspect = next("--aspect");
        else if (a == "--scene") o.scene = next("--scene");
        else if (a == "--track") o.track = next("--track");
        else if (a == "--reversed") o.reversed = true;
        else if (a == "--replay") o.replay_file = next("--replay");
        else if (a == "--record") o.record_file = next("--record");
        else if (a == "--save-dir") o.save_dir = next("--save-dir");
        else if (a == "--tween-test") o.tween_test = true;
        else if (a == "--view-distance") o.view_distance = float(std::atof(next("--view-distance")));
        else if (a == "--capture-from") o.capture_from = std::strtoull(next("--capture-from"), nullptr, 10);
        else if (a == "--positions") o.positions_file = next("--positions");
        else if (a == "--race-options") o.race_options = int(std::strtol(next("--race-options"), nullptr, 0));
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
        else if (a == "--volumes") o.volumes = next("--volumes");
        else if (a == "--no-music") o.music = false;
        else if (a == "--no-speech") o.speech = false;
        else if (a == "--music-source") o.music_source = next("--music-source");
        else if (a == "--record-audio") o.record_audio = next("--record-audio");
        else if (a == "--menu-keys") o.menu_keys = next("--menu-keys");
        else if (a == "--game-mode") o.game_mode = int(std::strtol(next("--game-mode"), nullptr, 0));
        else if (a == "--level") o.level = std::clamp(std::atoi(next("--level")), 0, 4);
        else if (a == "--audio-log") o.audio_log = true;
        else if (a == "--audio-filter") o.audio_smooth = std::string(next("--audio-filter")) != "nearest";
        else if (a == "--detail") o.detail = std::string(next("--detail")) == "low" ? "L" : "H";
        else if (a == "--ui") o.ui = next("--ui");
        else if (a == "--render-size") {
            if (std::sscanf(next("--render-size"), "%dx%d", &o.fb_width, &o.fb_height) != 2 || o.fb_width <= 0 || o.fb_height <= 0) {
                std::fprintf(stderr, "--render-size expects WxH\n");
                return false;
            }
            o.fb_set = true;
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
    if (aspect == "square" || aspect == "wide") target = double(fb_w) / fb_h;
    else target = 4.0 / 3.0;
    double w = out_w, h = out_w / target;
    if (h > out_h) { h = out_h; w = out_h * target; }
    return {float((out_w - w) / 2), float((out_h - h) / 2), float(w), float(h)};
}

// OPTIONS > VIDEO as the run has it now
scr::VideoSettings video_from(const Options& o) {
    scr::VideoSettings v;
    v.fullscreen = o.fullscreen;
    v.widescreen = o.aspect == "wide";
    if (!o.native_res) {
        v.width = o.fb_width;
        v.height = o.fb_height;
    }
    v.view_distance = o.view_distance;
    v.vsync = o.vsync;
    v.frame_limit = o.frame_limit;
    v.lod = o.lod;
    v.mipmaps = o.mipmaps;
    return v;
}

// The saved (or just chosen) VIDEO settings into the run. Without force, what the command line
// gave stays.
void apply_video(Options& o, const scr::VideoSettings& v, bool force) {
    auto take = [&](const char* name) { return force || !o.was_given(name); };
    if (take("--fullscreen") && take("--windowed")) o.fullscreen = v.fullscreen;
    if (take("--aspect")) o.aspect = v.widescreen ? "wide" : "4:3";
    if (take("--render-size")) {
        o.native_res = v.width <= 0 || v.height <= 0;
        o.fb_set = !o.native_res;
        if (o.fb_set) {
            o.fb_width = v.width;
            o.fb_height = v.height;
        }
    }
    if (take("--view-distance")) o.view_distance = v.view_distance;
    if (take("--vsync")) o.vsync = v.vsync;
    if (take("--frame-limit")) o.frame_limit = v.frame_limit;
    if (take("--lod")) o.lod = v.lod;
    if (take("--mipmaps")) o.mipmaps = v.mipmaps;
}

// The window for the run's VIDEO settings: fullscreen or not, and in a window, the size of the
// picture when it fits on the screen (so its pixels are the screen's).
void apply_window(SDL_Window* window, const Options& o) {
    SDL_SetWindowFullscreen(window, o.fullscreen);
    if (!o.fullscreen && o.fb_set && o.fb_height >= 480) {
        const int w = o.aspect == "wide" ? o.fb_width : int(std::lround(double(o.fb_height) * 4.0 / 3.0)), h = o.fb_height;
        const float density = std::max(1.0f, SDL_GetWindowPixelDensity(window));
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable)) {
            const int pw = int(std::lround(w / density)), ph = int(std::lround(h / density));
            if (pw <= usable.w && ph + 40 <= usable.h) {
                SDL_SetWindowSize(window, pw, ph);
                SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            }
        }
    }
    SDL_SyncWindow(window);
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
        std::string replay;  // its replay (temp<n>.rpl in replay_dir), for the records
    };
    std::vector<Outcome> outcomes;      // the races since the menu (PLAY AGAIN repeats it)
    std::string choice_path;            // our copy of CONFIG/CHOICE.DAT
    scr::input::Input input;            // the controls (input.h)
    std::string controls_path;          // CONTROLS.INI next to CHOICE.DAT ("": not saved)
    // the SOUND page: announcer, the player's engine, other effects, music (0..kVolumeMax)
    std::array<int, 4> volumes{scr::kVolumeMax, scr::kVolumeMax, scr::kVolumeMax, scr::kVolumeMax};
    bool music_midi = false;            // and its CD MUSIC / MIDI MUSIC line
    scr::VideoSettings saved_video;     // OPTIONS > VIDEO as saved in CHOICE.DAT (not what the command line gave)
    bool back_to_menu = false;          // the race asks to return to the menu
    bool attract_done = false;          // the intro demo is over (a key, or the credits ended)
    bool to_demo = false;               // the menu was left alone: the intro demo again
    bool play_replay = false;           // race_run: play the race just run back (single races)
    std::string replay_dir;             // <pref>/REPLAYS: temp.rpl and REPLAY<track><n>.RPL
    bool restart_race = false;          // PLAY AGAIN? yes
    uint64_t frames_run = 0;
};

// One scene (--scene ...): loads it and runs its loop until it ends.
int run_scene(Options& opt, SDL_Window* window, SDL_Renderer* renderer, const std::optional<std::string>& game_dir,
              std::optional<scr::PcxImage>& title, Shared& shared) {
    // --aspect wide: square pixels, and a picture as large as the window unless told otherwise
    // (headless runs: the size asked for, whatever the window turns out to be)
    // (OPTIONS > VIDEO's RES NATIVE: the same, and on a 4:3 picture the largest 4:3 part of the window)
    const bool wide = opt.aspect == "wide";
    if ((wide || opt.native_res) && !opt.fb_set) {
        int ww = opt.width, wh = opt.height;
        if (!opt.headless) SDL_GetWindowSizeInPixels(window, &ww, &wh);
        if (ww > 0 && wh > 0) {
            if (!wide) {
                wh = std::min(wh, int(std::lround(double(ww) * 3.0 / 4.0)));
                ww = int(std::lround(double(wh) * 4.0 / 3.0));
            }
            opt.fb_width = ww;
            opt.fb_height = wh;
        }
    }
    // STARTH.EXE's 640x400 HUD and menu graphics once the picture has the lines for them
    const bool ui_hires = opt.ui == "high" || (opt.ui != "low" && opt.fb_height >= 400);
    // A track viewed from a fixed camera (the first step of the 3D renderer).
    std::optional<scr::Track> track;
    scr::Camera cam;
    scr::TrackAnimations anims;           // trucks, jets, trams... on their splines
    scr::Obstacles obstacles;             // cones and the like (MAPnOST.SPL in races)
    scr::TvCamera tv_camera;               // the intro demo's camera
    scr::Replay replay;                    // playing back (opt.replay_file) or recording
    size_t replay_pos = 0;
    const bool replaying = !opt.replay_file.empty() && replay.load(opt.replay_file);
    std::FILE* positions = opt.positions_file.empty() || opt.scene != "drive" ? nullptr : std::fopen(opt.positions_file.c_str(), "wb");
    scr::AttractOverlay attract;           // the intro demo's logo and credits
    std::optional<std::string> gfx_path;  // GFX/MAPn?.DAT
    // Time attack: the ghost of the best lap (ghost_record_frame / ghost_update), kept as
    // <pref dir>/MAPn.ghost: int car, float lap time, then frames of
    // { x, y, z, pitch, yaw, roll, animation, reflection, time }.
    // Time attack: the ghost of the best lap, in the original's MAPn.ATK format (attack_save /
    // attack_load): a 0x24-byte header { int capacity, int index, int frames, int valid,
    // int buffer, int race_options, int car, float lap time, int pointer } and 0x28 bytes a
    // frame { int x, y, z; float pitch, yaw, roll; int animation word, reflection word; float
    // track progress, lap time } (ghost_record_frame). Ours are kept in the pref dir (the
    // original wrote GAME/REPLAYS); an .ATK of the original's in REPLAYS is read too.
    struct GhostFrame {
        int x, y, z;
        float pitch, yaw, roll;
        int32_t animation, reflection;
        float progress, time;
    };
    static_assert(sizeof(GhostFrame) == 0x28);
    struct Ghost {
        std::string path, fallback;
        int car = 0;
        float lap = 0;              // 0: none
        std::vector<GhostFrame> frames, recording;
        bool load_from(const std::string& p) {
            std::FILE* f = std::fopen(p.c_str(), "rb");
            if (!f) return false;
            int32_t h[9]{};
            bool ok = std::fread(h, 4, 9, f) == 9;
            frames.clear();
            if (ok && h[3] != 0 && h[2] > 0 && h[2] < 1000000) {
                frames.resize(size_t(h[2]));
                ok = std::fread(frames.data(), sizeof(GhostFrame), frames.size(), f) == frames.size();
            }
            std::fclose(f);
            float l = 0;
            std::memcpy(&l, &h[7], 4);
            car = h[6];
            lap = ok && !frames.empty() ? l : 0;
            return lap > 0;
        }
        bool load() { return load_from(path) || (!fallback.empty() && load_from(fallback)); }
        void save(float lap_time, int race_options, int player_car) {
            std::FILE* f = std::fopen(path.c_str(), "wb");
            if (!f) return;
            int32_t h[9]{};
            h[0] = h[1] = h[2] = int32_t(recording.size());
            h[3] = 1;
            h[5] = race_options;
            h[6] = player_car;
            std::memcpy(&h[7], &lap_time, 4);
            std::fwrite(h, 4, 9, f);
            std::fwrite(recording.data(), sizeof(GhostFrame), recording.size(), f);
            std::fclose(f);
        }
    } ghost;
    scr::Model ghost_model;              // the ghost's car when it is another than the player's
    std::vector<scr::Model> anim_models;  // per animation (an empty model draws nothing)
    const bool drive = opt.scene == "drive";
    std::vector<std::vector<scr::VertexShift>> weld_base;  // the track's automatic welds, before the edits
    scr::TrackEdits track_edits;
    std::string track_edits_path;  // where the edits are kept (empty: nowhere)
    if (opt.scene == "track" || drive) {
        if (game_dir) {
            if (auto path = scr::resolve_game_file(*game_dir, "GFX/" + opt.track + opt.detail + ".DAT")) track = scr::load_track(*path, opt.track);
        }
        if (!track) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "cannot load track %s from the game files", opt.track.c_str());
            return 1;
        }
        SDL_Log("track %s: %zu objects, %zu placements", opt.track.c_str(), track->objects.size(), track->placements.size());
        // The corners moved by hand in photo mode's edit mode (core/track_edit.h): drawing only.
        weld_base = track->welds;
        track_edits.track = opt.track + opt.detail;
        track_edits.placements = track->placements.size();
        if (opt.track_edits) {
            if (!opt.track_edits_file.empty()) track_edits_path = opt.track_edits_file;
            else if (!shared.choice_path.empty())
                track_edits_path = (std::filesystem::path(shared.choice_path).parent_path() / "TRACKS" / (track_edits.track + ".TXT")).string();
            std::string err;
            std::error_code ec;
            if (!track_edits_path.empty() && std::filesystem::exists(track_edits_path, ec)) {
                if (track_edits.load(track_edits_path, &err)) {
                    scr::apply_track_edits(*track, weld_base, track_edits);
                    SDL_Log("track edits: %zu corners from %s", track_edits.corners.size(), track_edits_path.c_str());
                } else {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "track edits %s not used: %s", track_edits_path.c_str(), err.c_str());
                    track_edits.corners.clear();
                    track_edits_path.clear();  // (and not overwritten)
                }
            }
        }
        // Tracks 7-12 (g_race_options & 8): the same scenery, driven the other way. The racing
        // lines, markers, obstacles and the rest come from DATA/MAPnI.DAT, and
        // track_find_nearest_point turns the centre line's heading round.
        const std::string data_name = "DATA/" + opt.track + (opt.reversed ? "I" : "") + ".DAT";
        if (opt.reversed)
            for (scr::TrackPoint& tp : track->points) {
                tp.heading += 3.1415927410125732f;
                if (6.2831854820251465f <= tp.heading) tp.heading += -6.2831854820251465f;
            }
        // Moving scenery (track_load_animations).
        auto data = scr::resolve_game_file(*game_dir, data_name);
        auto gfx = scr::resolve_game_file(*game_dir, "GFX/" + opt.track + opt.detail + ".DAT");
        gfx_path = gfx;
        if (data && gfx) obstacles.load(*data, *gfx, opt.track, "OST", *track, opt.race_options & 3);
        if (data && gfx && opt.moving_scenery && anims.load(*data, opt.track)) {
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
        std::array<float, 2> shadow_angles{};           // the player's (kept while it tumbles)
        int car_index = 0;                              // CARS_DATA record of the player's car
        scr::CarParts model_parts;                      // the player's car parts that move
        std::vector<scr::PartPose> part_poses;
        std::vector<scr::CarParts> cpu_parts;
        std::vector<int32_t> cpu_roll;
        std::vector<std::vector<scr::PartPose>> cpu_poses;
    };
    std::optional<Player> player;
    std::optional<scr::Hud> hud;  // the race HUD (--scene drive with CPU cars)
    // Sound: the mixer always runs (deterministic, testable); the device only in a window.
    scr::SoundBank sound_bank;
    scr::Mixer mixer;
    scr::RaceAudio race_audio;
    scr::CdAudio cd;  // the CD image and/or the ripped tracks of the MUSIC folder
    int cd_track = 0;  // the CD track playing (0: MIDI or none)
    int midi_song = 0;  // the MIDI tune playing (MUSICn.HMP; 0: CD or none)
    SDL_AudioStream* audio_stream = nullptr;
    std::vector<float> recorded;  // --record-audio
    mixer.sfx_volume = float(opt.sfx_volume) / 100.0f;
    mixer.music_volume = float(opt.music_volume) / 100.0f;
    mixer.smooth = opt.audio_smooth;
    auto apply_group_volumes = [&]() {
        for (int g = 0; g < scr::kSoundGroups; ++g)
            mixer.set_group_volume(scr::SoundGroup(g), float(shared.volumes[size_t(g)]) / float(scr::kVolumeMax));
    };
    apply_group_volumes();
    // The music: the CD's track, or the MIDI tune on the FM synth (OPTIONS > SOUND, ours; the
    // original plays MIDI only without the CD). --music-source cd|midi wins over the setting;
    // CD falls back to MIDI when there is no CD image. Returns the CD track playing, 0 for none.
    bool cd_open = false;
    auto start_music = [&](int track_no, int song_no) -> int {
        if (!opt.music || !((!opt.headless && !opt.mute) || !opt.record_audio.empty()) || !game_dir) return 0;
        const std::string source = opt.music_source == "cd" || opt.music_source == "midi" ? opt.music_source
                                   : shared.music_midi                                   ? "midi"
                                                                                         : "auto";
        mixer.set_music(nullptr);
        midi_song = 0;
        if (source != "midi") {
            if (!cd_open) {
                cd_open = cd.open(*game_dir);
                if (!cd_open)
                    SDL_Log("no CD music (no CD image: SCREAM.cue/.bin or SCREAM.DAT/.gog; no MUSIC folder)");
                else if (!cd.files.empty())
                    SDL_Log("CD music: %d ripped track(s) in the MUSIC folder%s", int(cd.files.size()),
                            cd.image ? ", the rest from the CD image" : "");
            }
            if (cd_open) {
                if (auto music = cd.play(track_no)) {
                    mixer.set_music(music);
                    return track_no;
                }
            }
        }
        if (source != "cd") {
            auto music_dat = scr::resolve_game_file(*game_dir, "DATA/MUSIC.DAT");
            auto song = music_dat ? scr::load_hmp_music(*music_dat, song_no) : nullptr;
            if (song) {
                mixer.set_music(song);
                midi_song = song_no;
            } else {
                SDL_Log("no MIDI music (DATA/MUSIC.DAT)");
            }
        }
        return 0;
    };
    int radar = 1;               // M cycles 0 (off) .. 3 (DAT_0011d2a0)
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
        player->model_parts = scr::find_car_parts(player->model);
        player->car_index = int(idx);
        const scr::CarSetup setup = scr::car_setup(rec, player->model);
        // CPU cars: the racing lines and OPPONENTS of DATA/<track>.DAT; each takes the next
        // CARS_DATA record, skipping the player's (opponent_car_load).
        auto data = scr::resolve_game_file(*game_dir, "DATA/" + opt.track + (opt.reversed ? "I" : "") + ".DAT");
        player->cpu_cars = opt.start.empty() && data && player->race.load(*data, opt.track, char('A' + opt.level));
        player->race.laps = opt.laps;
        player->race.mode = opt.game_mode;
        player->race.clock_stopped = (opt.race_options & 4) != 0;
        std::vector<scr::CarSetup> cpu_setups;
        const bool solo = opt.game_mode == 4 || opt.game_mode == 0x20 || opt.game_mode == 0x40;
        if (player->cpu_cars && solo) {
            // the special modes: no CPU cars, the first grid place, the mode's obstacles
            player->race.start(*track, setup, {});
            if (opt.game_mode == 0x40) player->race.loads_gates(*data, opt.track);
            if (gfx_path)
                obstacles.load(*data, *gfx_path, opt.track, opt.game_mode == 0x20 ? "CNG" : opt.game_mode == 0x40 ? "SLA" : "OST",
                               *track, opt.race_options & 3);
            if (opt.game_mode == 4) {
                // (reversed tracks share the file, as in the original: g_track_name is MAPn)
                ghost.path = opt.ghost_dir + opt.track + ".ATK";
                if (auto orig = scr::resolve_game_file(*game_dir, "REPLAYS/" + opt.track + ".ATK")) ghost.fallback = *orig;
                ghost.car = int(idx);
                if (opt.ghost && ghost.load() && ghost.car != int(idx) && ghost.car >= 0 && size_t(ghost.car) < n) {
                    // ghost_car_load: the car the ghost was driven in
                    const uint8_t* gr = cars_data->data() + size_t(ghost.car) * 0x40;
                    if (auto gm = scr::load_object(*path, std::string(reinterpret_cast<const char*>(gr)) + ".3DX")) ghost_model = std::move(*gm);
                }
            }
        } else if (player->cpu_cars) {
            size_t next = 0;
            for (size_t i = 0; i < player->race.defs.size(); ++i) {
                if (next == idx) next = (next + 1) % n;
                const uint8_t* r = cars_data->data() + next * 0x40;
                next = (next + 1) % n;
                // (opponent_car_load: above the third level, the BULLET league of the
                // championship and the normal game's hardest, every CPU car is car 12, the
                // BULLET, in the colours of the car whose turn it was)
                const uint8_t* body = opt.level > 2 && n > 0xc ? cars_data->data() + 0xc * 0x40 : r;
                auto m = scr::load_object(*path, std::string(reinterpret_cast<const char*>(body)) + ".3DX");
                if (!m) break;
                scr::CarSetup cs = scr::car_setup(body, *m);
                cs.animation = scr::car_setup(r, *m).animation;
                cpu_setups.push_back(cs);
                player->cpu_models.push_back(std::move(*m));
            }
            player->race.start(*track, setup, cpu_setups);
            if (replaying) {
                // replay_start_playback: the trackside cameras of MAPnRPL.SPL (view 3)
                if (auto a = scr::Archive::open(*data))
                    if (auto spl = a->read(opt.track + "RPL.SPL")) tv_camera.load_trackside(scr::parse_splines(*spl));
            }
            if (opt.attract) {
                // race_load_attract: g_race_flags | 0x2001, no countdown
                player->race.started = true;
                player->race.countdown_stage = -1;
                if (auto cam_spl = scr::Archive::open(*data)) {
                    if (auto spl = cam_spl->read(opt.track + "CAM.SPL")) tv_camera.load(scr::parse_splines(*spl));
                }
                if (auto graf = scr::resolve_game_file(*game_dir, "GFX/GRAF.DAT"))
                    if (auto local = scr::resolve_game_file(*game_dir, "DATA/LOCAL.DAT")) attract.load(*graf, *local);
            }
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
            if (!opt.attract) race_audio.start(mixer, sound_bank, player->race, int(idx), 0, opt.speech);
        } else {
            SDL_Log("no sound effects (DATA/SOUND.DAT not found)");
        }
        const int map = opt.track.size() >= 4 ? std::atoi(opt.track.c_str() + 3) - 1 : 0;
        cd_track = start_music(scr::cd_track_for_race(map), scr::music_for_race(map));
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
            if (!game || !hud->load(*game, ui_hires)) hud.reset();
            if (auto local = scr::resolve_game_file(*game_dir, "DATA/LOCAL.DAT"); hud && local) hud->load_texts(*local);
            // The car's dashboard: COCK<nn><A|B><L|H>.PKC and its layout COCK<nn><L|H> in CARS.DTR.
            if (hud && name.size() >= 6) {
                const std::string nn = name.substr(4, 2);
                hud->load_cockpit(*game, nn + (setup.animation ? "B" : "A"),
                                  std::string(text->begin(), text->end()), "COCK" + nn);
            }
        }
    }
    scr::input::Input& input = shared.input;
    int capture_action = -1;  // the rebinding screen waits for this action's key / button
    bool capture_pad = false;
    bool nav_block = false;   // after a capture: the menu's keys wait until all are let go
    bool pad_back_was = false;  // the main page: Back quits (on its press)
    float quit_hold = 0;        // the main page: seconds B has been held (pads without Back)
    constexpr float kQuitHold = 2.0f, kQuitHoldShow = 0.2f;
    scr::Renderer renderer3d;
    renderer3d.set_textures(!opt.flat);
    renderer3d.set_threads(opt.threads);
    if (!renderer3d.set_fill_path(opt.fill == "sse2"   ? scr::Renderer::FillPath::Sse2
                                  : opt.fill == "avx2" ? scr::Renderer::FillPath::Avx2
                                                       : scr::Renderer::FillPath::Auto)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "--fill %s: this CPU cannot run it", opt.fill.c_str());
        return 2;
    }
    renderer3d.set_lod(opt.lod);
    renderer3d.set_mipmaps(opt.mipmaps, opt.mip_sharpness);
    renderer3d.set_clamp_edges(opt.clamp_edges);
    renderer3d.set_seal_all_edges(opt.seal_all);
    renderer3d.set_weld_joints(opt.weld_joints);
    renderer3d.set_sky_tilt(opt.sky_tilt);
    renderer3d.set_face_lists(opt.face_lists);
    renderer3d.set_folded_faces(opt.folded_faces);

    // --scene menu: the front end (kept by main across races)
    scr::Menu* menu = nullptr;
    scr::VideoSettings menu_video;  // the menu's VIDEO settings when last looked at
    scr::SoundBank menu_sounds;
    // CHOICE.DAT from the menu, with OPTIONS > VIDEO as chosen there (not what the command line gave)
    auto save_menu = [&]() {
        if (shared.choice_path.empty() || !menu) return;
        scr::ChoiceFile c = menu->save_choice();
        c.video = shared.saved_video;
        c.save(shared.choice_path);
    };
    if (opt.scene == "menu") {
        // A new picture that wants the other menu graphics (640x400 or 320x200): the menu is
        // made again from them, as it was.
        std::optional<scr::ChoiceFile> kept;
        int kept_page = 0, kept_options = 0;
        std::vector<int> kept_stack;
        std::array<int, 20> kept_selected{};
        if (shared.menu_assets && shared.menu && (shared.menu_assets->res == 'H') != ui_hires) {
            kept = shared.menu->save_choice();
            kept_page = shared.menu->page;
            kept_stack = shared.menu->page_stack;
            kept_selected = shared.menu->selected;
            kept_options = shared.menu->race_options;
            shared.menu.reset();
            shared.menu_assets.reset();
        }
        if (!shared.menu_assets) {
            auto md = scr::resolve_game_file(*game_dir, "GFX/MENU.DAT");
            auto gd = scr::resolve_game_file(*game_dir, "GFX/GRAF.DAT");
            auto ld = scr::resolve_game_file(*game_dir, "DATA/LOCAL.DAT");
            shared.menu_assets.emplace();
            if (!md || !gd || !ld || !shared.menu_assets->load(*md, *gd, *ld, ui_hires)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "cannot load the menu from GFX/MENU.DAT and GFX/GRAF.DAT");
                return 1;
            }
            shared.menu.emplace(*shared.menu_assets);
            if (!shared.replay_dir.empty()) shared.menu->replay_dirs.push_back(shared.replay_dir);
            shared.menu->replay_dirs.push_back(*game_dir + "/REPLAYS/");
            // The saved menu state: ours, else the game's CONFIG/CHOICE.DAT, else the defaults.
            scr::ChoiceFile choice;
            if (kept) choice = *kept;
            else if (!choice.load(shared.choice_path))
                if (auto orig = scr::resolve_game_file(*game_dir, "CONFIG/CHOICE.DAT")) choice.load(*orig);
            shared.menu->load_choice(choice);
            shared.menu->set_volumes(shared.volumes);  // as main read them (or --volumes)
            shared.menu->set_music_midi(shared.music_midi);
            if (kept) {
                shared.menu->page = kept_page;
                shared.menu->page_stack = kept_stack;
                shared.menu->selected = kept_selected;
                shared.menu->race_options = kept_options;
            }
        }
        menu = &*shared.menu;
        menu->quit = false;
        menu->set_volumes(shared.volumes);  // (the race's P panel may have changed them,
        menu->set_music_midi(shared.music_midi);  // and its EJECT the music's source)
        menu->set_video(video_from(opt));  // OPTIONS > VIDEO as this run has it
        menu_video = menu->video();
        menu->set_rumble(shared.input.bindings.rumble);  // OPTIONS > CONTROLS
        // frontend_load, back from a race: the main page, nothing to go back to (Esc there
        // quits); a records page replay goes back to its page
        if (shared.race && shared.race->replay.empty()) {
            menu->page = 0;
            menu->page_stack.clear();
        }
        if (!shared.outcomes.empty() && shared.race) {
            for (const auto& o : shared.outcomes) {
                std::error_code ec;
                const bool saved = !o.replay.empty() && std::filesystem::exists(o.replay, ec);
                const int n = menu->race_result(shared.race->map, o.best_lap, o.total_time, shared.race->laps, o.finished, saved);
                if (n > 0) {
                    // menu_replay_files: temp.rpl becomes REPLAY<track><n>.RPL
                    const std::string to = shared.replay_dir + "REPLAY" + std::to_string(shared.race->map - 1) + std::to_string(n) + ".RPL";
                    std::filesystem::remove(to, ec);
                    std::filesystem::rename(o.replay, to, ec);
                }
                if (!o.replay.empty()) std::filesystem::remove(o.replay, ec);
            }
            const auto& o = shared.outcomes.back();
            menu->after_race(o.finished, o.failed, o.position, o.total_time);
        } else if (shared.race && !shared.race->replay.empty()) {
            menu->show_records();  // a replay from the records page: back to it
        } else if (shared.race && shared.race->game_mode == 2) {
            // a championship race left with Esc: it counts as lost
            menu->after_race(false, true, 10, 0);
        } else {
            // back from the intro demo (or the first time in): the title, then the main page
            // whatever page the menu was left on (show_title keeps the page itself)
            menu->show_title();
            menu->page = 0;
        }
        shared.outcomes.clear();
        shared.race.reset();
        save_menu();
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
        // the menu tune: CD track 4 or MUSIC3
        start_music(scr::kCdTrackMenu, scr::kMusicMenu);
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
    // (wide: the menus are laid out for 4:3 and stay that shape, with bars beside them)
    const int menu_width = wide ? std::min(opt.fb_width, int(std::lround(double(opt.fb_height) * 4.0 / 3.0))) : opt.fb_width;
    scr::Framebuffer fb = track ? scr::Framebuffer(opt.fb_width, opt.fb_height)
                          : menu ? scr::Framebuffer(menu_width, opt.fb_height) : scr::Framebuffer();
    fb.wide = wide && (track || menu);
    // The picture goes to the renderer as it is, one byte a pixel, with its palette (the
    // graphics card looks the colours up); renderers that cannot get it converted to RGBA,
    // and so does SDL's software renderer (headless runs), which scales RGBA much faster.
    if (opt.upload.empty() && std::strcmp(SDL_GetRendererName(renderer), "software") == 0) opt.upload = "rgba";

    // Widescreen menus: the backdrop, blurred and darker, stretched over the whole window
    // behind the 4:3 menu (in place of black bars)
    SDL_Texture* bars_tex = nullptr;
    const scr::PcxImage* bars_of = nullptr;
    auto make_bars = [&](const scr::PcxImage& bg) {
        if (bars_tex) SDL_DestroyTexture(bars_tex);
        bars_tex = nullptr;
        constexpr int kW = 64, kH = 40;
        if (bg.width <= 0 || bg.pixels.empty()) return;
        const int bh = int(bg.pixels.size() / size_t(bg.width));
        const scr::Palette pal = bg.palette();
        // averaged down, then blurred some more (the window's filter does the rest)
        std::vector<float> rgb(size_t(kW * kH * 3));
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                const int x0 = x * bg.width / kW, x1 = std::max(x0 + 1, (x + 1) * bg.width / kW);
                const int y0 = y * bh / kH, y1 = std::max(y0 + 1, (y + 1) * bh / kH);
                float r = 0, g = 0, b = 0;
                for (int sy = y0; sy < y1; ++sy)
                    for (int sx = x0; sx < x1; ++sx) {
                        const auto& c = pal[bg.pixels[size_t(sy) * size_t(bg.width) + size_t(sx)]];
                        r += c.r;
                        g += c.g;
                        b += c.b;
                    }
                const float n = float((x1 - x0) * (y1 - y0));
                float* o = &rgb[size_t(y * kW + x) * 3];
                o[0] = r / n;
                o[1] = g / n;
                o[2] = b / n;
            }
        for (int pass = 0; pass < 3; ++pass) {
            std::vector<float> t(rgb.size());
            for (int y = 0; y < kH; ++y)
                for (int x = 0; x < kW; ++x)
                    for (int ch = 0; ch < 3; ++ch) {
                        float s = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int sx = std::clamp(x + dx, 0, kW - 1), sy = std::clamp(y + dy, 0, kH - 1);
                                s += rgb[size_t(sy * kW + sx) * 3 + size_t(ch)];
                            }
                        t[size_t(y * kW + x) * 3 + size_t(ch)] = s / 9.0f;
                    }
            rgb.swap(t);
        }
        std::vector<uint8_t> px(size_t(kW * kH * 4));
        for (size_t i = 0; i < size_t(kW * kH); ++i) {
            for (size_t ch = 0; ch < 3; ++ch) px[i * 4 + ch] = uint8_t(std::clamp(rgb[i * 3 + ch] * 0.75f, 0.0f, 255.0f));
            px[i * 4 + 3] = 255;
        }
        bars_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, kW, kH);
        if (!bars_tex) return;
        SDL_UpdateTexture(bars_tex, nullptr, px.data(), kW * 4);
        SDL_SetTextureScaleMode(bars_tex, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(bars_tex, SDL_BLENDMODE_NONE);
    };
    SDL_Texture* tex = nullptr;
    std::vector<SDL_Texture*> halves;  // the picture halved, for showing it much smaller (see below)
    SDL_Palette* tex_palette = nullptr;
    std::array<SDL_Color, 256> tex_colors{};
    bool tex_colors_set = false;
    auto make_texture = [&]() {
        if (tex) SDL_DestroyTexture(tex);
        if (tex_palette) SDL_DestroyPalette(tex_palette);
        tex = nullptr;
        tex_palette = nullptr;
        tex_colors_set = false;
        // (and so does a picture larger than the window: it is filtered down, which takes colours)
        int out_w = 0, out_h = 0;
        SDL_GetRenderOutputSize(renderer, &out_w, &out_h);
        const bool larger = (!opt.headless || opt.hidden) && out_w > 0 && (fb.width() > out_w || fb.height() > out_h);
        if (opt.upload != "rgba" && !(opt.upload.empty() && larger)) {
            tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_INDEX8, SDL_TEXTUREACCESS_STREAMING, fb.width(), fb.height());
            if (tex) tex_palette = SDL_CreatePalette(256);
            if (!tex_palette || !SDL_SetTexturePalette(tex, tex_palette) || !SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE)) {
                if (tex) SDL_DestroyTexture(tex);
                if (tex_palette) SDL_DestroyPalette(tex_palette);
                tex = nullptr;
                tex_palette = nullptr;
            }
        }
        if (!tex) {
            tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, fb.width(), fb.height());
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);  // (every pixel is opaque)
        }
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    };
    make_texture();
    // OPTIONS > VIDEO in the menu, live: the menu's picture made again at the new size (from
    // the window's pixels for NATIVE and widescreen), and its texture
    auto resize_menu_picture = [&]() {
        const bool w = opt.aspect == "wide";
        if (!opt.fb_set && (w || opt.native_res)) {
            int ww = opt.width, wh = opt.height;
            if (!opt.headless) SDL_GetWindowSizeInPixels(window, &ww, &wh);
            if (ww > 0 && wh > 0) {
                if (!w) {
                    wh = std::min(wh, int(std::lround(double(ww) * 3.0 / 4.0)));
                    ww = int(std::lround(double(wh) * 4.0 / 3.0));
                }
                opt.fb_width = ww;
                opt.fb_height = wh;
            }
        }
        const int mw = w ? std::min(opt.fb_width, int(std::lround(double(opt.fb_height) * 4.0 / 3.0))) : opt.fb_width;
        if (mw == fb.width() && opt.fb_height == fb.height() && fb.wide == w) return;
        scr::Framebuffer next(mw, opt.fb_height);
        next.wide = w;
        fb = std::move(next);
        make_texture();
        SDL_Log("menu picture: %dx%d", fb.width(), fb.height());
    };

    uint64_t frame = 0;
    float end_timer = 0;
    float view_note_timer = 0;  // F9: "VIEW DISTANCE 2X" shown for a moment
    bool note_mipmaps = false;  // (F10: "MIPMAPS ON" instead)
    // Drawing between steps (interactive races): the state of the step before the last one.
    bool have_prev = false;
    scr::Camera prev_cam, step_cam;
    scr::Car prev_player, draw_player;
    std::vector<scr::Opponent> prev_opps, draw_opps;
    std::vector<scr::Obstacle> prev_obstacles;
    std::vector<scr::TrackAnimation> prev_anims;
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
    // The panel's volumes are OPTIONS > SOUND's (shared.volumes: heard at once, saved in
    // CHOICE.DAT). While it is up the race's sounds are silent and the music plays on (as in
    // the original, where the CD drive goes on playing).
    const float base_sfx = mixer.sfx_volume, base_music = mixer.music_volume;
    auto apply_volumes = [&]() {
        mixer.sfx_volume = base_sfx;
        mixer.music_volume = base_music;
    };
    // the panel's track numbers: the CD's audio tracks, or for the MIDI tunes MUSICn.HMP as CD
    // track n + 1 (the tunes are the CD's tracks 2.. in MIDI)
    auto panel_tracks = [&]() {
        if (cd_track && cd.first_track()) {
            panel.first_track = cd.first_track();
            panel.last_track = cd.last_track();
            panel.track = cd_track;
        } else if (midi_song && game_dir) {
            int last = midi_song;
            if (auto music_dat = scr::resolve_game_file(*game_dir, "DATA/MUSIC.DAT"))
                if (auto a = scr::Archive::open(*music_dat))
                    while (last < 99 && a->read("MUSIC" + std::to_string(last + 1) + ".HMP")) ++last;
            panel.first_track = 2;
            panel.last_track = last + 1;
            panel.track = midi_song + 1;
        }
    };
    // a track of the panel's, from the CD or as the MIDI tune with that number: true when it plays
    auto play_panel_track = [&](int t, bool from_cd) {
        if (from_cd) {
            if (!cd_open && game_dir) cd_open = cd.open(*game_dir);
            if (!cd_open) return false;
            auto music = cd.play(t);
            if (!music) return false;
            mixer.set_music(music);
            cd_track = t;
            midi_song = 0;
        } else {
            auto music_dat = game_dir ? scr::resolve_game_file(*game_dir, "DATA/MUSIC.DAT") : std::nullopt;
            auto song = music_dat ? scr::load_hmp_music(*music_dat, t - 1) : nullptr;
            if (!song) return false;
            mixer.set_music(song);
            midi_song = t - 1;
            cd_track = 0;
        }
        panel.track = t;
        SDL_Log("music: %s track %d", cd_track ? "CD" : "MIDI", t);
        return true;
    };
    auto save_volumes = [&]() {
        if (shared.choice_path.empty()) return;
        scr::ChoiceFile c;  // (our CHOICE.DAT, with the volumes changed)
        if (!c.load(shared.choice_path)) return;
        std::copy(shared.volumes.begin(), shared.volumes.end(), c.volume.begin());
        c.music_midi = shared.music_midi ? 1 : 0;
        c.save(shared.choice_path);
    };
    scr::Framebuffer frozen;
    auto start_prompt = [&]() {
        prompt = 0;
        frozen = fb;
        // three passes of draw_darkened_rect over the whole screen
        if (track) {
            scr::HudSurface hsurf(frozen);
            for (int i = 0; i < 3; ++i) hsurf.darken_all(track->darken075);
        }
    };
    int listener_x = player ? player->race.player.pos_x : 0, listener_z = player ? player->race.player.pos_z : 0;
    if (opt.audio_log)
        mixer.on_play = [&](int s) {
            SDL_Log("frame %llu: sound %d %s", static_cast<unsigned long long>(frame), s,
                    size_t(s) < sound_bank.fx.size() ? sound_bank.fx[size_t(s)].name.c_str() : "?");
        };
    bool running = true;
    // The camera's angles for the renderer, in 2048ths of a turn. The original keeps whole
    // ones, which a large picture shows as steps when the car turns slowly (a step is nearly
    // two pixels of a 1080-line picture, and the sky moved one column to two of them); ours
    // keeps the fraction, and the sky follows it. --smooth-turn off: as the original.
    cam.fine_sky = opt.smooth_turn;
    auto angle_units = [&](float radians) {
        const float u = radians * (1024.0f / 3.1415927410125732f);
        return opt.smooth_turn ? std::fmod(std::fmod(u, 2048.0f) + 2048.0f, 2048.0f) : float(int(u) & 0x7ff);
    };
    scr::SimClock clock;  // the original's frame clock: 100 Hz ticks, at least 2 per frame
    // F8: photo mode - the race stands still and goes quiet, the HUD goes away and a free
    // camera flies about (WASD, Q/E or Space/C up and down, mouse or arrow keys to look,
    // Shift faster, Ctrl slower). F8 or Esc puts everything back.
    bool freecam = false;
    bool rear_view = false;  // B (or the pad's right shoulder) held in a race: looking back (ours)
    float fc_x = 0, fc_y = 0, fc_z = 0, fc_yaw = 0, fc_pitch = 0;  // angles in 2048 units
    float fc_saved_sfx = 0, fc_saved_music = 0;
    int32_t fc_anim = 0;
    // Tab in photo mode: the edit mode (core/track_edit.h). The mouse picks corners (Shift adds,
    // Ctrl takes edges), the right button held looks about; M meets the two picked half way, J
    // puts the second onto the first, the arrows and PgUp/PgDn nudge (Shift 8 units, Ctrl a
    // quarter), Del puts back, Ctrl+Z / Ctrl+Y undo / redo, G / Shift+G the next open gap. Every
    // change is saved at once. Drawing only: the game's sums keep the track file's corners.
    bool editing = false, edit_look = false;
    std::unique_ptr<scr::TrackEditor> editor;
    scr::TrackEditor::Colors edit_colors;
    int edit_hover = -1, edit_hover_edge = -1;
    std::string edit_note;  // the last thing done, shown under the help
    auto set_editing = [&](bool on) {
        if (on && !editor && track) {
            editor = std::make_unique<scr::TrackEditor>(*track, weld_base, track_edits);
            edit_colors = scr::overlay_colors(track->palette);
        }
        editing = on && editor;
        edit_look = false;
        edit_hover = edit_hover_edge = -1;
        if (window && !opt.headless) SDL_SetWindowRelativeMouseMode(window, freecam && !editing);
        edit_note = !editing ? "" : track_edits_path.empty() ? "NOT SAVED: NO PLACE FOR THE FILE" : "SAVED TO " + track_edits_path;
    };
    auto edit_changed = [&](const char* what) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s  %zu GAPS LEFT", what, editor->gaps().size());
        edit_note = buf;
        if (!track_edits_path.empty()) {
            if (editor->edits().save(track_edits_path)) editor->mark_saved();
            else edit_note = "CANNOT SAVE " + track_edits_path;
        }
    };
    // window position (SDL's) -> framebuffer pixel
    auto mouse_to_fb = [&](float wx, float wy, float& fx, float& fy) {
        int ww = 1, wh = 1, ow = 1, oh = 1;
        if (window) SDL_GetWindowSize(window, &ww, &wh);
        if (renderer) SDL_GetRenderOutputSize(renderer, &ow, &oh);
        const float px = wx * float(ow) / float(std::max(ww, 1)), py = wy * float(oh) / float(std::max(wh, 1));
        const SDL_FRect dst = present_rect(ow, oh, opt.aspect, fb.width(), fb.height());
        fx = (px - dst.x) / std::max(dst.w, 1.0f) * float(fb.width());
        fy = (py - dst.y) / std::max(dst.h, 1.0f) * float(fb.height());
    };
    auto set_freecam = [&](bool on) {
        if (on == freecam) return;
        if (!on && editing) set_editing(false);
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
    uint64_t last_draw_ns = 0;  // when the last picture was drawn (--frame-limit)
    clock.add_ticks(scr::SimClock::kMinTicksPerFrame);  // draw frame 0 immediately
    int exit_code = 0;

    // --perf: the loop's time by section (core/profiler.h), reported every 5 seconds
    auto prof_last = std::chrono::steady_clock::now();
    auto prof_report_at = prof_last + std::chrono::seconds(5);
    auto mark = [&](scr::prof::Section sec) {
        const auto t = std::chrono::steady_clock::now();
        scr::prof::add(sec, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t - prof_last).count()));
        prof_last = t;
    };
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (input.handle_event(ev)) continue;  // controllers coming and going; the rebinding screen's keys
            switch (ev.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                // a menu drawn at the window's size (RES NATIVE, widescreen) follows it
                if (menu && !opt.fb_set) resize_menu_picture();
                break;
            case SDL_EVENT_KEY_DOWN:
                if (track && !menu && !ev.key.repeat && ev.key.key == SDLK_F9) {
                    // F9 / Shift+F9: the next / previous view distance (ours)
                    static constexpr float kSteps[] = {1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
                    constexpr int n = int(std::size(kSteps));
                    int i = 0;
                    for (int k = 1; k < n; ++k)
                        if (std::fabs(kSteps[k] - opt.view_distance) < std::fabs(kSteps[i] - opt.view_distance)) i = k;
                    i = (ev.key.mod & SDL_KMOD_SHIFT) ? (i + n - 1) % n : (i + 1) % n;
                    opt.view_distance = kSteps[i];
                    shared.saved_video.view_distance = opt.view_distance;  // (OPTIONS > VIDEO's, saved by the menu)
                    view_note_timer = 2.0f;
                    note_mipmaps = false;
                    break;
                }
                if (track && !menu && !ev.key.repeat && ev.key.key == SDLK_F10) {
                    // F10: squeezed textures from their smaller versions, or as they are (ours)
                    opt.mipmaps = !opt.mipmaps;
                    shared.saved_video.mipmaps = opt.mipmaps;
                    renderer3d.set_mipmaps(opt.mipmaps, opt.mip_sharpness);
                    view_note_timer = 2.0f;
                    note_mipmaps = true;
                    break;
                }
                if (freecam && track && ev.key.key == SDLK_TAB && !ev.key.repeat) {
                    set_editing(!editing);
                    break;
                }
                if (freecam && editing) {
                    const SDL_Keymod mod = ev.key.mod;
                    const bool ctrl = (mod & SDL_KMOD_CTRL) != 0, shift = (mod & SDL_KMOD_SHIFT) != 0;
                    const SDL_Keycode k = ev.key.key;
                    bool used = true;
                    if (k == SDLK_ESCAPE && !editor->selection().empty()) {
                        editor->clear_selection();
                    } else if (k == SDLK_M && !ev.key.repeat) {
                        if (editor->meet()) edit_changed("MET HALF WAY");
                    } else if (k == SDLK_J && !ev.key.repeat) {
                        if (editor->join()) edit_changed("JOINED");
                    } else if (k == SDLK_DELETE || k == SDLK_BACKSPACE) {
                        if (editor->reset_selected()) edit_changed("PUT BACK");
                    } else if (k == SDLK_Z && ctrl) {
                        if (shift ? editor->redo() : editor->undo()) edit_changed(shift ? "REDONE" : "UNDONE");
                    } else if (k == SDLK_Y && ctrl) {
                        if (editor->redo()) edit_changed("REDONE");
                    } else if (k == SDLK_G && !ev.key.repeat) {
                        std::array<float, 3> mid{};
                        if (editor->select_gap(shift ? -1 : 1, mid)) {
                            // the camera 700 units back from the gap, looking the way it looked
                            const float a = fc_yaw * 3.1415927f / 1024.0f, pch = fc_pitch * 3.1415927f / 1024.0f;
                            const float fx = std::cos(pch) * std::sin(a), fy = std::sin(pch), fz = -std::cos(a) * std::cos(pch);
                            fc_x = mid[0] - fx * 700.0f;
                            fc_y = mid[1] - fy * 700.0f;
                            fc_z = mid[2] - fz * 700.0f;
                            const auto pa = editor->position(editor->selection()[0][0]), pb = editor->position(editor->selection()[1][0]);
                            const double gap = std::sqrt(double((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) +
                                                                (pa[2] - pb[2]) * (pa[2] - pb[2])));
                            char buf[64];
                            std::snprintf(buf, sizeof buf, "GAP OF %.0f UNITS  %zu LEFT", gap, editor->gaps().size());
                            edit_note = buf;
                        } else {
                            edit_note = "NO OPEN GAPS";
                        }
                    } else if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
                        // on the ground away from the eye and across the screen, or up and down
                        const float step = shift ? 8.0f : ctrl ? 0.25f : 1.0f;
                        const float a = fc_yaw * 3.1415927f / 1024.0f;
                        const float fwd_x = std::sin(a), fwd_z = -std::cos(a), right_x = -std::cos(a), right_z = -std::sin(a);
                        float dx = 0, dy = 0, dz = 0;
                        if (k == SDLK_UP) { dx = fwd_x; dz = fwd_z; }
                        if (k == SDLK_DOWN) { dx = -fwd_x; dz = -fwd_z; }
                        if (k == SDLK_RIGHT) { dx = right_x; dz = right_z; }
                        if (k == SDLK_LEFT) { dx = -right_x; dz = -right_z; }
                        if (k == SDLK_PAGEUP) dy = 1;
                        if (k == SDLK_PAGEDOWN) dy = -1;
                        if (editor->nudge(dx * step, dy * step, dz * step)) edit_changed("NUDGED");
                    } else {
                        used = false;
                    }
                    if (used) break;
                }
                if (track && !menu && !paused && prompt < 0 && !ev.key.repeat &&
                    (ev.key.key == SDLK_F8 || (freecam && ev.key.key == SDLK_ESCAPE))) {
                    set_freecam(!freecam);
                    break;
                }
                if (freecam && ev.key.key != SDLK_F12) break;  // the race keys wait
                // frontend_text_input: letters typed in the menus (the cheats)
                if (menu && !ev.key.repeat && ev.key.key >= SDLK_A && ev.key.key <= SDLK_Z)
                    menu->type_letter(char('A' + int(ev.key.key - SDLK_A)));
                if (ev.key.key == SDLK_ESCAPE && !menu) {
                    if (paused) {
                        paused = false;  // Esc leaves the panel
                        apply_volumes();
                    } else if (replaying && !opt.instant_replay && prompt < 0) {
                        shared.back_to_menu = true;  // a replay from the records: back to them
                        running = false;
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
                else if (player && ev.key.key >= SDLK_F1 && ev.key.key <= SDLK_F4 && player->race.started && !replaying)
                    player->camera.select(int(ev.key.key - SDLK_F1));  // (camera_handle_view_keys: not in replays)
                else if (ev.key.key == SDLK_M && !ev.key.repeat) radar = radar >= 3 ? 0 : radar + 1;
                else if (ev.key.key == SDLK_P && !ev.key.repeat && player && hud && prompt < 0 && !paused) {
                    paused = true;
                    frozen = fb;
                    if (track) {
                        scr::HudSurface hsurf(frozen);
                        hsurf.darken_all(track->darken075);  // draw_darkened_rect
                    }
                    panel.button = 0;
                    panel_tracks();
                    mixer.sfx_volume = 0;  // (the music plays on)
                }
                else if (ev.key.key == SDLK_F12 && !ev.key.repeat) {
                    screenshot_pending = true;  // saved once the next frame is drawn
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (freecam && editing) {
                    const bool down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                    if (ev.button.button == SDL_BUTTON_RIGHT) {
                        // held: look about with the mouse
                        edit_look = down;
                        if (window && !opt.headless) SDL_SetWindowRelativeMouseMode(window, down);
                        float ignore_x, ignore_y;
                        SDL_GetRelativeMouseState(&ignore_x, &ignore_y);
                    } else if (ev.button.button == SDL_BUTTON_LEFT && down) {
                        const SDL_Keymod mod = SDL_GetModState();
                        const bool add = (mod & SDL_KMOD_SHIFT) != 0;
                        float mx, my;
                        mouse_to_fb(ev.button.x, ev.button.y, mx, my);
                        const float radius = float(fb.height()) / 40.0f;
                        if (mod & SDL_KMOD_CTRL) {
                            const int e = editor->pick_edge(cam, fb, &renderer3d, mx, my, radius);
                            if (e >= 0) editor->select({editor->edges()[size_t(e)].first, editor->edges()[size_t(e)].second}, add);
                            else if (!add) editor->clear_selection();
                        } else {
                            const int c = editor->pick_corner(cam, fb, &renderer3d, mx, my, radius);
                            if (c >= 0) editor->select({c}, add);
                            else if (!add) editor->clear_selection();
                        }
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                // the pad in a race: Start the car-stereo panel (P) and out of it again, Back
                // as Esc; in the panel and the question B goes back (Esc)
                if (!menu && player && !freecam) {
                    SDL_Keycode k = 0;
                    const int b = ev.gbutton.button;
                    if (b == SDL_GAMEPAD_BUTTON_START) k = paused ? SDLK_ESCAPE : prompt < 0 ? SDLK_P : 0;
                    else if (b == SDL_GAMEPAD_BUTTON_BACK) k = SDLK_ESCAPE;
                    else if (b == SDL_GAMEPAD_BUTTON_EAST && (paused || prompt >= 0)) k = SDLK_ESCAPE;
                    if (k) {
                        SDL_Event kev{};
                        kev.type = SDL_EVENT_KEY_DOWN;
                        kev.key.key = k;
                        kev.key.down = true;
                        SDL_PushEvent(&kev);
                    }
                }
                break;
            default: break;
            }
        }
        if (!running) break;
        mark(scr::prof::Events);

        // Headless runs advance exactly 2 ticks per loop, so output depends only on the
        // frame count. Interactive runs count real 10 ms ticks, like the timer interrupt.
        bool stepped = false;   // a picture was drawn
        bool catching_up = false;  // a step that is not drawn: the next one is already due
        if (opt.headless) {
            if (frame > 0 || opt.tween_test) clock.add_ticks(opt.tween_test ? 1 : scr::SimClock::kMinTicksPerFrame);
        } else {
            const uint64_t now = SDL_GetTicksNS();
            tick_acc += now - last;
            last = now;
            clock.add_ticks(static_cast<uint32_t>(tick_acc / scr::SimClock::kTickNs));
            tick_acc %= scr::SimClock::kTickNs;
        }
        // A game step when 2 ticks have passed; in between, interactive races draw the
        // picture again, blended towards the next step.
        const bool step = clock.frame_ready();
        const bool interactive = !opt.headless || opt.tween_test;
        // OPTIONS > VIDEO's FPS LIMIT (interactive runs): no picture before its time (the game
        // steps all the same)
        const uint64_t now_ns = SDL_GetTicksNS();
        const bool too_soon = !opt.headless && opt.frame_limit > 0 && now_ns - last_draw_ns < 1000000000ull / uint64_t(opt.frame_limit);
        const bool tween = !step && interactive && have_prev && track && player && !paused && prompt < 0 && !freecam && !too_soon;
        if (step || tween) {
            // Interactive runs: every step is exactly 2 ticks, so the game plays the same at any
            // frame rate. Behind (another step already due): this step is not drawn, the next
            // one is; more than half a second behind (a stall): the extra time is dropped.
            if (step && interactive && clock.pending_ticks() > 50) clock.drop_ticks(clock.pending_ticks() - 2);
            if (step) clock.begin_frame(interactive ? 2 : UINT32_MAX);
            const bool skip_draw = step && interactive && !opt.headless && (clock.frame_ready() || too_soon);
            if (!skip_draw) last_draw_ns = now_ns;
            catching_up = skip_draw;
            renderer3d.set_skip(skip_draw);
            // how far the display is between the last step and the next (1: at the last step)
            const float alpha = !interactive || !have_prev ? 1.0f
                : std::clamp(float(double(clock.pending_ticks() * scr::SimClock::kTickNs + tick_acc) /
                                   double(2 * scr::SimClock::kTickNs)), 0.0f, 1.0f);
            if (step && track && player) {
                prev_cam = cam;
                prev_player = player->race.player;
                prev_opps = player->race.opponents;
                prev_obstacles = obstacles.list;
                prev_anims = anims.list;
                have_prev = true;
            }
            if (step && player && hud && !paused && frame == 150 && opt.hold.find("pause") != std::string::npos) {
                // --hold pause: open the P panel at frame 150 (headless checks)
                paused = true;
                panel_tracks();
                mixer.sfx_volume = 0;
                frozen = fb;
                scr::HudSurface hsurf(frozen);
                hsurf.darken_all(track->darken075);
            }
            if (prompt >= 0) {
                fb = frozen;
                const bool* keys = SDL_GetKeyboardState(nullptr);
                const auto pn = input.pad_nav();  // (B answers as Esc, an event)
                const bool now[4] = {keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_LEFT] || pn.up || pn.left,
                                     keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_RIGHT] || pn.down || pn.right,
                                     keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER] || pn.enter ||
                                         opt.hold.find("promptno") != std::string::npos,  // headless checks: answer no
                                     false};
                if (opt.hold.find("promptno") != std::string::npos) prompt = 1;
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
                    scr::HudSurface hsurf(fb, hud->scale);
                    auto centre = [&](const std::string& t, int y, int m) {
                        hsurf.text(hud->fonts, (hsurf.width() - hsurf.measure(hud->fonts, t, 0)) >> 1, y * hud->scale, t, 0, m);
                    };
                    const bool blink = (frame & 8) != 0;
                    centre(hud->texts[19], 0x34, 2);
                    centre(hud->texts[20], 0x5c, prompt == 0 && blink ? 2 : 0xe);
                    centre(hud->texts[21], 0x7a, prompt == 1 && blink ? 2 : 0xe);
                }
            } else if (paused) {
                fb = frozen;
                // --hold pause: headless looks at the panel (pausevol: the volume bar, after
                // pressing previous, volume -, mode, volume - and eject)
                const bool scripted = opt.hold.find("pause") != std::string::npos;
                bool script_enter = false;
                if (scripted && opt.hold.find("pausevol") != std::string::npos) {
                    const uint64_t f = frame - 150;
                    if (f == 2) panel.button = 2;
                    if (f == 4) panel.button = 4;
                    if (f == 8) panel.button = 6;
                    if (f == 12) panel.button = 4;
                    if (f == 16) panel.button = 1;  // eject: the other source
                    script_enter = f == 3 || f == 5 || f == 9 || f == 13 || f == 17;
                } else if (scripted) {
                    panel.button = 4;
                }
                const bool* keys = SDL_GetKeyboardState(nullptr);
                const auto pn = input.pad_nav();  // (B and Start leave it, as Esc: an event)
                const bool now[3] = {keys[SDL_SCANCODE_LEFT] || pn.left, keys[SDL_SCANCODE_RIGHT] || pn.right,
                                     keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER] || pn.enter || script_enter};
                if (now[0] && !panel_keys[0] && panel.button > 0) --panel.button;
                if (now[1] && !panel_keys[1] && panel.button < 6) ++panel.button;
                panel.pressed = now[2];
                for (size_t i = 0; i < 4; ++i) panel.volume[i] = shared.volumes[i];
                panel.volume_max = scr::kVolumeMax;
                if (now[2] && !panel_keys[2]) {
                    if (panel.button == 0) {
                        paused = false;  // play: back to the race
                        apply_volumes();
                    } else if (panel.button == 1) {
                        // eject (ours): the same track from the other source, CD <-> MIDI, as
                        // OPTIONS > SOUND's CD MUSIC / MIDI MUSIC (saved); off is MUSIC 0
                        const bool to_cd = !cd_track;
                        if ((cd_track || midi_song) && play_panel_track(panel.track, to_cd)) {
                            shared.music_midi = !to_cd;
                            panel_tracks();
                            save_volumes();
                        }
                    } else if (panel.button == 2 || panel.button == 3) {
                        // previous / next track, CD or MIDI, playing at once
                        // (past a track that will not play, such as one missing from the MUSIC folder)
                        const int dir = panel.button == 3 ? 1 : -1;
                        for (int t = panel.track + dir; t >= panel.first_track && t <= panel.last_track; t += dir)
                            if (!(cd_track || midi_song) || play_panel_track(t, cd_track != 0)) break;
                    } else if (panel.button == 4 || panel.button == 5) {
                        // a step of the volume the mode shows, as OPTIONS > SOUND (saved)
                        int& v = shared.volumes[size_t(std::clamp(panel.mode, 0, 3))];
                        v = std::clamp(v + (panel.button == 5 ? 1 : -1), 0, scr::kVolumeMax);
                        apply_group_volumes();
                        save_volumes();
                        for (size_t i = 0; i < 4; ++i) panel.volume[i] = shared.volumes[i];
                    } else if (panel.button == 6) {
                        panel.mode = (panel.mode + 1) % 4;
                    }
                }
                std::copy(now, now + 3, panel_keys);
                panel.blink = (frame & 0x10) != 0;
                if (paused) hud->draw_cd_panel(fb, panel);
            } else if (track) {
                fb.palette() = track->palette;
                renderer3d.set_anim_counter(freecam ? fc_anim : clock.anim_counter());
                if (step && !freecam && player && frame == 150 && opt.hold.find("freecam") != std::string::npos)
                    set_freecam(true);  // --hold freecam: photo mode at frame 150, flying forward (headless checks)
                if (step && !freecam && player && frame == 150 && opt.hold.find("edit") != std::string::npos) {
                    // --hold edit: photo mode's edit mode at frame 150, looking down at the widest open gap
                    set_freecam(true);
                    set_editing(true);
                    std::array<float, 3> mid{};
                    if (editor && editor->select_gap(1, mid)) {
                        fc_pitch = -440.0f;
                        const float a = fc_yaw * 3.1415927f / 1024.0f, pch = fc_pitch * 3.1415927f / 1024.0f;
                        fc_x = mid[0] - std::cos(pch) * std::sin(a) * 700.0f;
                        fc_y = mid[1] - std::sin(pch) * 700.0f;
                        fc_z = mid[2] + std::cos(a) * std::cos(pch) * 700.0f;
                        if (opt.hold.find("editmeet") != std::string::npos && editor->meet()) edit_changed("MET HALF WAY");
                    }
                }
                if (freecam) {
                    // photo mode: move the free camera, nothing else moves
                    const bool* keys = SDL_GetKeyboardState(nullptr);
                    const float dt = clock.frame_time();
                    float mx = 0, my = 0;
                    SDL_GetRelativeMouseState(&mx, &my);
                    if (opt.headless || (editing && !edit_look)) mx = my = 0;  // (edit mode: the mouse points)
                    const float turn = 600.0f * dt;  // 2048 units per turn
                    fc_yaw -= mx * 1.5f;
                    fc_pitch -= my * 1.5f;
                    if (!editing) {  // (edit mode: the arrows nudge)
                        if (keys[SDL_SCANCODE_LEFT]) fc_yaw += turn;
                        if (keys[SDL_SCANCODE_RIGHT]) fc_yaw -= turn;
                        if (keys[SDL_SCANCODE_UP]) fc_pitch += turn;
                        if (keys[SDL_SCANCODE_DOWN]) fc_pitch -= turn;
                    }
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
                } else if (!step) {
                    cam = step_cam;  // (drawing between steps: nothing moves)
                } else if (player && opt.attract) {
                    // attract_mode_update_cars: the CPU cars, the TV camera, the credits; space,
                    // Enter, Esc or the throttle fade it out (handle_global_keys)
                    const bool* keys = SDL_GetKeyboardState(nullptr);
                    bool leave = keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER] ||
                                 keys[SDL_SCANCODE_ESCAPE] || keys[SDL_SCANCODE_UP] ||
                                 (frame >= 300 && opt.hold.find("accel") != std::string::npos);  // headless checks
                    if (input.any_pad_button()) leave = true;
                    if (leave) attract.leave();
                    player->race.update_attract(*track, clock.frame_time(), clock.pow_table_row(), tv_camera.x, tv_camera.z);
                    anims.update(tv_camera.x, tv_camera.z, clock.frame_time());
                    tv_camera.update(player->race.opponents, 2);
                    attract.update(clock.frame_time(), clock.pow_table_row());
                    if (attract.done()) {
                        shared.attract_done = true;
                        running = false;
                    }
                    cam.center_y = 130.0f;
                    cam.x = float(tv_camera.x);
                    cam.y = float(tv_camera.y);
                    cam.z = float(tv_camera.z);
                    cam.pitch = angle_units(tv_camera.pitch);
                    cam.yaw = angle_units(tv_camera.yaw);
                    cam.roll = 0;
                } else if (player) {
                    // the controls as the original's g_input_* and control setup (input.h)
                    scr::CarInput in = input.drive(opt.hold);
                    auto held = [&](const char* k) { return opt.hold.find(k) != std::string::npos; };
                    float ft = clock.frame_time();
                    int pow_row = clock.pow_table_row();
                    if (positions) {
                        // the same fields as step_race.py's COMPACT records, before this frame
                        const scr::Car& pc = player->race.player;
                        int32_t rec[9 + 1 + 27]{};
                        rec[0] = pc.pos_x; rec[1] = pc.pos_y; rec[2] = pc.pos_z;
                        std::memcpy(&rec[6], &pc.pitch, 4); std::memcpy(&rec[7], &pc.yaw, 4); std::memcpy(&rec[8], &pc.roll, 4);
                        std::memcpy(&rec[9], &pc.speed, 4);
                        for (size_t k = 0; k < 9 && k < player->race.opponents.size(); ++k) {
                            const scr::Opponent& o = player->race.opponents[k];
                            rec[10 + 3 * k] = o.pos_x; rec[11 + 3 * k] = o.pos_y; rec[12 + 3 * k] = o.pos_z;
                        }
                        std::fwrite(rec, sizeof rec, 1, positions);
                    }
                    if (replaying) {
                        // replay_read_frame: the recorded frame time and inputs
                        if (replay_pos < replay.frames.size()) {
                            const scr::ReplayFrame& rf = replay.frames[replay_pos++];
                            in = scr::CarInput{};
                            replay.read_frame(rf, in);  // (with the replay's control setup)
                            // (as recorded; only nonsense is kept out)
                            ft = rf.frame_time > 0.0f && rf.frame_time < 1.0f ? rf.frame_time : 0.02f;
                            pow_row = int(1.0 / double(ft));
                            player->race.raw_frame_time = rf.raw_time > 0.0f && rf.raw_time < 1.0f ? rf.raw_time : ft;
                        } else if (prompt < 0) {
                            // the end of the replay (g_race_flags 0x220)
                            if (opt.instant_replay) {
                                start_prompt();
                            } else {
                                shared.back_to_menu = true;
                                running = false;
                            }
                        }
                    } else if ((opt.from_menu || !opt.record_file.empty()) && (opt.game_mode & 3) && replay.frames.size() < 400000) {
                        // replay_write_frame
                        replay.add_frame(in, ft, ft);
                    }
                    in.mirror = (opt.race_options & 0x10) != 0;  // MIRRO: car_update_steering turns the steering round
                    // camera_update_view: only camera_mode_car (F3, F4) wraps the car's yaw;
                    // a replay is seen from the trackside cameras
                    player->race.player.camera_car_mode = !replaying && (player->camera.view == 2 || player->camera.view == 3);
                    if (player->cpu_cars)
                        player->race.update(*track, in, ft, pow_row);
                    else {
                        player->race.player.update(*track, in, ft, pow_row);
                        player->race.player.camera_update();
                    }
                    if (frame == 700 && held("finish") && !replaying) player->race.finished = true;  // headless checks of the race end
                    race_audio.update(player->race, clock.frame_time());
                    // rumble (an option): from what the car did; nothing goes back into the race
                    input.rumble(player->race.player, !replaying && player->race.started && !player->race.finished && !player->race.failed);
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
                            ghost.recording.push_back({pc.pos_x, pc.pos_y + pc.body_ofs_y, pc.pos_z, pc.pitch + pc.body_pitch_ofs,
                                                       pc.yaw + pc.drift_angle, pc.roll + pc.body_roll_ofs,
                                                       int32_t(pc.render_animation()), pc.surface_render_flag,
                                                       pc.track_progress, rr.lap_time});
                        }
                        if (rr.finished && !rr.failed && rr.mode == 4 && !ghost.recording.empty() &&
                            (ghost.lap <= 0 || rr.best_lap < ghost.lap)) {
                            // race_run's attack_save: the new best lap becomes the ghost
                            ghost.save(rr.best_lap, opt.race_options, player->car_index);
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
                    cam.x = float(player->camera.x);
                    cam.y = float(player->camera.y);
                    cam.z = float(player->camera.z);
                    cam.pitch = angle_units(player->camera.pitch);
                    cam.yaw = angle_units(player->camera.yaw);
                    cam.roll = angle_units(player->camera.roll);
                    if (replaying && tv_camera.has_trackside()) {
                        // camera_mode_trackside: the nearest replay camera, aimed at the car
                        const scr::Car& pc = player->race.player;
                        tv_camera.trackside(pc.pos_x, pc.pos_y, pc.pos_z);
                        cam.center_y = 130.0f;
                        cam.x = float(tv_camera.x);
                        cam.y = float(tv_camera.y);
                        cam.z = float(tv_camera.z);
                        cam.pitch = angle_units(tv_camera.pitch);
                        cam.yaw = angle_units(tv_camera.yaw);
                        cam.roll = 0;
                        player->camera.car_visible = true;
                    }
                }
                if (step) step_cam = cam;
                // obstacles_update (game state, once a step)
                if (step && !freecam) obstacles.update(*track, clock.frame_time());
                if (step && !player && !freecam) anims.update(int(cam.x), int(cam.z), clock.frame_time());
                mark(scr::prof::Step);
                // what is drawn: between the previous step and this one
                const bool blend = player && have_prev && alpha < 1.0f && !freecam;
                const scr::Camera draw_cam = blend ? tween_camera(prev_cam, step_cam, alpha) : cam;
                if (player) {
                    if (blend) {
                        tween_car(draw_player, prev_player, player->race.player, alpha);
                        draw_opps.resize(player->race.opponents.size());
                        for (size_t i = 0; i < draw_opps.size(); ++i) {
                            if (i < prev_opps.size()) tween_opponent(draw_opps[i], prev_opps[i], player->race.opponents[i], alpha);
                            else draw_opps[i] = player->race.opponents[i];
                        }
                    } else {
                        draw_player = player->race.player;
                        draw_opps = player->race.opponents;
                    }
                }
                mark(scr::prof::Tween);
                const auto perf_t0 = std::chrono::steady_clock::now();
                scr::Camera view_cam = draw_cam;
                // Looking back (ours; the picture only, the race does not know): while B or the
                // pad's right shoulder, or their bindings) is held the camera faces the other way. In the chase
                // views it also goes to the other side of the car, so the car is seen from the
                // front with what follows it behind.
                rear_view = false;
                if (player && !freecam && !opt.attract && !(replaying && tv_camera.has_trackside())) {
                    rear_view = input.look_back() || opt.hold.find("rear") != std::string::npos;
                }
                if (rear_view) {
                    if (player->camera.car_visible) {
                        view_cam.x = 2.0f * float(draw_player.pos_x) - view_cam.x;
                        view_cam.z = 2.0f * float(draw_player.pos_z) - view_cam.z;
                    }
                    view_cam.yaw = std::fmod(view_cam.yaw + 1024.0f, 2048.0f);
                    view_cam.roll = std::fmod(2048.0f - view_cam.roll, 2048.0f);
                }
                if (opt.view_distance > 0.0f) {
                    // the view distance (ours): the original's far clip (HI detail) times the
                    // setting; the last quarter fades out in an ordered dither
                    view_cam.far_depth = 43808.0f * opt.view_distance;
                    view_cam.fade_depth = view_cam.far_depth * 0.75f;
                }
                renderer3d.begin(*track, view_cam, fb, 0, true);
                renderer3d.draw_placements();
                for (size_t oi = 0; oi < obstacles.list.size(); ++oi) {
                    const scr::Obstacle& o = obstacles.list[oi];
                    if (o.y == -20000 || size_t(o.model) >= obstacles.models.size()) continue;
                    constexpr float kObsUnits = 1024.0f / 3.1415927410125732f;
                    scr::Obstacle ob = o;
                    if (blend && oi < prev_obstacles.size() && prev_obstacles[oi].y != -20000) {
                        const scr::Obstacle& q = prev_obstacles[oi];
                        ob.x = tween_int(q.x, o.x, alpha);
                        ob.y = tween_int(q.y, o.y, alpha);
                        ob.z = tween_int(q.z, o.z, alpha);
                        ob.ax = tween_angle(q.ax, o.ax, alpha, kTwoPiF);
                        ob.ay = tween_angle(q.ay, o.ay, alpha, kTwoPiF);
                        ob.az = tween_angle(q.az, o.az, alpha, kTwoPiF);
                    }
                    scr::Pose op;
                    op.x = float(ob.x);
                    op.y = float(ob.y);
                    // (ours) The models are centred on their middle and the original puts that
                    // at the ground, so the base is below the road and seems to slide along it
                    // as the camera comes nearer. Drawn raised by what the model has below its
                    // middle, the base is on the road. The picture only: the game's obstacle
                    // stays where it is.
                    if (opt.ground_obstacles) {
                        float lowest = 0;
                        for (const scr::Part& part : obstacles.models[size_t(o.model)].parts)
                            for (const scr::Vec3i16& v : part.vertices) lowest = std::min(lowest, float(v.y) + float(part.offset[1]));
                        op.y -= lowest;
                    }
                    // half of each obstacle is below the ground (it stands at the ground height);
                    // the original draws it over the ground all the same (painter's order). Seen
                    // at a shallow angle the ground in front of its lower half is much nearer
                    // than the obstacle: by its half height times distance over the camera's
                    // height above it. That much bias, for the test only, so that cars in front
                    // of the obstacle still cover it.
                    if (opt.ground_obstacles) {
                        // (standing on the road there is nothing to draw over: the plain depth
                        // test, with a little in hand for the base where the road slopes, so
                        // obstacles hide one another and a crest of the road hides them)
                        op.depth_bias = 24.0f;
                    } else {
                        const scr::Model& om = obstacles.models[size_t(o.model)];
                        const float dx = float(ob.x) - view_cam.x, dz = float(ob.z) - view_cam.z;
                        const float dist = std::sqrt(dx * dx + dz * dz);
                        const float height = std::max(40.0f, std::fabs(view_cam.y - float(ob.y)));
                        const float half = om.radius > 0 ? om.radius : 128.0f;
                        op.depth_bias = std::min(std::max(kObstacleBias, half * dist / height * 1.25f + 64.0f), dist * 0.9f);
                        op.bias_test_only = true;
                    }
                    op.z = float(ob.z);
                    op.ax = float(int(ob.ax * kObsUnits) & 0x7ff);
                    op.ay = float(int(ob.ay * kObsUnits) & 0x7ff);
                    op.az = float(int(ob.az * kObsUnits) & 0x7ff);
                    renderer3d.draw_model(obstacles.models[size_t(o.model)], op);
                }
                // The moving scenery (instance angles: yaw only).
                constexpr float kAnimToUnits = 1024.0f / 3.1415927410125732f;
                for (size_t i = 0; i < anims.list.size() && i < anim_models.size(); ++i) {
                    const scr::TrackAnimation& an = anims.list[i];
                    scr::Pose ap;
                    ap.x = float(an.x);
                    ap.y = float(an.y);
                    ap.z = float(an.z);
                    float an_yaw = an.yaw;
                    if (blend && i < prev_anims.size() &&
                        std::abs(prev_anims[i].x - an.x) + std::abs(prev_anims[i].z - an.z) < 8000) {
                        const scr::TrackAnimation& q = prev_anims[i];
                        ap.x = float(tween_int(q.x, an.x, alpha));
                        ap.y = float(tween_int(q.y, an.y, alpha));
                        ap.z = float(tween_int(q.z, an.z, alpha));
                        an_yaw = tween_angle(q.yaw, an.yaw, alpha, kTwoPiF);
                    }
                    ap.ay = float(int(an_yaw * kAnimToUnits) & 0x7ff);
                    renderer3d.draw_model(anim_models[i], ap);
                }
                if (player) {
                    // car_sync_render_object: body at the car, angles in 2048 units; the
                    // shadow sits on the ground just below.
                    const scr::Car& c = draw_player;
                    constexpr float kToUnits = 1024.0f / 3.1415927410125732f;
                    scr::Pose pose;
                    pose.x = float(c.pos_x);
                    // car_sync_render_object: the crash bounce and tumble (body_ofs_y, body_*_ofs) on
                    // top of the car's own position and angles
                    pose.y = float(c.pos_y + c.body_ofs_y);
                    pose.z = float(c.pos_z);
                    pose.ax = angle_units((c.pitch + c.body_pitch_ofs));
                    pose.ay = angle_units((c.yaw + c.drift_angle));
                    pose.az = angle_units((c.roll + c.body_roll_ofs));
                    pose.animation = int32_t(c.render_animation());
                    pose.reflection = c.surface_render_flag;
                    // car_apply_surface sets the (global) scroll rate from the player's surface.
                    const float env_rate = c.marker_flags == 0 ? 0.002f : 0.03f;
                    pose.env = &player->env;
                    pose.dynamic = true;
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
                    // the shadow keeps its angles while the car tumbles (crash state 1)
                    if (c.crash_state != 1) player->shadow_angles = {pose.ax, pose.az};
                    sh.ax = player->shadow_angles[0];
                    sh.az = player->shadow_angles[1];
                    sh.env = nullptr;
                    sh.depth_bias = kShadowBias;
                    // the tyre smoke (car_sync_debris_objects: instance position and animation word)
                    if (smoke_model && !opt.attract)
                        for (const scr::SmokePuff& sp : c.smoke) {
                            if (!sp.on) continue;
                            scr::Pose spose;
                            spose.x = float(sp.x);
                            spose.y = float(sp.y);
                            spose.z = float(sp.z);
                            spose.animation = sp.animation;
                            spose.depth_bias = kShadowBias;  // on the road, not in it
                            spose.billboard = true;  // always facing the camera (ours)
                            renderer3d.draw_model(*smoke_model, spose);
                        }
                    // the car's part table (lean, suspension, steering, wheel roll)
                    player->race.player.part_poses(player->model, player->model_parts, player->part_poses, step && !freecam);
                    pose.parts = &player->part_poses;
                    if ((player->camera.car_visible || freecam) && !opt.attract) {
                        if (shadow) renderer3d.draw_model(*shadow, sh);
                        renderer3d.draw_model(player->model, pose);
                    }
                    // ghost_sync_render_object: where the ghost was at this lap time
                    if (player->race.mode == 4 && ghost.lap > 0 && !ghost.frames.empty() && player->race.timing_started) {
                        // ghost_update: the first frame at or after this lap time, drawn from the
                        // one before towards it
                        const float t = player->race.lap_time;
                        size_t gi = 0;
                        while (gi + 1 < ghost.frames.size() && ghost.frames[gi].time < t) ++gi;
                        const GhostFrame& g1 = ghost.frames[gi];
                        const GhostFrame& g0 = ghost.frames[gi > 0 ? gi - 1 : 0];
                        const float span = g1.time - g0.time;
                        const float f = span > 0 ? std::clamp((t - g0.time) / span, 0.0f, 1.0f) : 1.0f;
                        auto mix = [&](float a, float b) { return a + (b - a) * f; };
                        auto mix_angle = [&](float a, float b) {
                            float d = b - a;
                            while (d > 3.14159265f) d -= 6.2831853f;
                            while (d < -3.14159265f) d += 6.2831853f;
                            return a + d * f;
                        };
                        scr::Pose gp;
                        gp.x = mix(float(g0.x), float(g1.x));
                        gp.y = mix(float(g0.y), float(g1.y));
                        gp.z = mix(float(g0.z), float(g1.z));
                        gp.ax = angle_units(mix_angle(g0.pitch, g1.pitch));
                        gp.ay = angle_units(mix_angle(g0.yaw, g1.yaw));
                        gp.az = angle_units(mix_angle(g0.roll, g1.roll));
                        gp.animation = g0.animation;
                        gp.reflection = g0.reflection;
                        gp.dynamic = true;
                        renderer3d.draw_model(ghost_model.parts.empty() ? player->model : ghost_model, gp);
                    }
                    // opponent_sync_render_object
                    for (size_t i = 0; i < player->race.opponents.size(); ++i) {
                        const scr::Opponent& o = draw_opps[i];
                        scr::Pose op;
                        op.x = float(o.pos_x);
                        op.y = float(o.pos_y);
                        op.z = float(o.pos_z);
                        op.ax = angle_units(o.pitch);
                        op.ay = angle_units(o.draw_yaw);
                        op.az = angle_units(o.roll);
                        op.animation = int32_t(uint32_t(o.setup.animation) | (o.catching_up ? 0u : 0x4000u) |
                                               (o.speed <= 4000.0f ? 0x2000u : 0u));
                        op.reflection = int32_t(o.render_flags);
                        if (player->cpu_env.size() < player->race.opponents.size())
                            player->cpu_env.resize(player->race.opponents.size());
                        op.env = &player->cpu_env[i];
                        op.dynamic = true;
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
                        // opponent_sync_render_object's part table: the body sections lean
                        // (pitch_ofs, roll_ofs), every wheel rolls by speed / 15 a frame
                        // (all at once: the renderer keeps pointers to the poses until the
                        // frame is drawn, so the lists must not grow while the cars are drawn)
                        const size_t cpu_count = player->race.opponents.size();
                        if (player->cpu_parts.size() < cpu_count) player->cpu_parts.resize(cpu_count);
                        if (player->cpu_roll.size() < cpu_count) player->cpu_roll.resize(cpu_count, 0);
                        if (player->cpu_poses.size() < cpu_count) player->cpu_poses.resize(cpu_count);
                        {
                            const scr::Model& cm = player->cpu_models[i];
                            if (player->cpu_parts[i].body.empty() && player->cpu_parts[i].wheels.empty())
                                player->cpu_parts[i] = scr::find_car_parts(cm);
                            const scr::CarParts& cp = player->cpu_parts[i];
                            auto& pp = player->cpu_poses[i];
                            pp.assign(cm.parts.size(), scr::PartPose{});
                            const int32_t lx = int(o.pitch_ofs * kToUnits) & 0x7ff, lz = int(o.roll_ofs * kToUnits) & 0x7ff;
                            for (int pi2 : cp.body) {
                                pp[size_t(pi2)].angles = true;
                                pp[size_t(pi2)].ax = lx;
                                pp[size_t(pi2)].az = lz;
                            }
                            if (step && !freecam) player->cpu_roll[i] = int32_t(double(player->cpu_roll[i]) - double(o.speed) / 15.0) & 0x7fe;
                            for (int pi2 : cp.wheels) {  // (the hubs roll, the tyres do not)
                                pp[size_t(pi2)].angles = true;
                                pp[size_t(pi2)].ax = cm.parts[size_t(pi2)].name.back() == 'x' ? player->cpu_roll[i] : 0;
                            }
                            op.parts = &pp;
                        }
                        renderer3d.draw_model(player->cpu_models[i], op);
                    }
                }
                for (const auto& c : cars) {
                    if (shadow) renderer3d.draw_model(*shadow, c.pose);
                    renderer3d.draw_model(c.model, c.pose);
                }
                renderer3d.flush_geometry();
                mark(scr::prof::Geometry);
                renderer3d.finish();
                mark(scr::prof::Fill);
                // MIRRO / UPDOW: race_frame flips the 3D picture (not the HUD) after drawing it
                if (opt.race_options & 0x30) {
                    const int fw = fb.width(), fh = fb.height();
                    if (opt.race_options & 0x10)
                        for (int y = 0; y < fh; ++y) std::reverse(&fb.at(0, y), &fb.at(0, y) + fw);
                    if (opt.race_options & 0x20)
                        for (int y = 0; y < fh / 2; ++y) std::swap_ranges(&fb.at(0, y), &fb.at(0, y) + fw, &fb.at(0, fh - 1 - y));
                }
                {
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - perf_t0).count();
                    perf_total += ms;
                    perf_max = std::max(perf_max, ms);
                    ++perf_frames;
                }
                if (opt.attract) {
                    // attract_mode_overlay, and attract_mode_fade_out's palette
                    attract.draw(fb);
                    const float b = attract.brightness();
                    if (b < 1.0f)
                        for (scr::Rgb& c : fb.palette()) {
                            c.r = uint8_t(float(c.r) * b);
                            c.g = uint8_t(float(c.g) * b);
                            c.b = uint8_t(float(c.b) * b);
                        }
                }
                if (player && player->cpu_cars && hud && !freecam && !opt.attract) {
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
                    if (step && hs.end && opt.from_menu) {
                        // race_end_screen: back to the menu five seconds after the end
                        end_timer += clock.frame_time();
                        if (5.0f <= end_timer && (race_audio.speech_current() == -1 || opt.headless) && prompt < 0 && !replaying) {
                            shared.outcomes.push_back(
                                Shared::Outcome{r.best_lap, r.total_time, r.finished, r.failed, r.player_position, {}});
                            // race_run: replay_save("temp") after a race that was not left or
                            // lost, then (single races) its replay at once. Each race of a
                            // PLAY AGAIN run keeps its own temp file until the menu files it.
                            const bool keep = r.finished && !r.failed && !shared.replay_dir.empty() && !replay.frames.empty();
                            if (keep) {
                                replay.car = player->car_index;
                                replay.laps = opt.laps;
                                replay.level = opt.level;
                                replay.game_mode = opt.game_mode;
                                replay.race_options = opt.race_options | (opt.reversed ? scr::Replay::kReversed : 0);
                                std::error_code ec;
                                std::filesystem::create_directories(shared.replay_dir, ec);
                                const std::string path = shared.replay_dir + "temp" + std::to_string(shared.outcomes.size()) + ".rpl";
                                replay.finish_recording(input.replay_controls());
                                if (replay.save(path)) shared.outcomes.back().replay = path;
                            }
                            if (keep && opt.game_mode == 1 && !shared.outcomes.back().replay.empty()) {
                                shared.play_replay = true;
                                running = false;
                            } else if (shared.race && shared.race->game_mode == 2) {
                                // the championship goes on in the menu
                                shared.back_to_menu = true;
                                running = false;
                            } else {
                                start_prompt();
                            }
                        }
                    }
                    hs.won = r.finished && r.player_position == 1;
                    hs.mode = r.mode;
                    if (r.mode == 4) {
                        // race_result_time_attack: beaten the ghost (or no ghost to beat)
                        hs.won = r.finished && (ghost.lap <= 0 || r.total_time < ghost.lap);
                    } else if (r.mode == 0x20) {
                        // race_result_carnage: every cone
                        hs.won = r.finished;
                        hs.perfect = !obstacles.list.empty() && obstacles.hits == int(obstacles.list.size());
                    } else if (r.mode == 0x40) {
                        // race_result_checkpoints: every gate and no cone
                        hs.won = r.finished;
                        hs.perfect = r.markers_passed == int(r.gates.size()) && obstacles.hits == 0;
                    }
                    hs.player_x = c.pos_x;
                    hs.player_z = c.pos_z;
                    hs.player_yaw = c.yaw;
                    for (const scr::Opponent& o : r.opponents) hs.cpu_cars.push_back({o.pos_x, o.pos_z});
                    // draw_map_objects: the cross-sections of the 17 x 17 cells around the player.
                    for (const scr::TrackSection& ts : track->sections)
                        if (std::abs(ts.x - c.pos_x) < 8 * 8192 + 4096 && std::abs(ts.z - c.pos_z) < 8 * 8192 + 4096)
                            hs.radar_lines.push_back({ts.left_x, ts.left_z, ts.right_x, ts.right_z});
                    hud->draw(fb, hs, track->darken075, clock.frame_time(), clock.pow_table_row(), rear_view && player->camera.hud_view == 1 ? 2 : player->camera.hud_view,
                              radar, step);
                }
                if (freecam && editing && editor) {
                    // the edit mode's marks: what the mouse is over (Ctrl: edges), the picked, the gaps
                    if (!opt.headless && window) {
                        float wx, wy, mx, my;
                        SDL_GetMouseState(&wx, &wy);
                        mouse_to_fb(wx, wy, mx, my);
                        const float radius = float(fb.height()) / 40.0f;
                        const bool edges = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
                        edit_hover = edit_look || edges ? -1 : editor->pick_corner(cam, fb, &renderer3d, mx, my, radius);
                        edit_hover_edge = edit_look || !edges ? -1 : editor->pick_edge(cam, fb, &renderer3d, mx, my, radius);
                    }
                    editor->draw_overlay(cam, fb, &renderer3d, edit_hover, edit_hover_edge, edit_colors);
                    static const char* const kHelp[] = {
                        "EDIT MODE   TAB LEAVES   HOLD RIGHT MOUSE TO LOOK",
                        "CLICK PICKS A CORNER   SHIFT ADDS   CTRL PICKS AN EDGE",
                        "M MEET HALF WAY   J JOIN ONTO THE FIRST   DEL PUT BACK",
                        "ARROWS PGUP PGDN NUDGE (SHIFT 8, CTRL 1/4)   CTRL+Z UNDO   G NEXT GAP",
                    };
                    const int ts = std::max(1, fb.height() / 300);
                    int ty = 4 * ts;
                    for (const char* line : kHelp) {
                        scr::draw_overlay_text(fb, 4 * ts, ty, line, edit_colors.corner, edit_colors.shadow, ts);
                        ty += 10 * ts;
                    }
                    if (!edit_note.empty()) scr::draw_overlay_text(fb, 4 * ts, ty, edit_note, edit_colors.hover, edit_colors.shadow, ts);
                }
                if (view_note_timer > 0.0f && hud) {
                    // F9's note: the new view distance
                    if (step) view_note_timer -= clock.frame_time();
                    char note[48];
                    if (note_mipmaps) std::snprintf(note, sizeof note, "MIPMAPS %s", opt.mipmaps ? "ON" : "OFF");
                    else std::snprintf(note, sizeof note, "VIEW DISTANCE %gX", double(opt.view_distance));
                    scr::HudSurface hsurf(fb, hud->scale);
                    hsurf.text(hud->fonts, (hsurf.width() - hsurf.measure(hud->fonts, note, 1)) / 2, 0x50 * hud->scale, note, 1, 0);
                }
            } else if (menu) {
                scr::MenuKeys mk;
                bool script_back = false, script_b = false;  // --menu-keys padback / padb
                // the default keys and pad buttons, whatever the bindings (Start as Enter)
                const auto nv = input.nav();
                // (B on the main page would quit the game: only Esc does that)
                const bool top = menu->mode == 0 && menu->page == 0 && menu->page_stack.empty();
                mk.esc = top ? bool(SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_ESCAPE]) : nv.esc;
                mk.enter = nv.enter || nv.start;
                mk.up = nv.up;
                mk.down = nv.down;
                mk.left = nv.left;
                mk.right = nv.right;
                // scripted keys: "frame:key" held for that one frame
                for (size_t pos = 0; pos < opt.menu_keys.size();) {
                    size_t end = opt.menu_keys.find(',', pos);
                    if (end == std::string::npos) end = opt.menu_keys.size();
                    const std::string item = opt.menu_keys.substr(pos, end - pos);
                    pos = end + 1;
                    const size_t colon = item.find(':');
                    // "frame:key", or "first-last:key" held over those frames
                    char* dash = nullptr;
                    const uint64_t first = std::strtoull(item.c_str(), &dash, 10);
                    const uint64_t last_frame = dash && *dash == '-' ? std::strtoull(dash + 1, nullptr, 10) : first;
                    if (colon == std::string::npos || frame < first || frame > last_frame) continue;
                    const std::string k = item.substr(colon + 1);
                    script_back |= k == "padback";
                    script_b |= k == "padb";  // B on a pad without Back
                    mk.esc |= k == "esc";
                    mk.enter |= k == "enter";
                    mk.up |= k == "up";
                    mk.down |= k == "down";
                    mk.left |= k == "left";
                    mk.right |= k == "right";
                }
                // OPTIONS > CONTROLS (ours): the lines' texts from the bindings, the rebinding
                // screen's requests, RUMBLE and PAD (saved and used straight away)
                {
                    scr::input::Bindings& bd = input.bindings;
                    const scr::input::Bindings before = bd;
                    if (const auto c = input.poll_capture(); c.done) {
                        if (!c.cancelled) bd.assign(capture_action, capture_pad, c.bind);
                        capture_action = -1;
                        nav_block = true;  // (what was pressed must not also work the menu)
                    }
                    if (menu->bind_request && capture_action < 0) {
                        capture_action = menu->bind_request->action;
                        capture_pad = menu->bind_request->pad;
                        input.start_capture(capture_pad);
                    }
                    menu->bind_request.reset();
                    if (menu->bind_toggle >= 0) bd.toggle_digital(menu->bind_toggle);
                    menu->bind_toggle = -1;
                    if (menu->controls_reset) {
                        const bool rumble = bd.rumble;
                        const std::string guid = bd.pad_guid;
                        bd = scr::input::Bindings::defaults();
                        bd.rumble = rumble;
                        bd.pad_guid = guid;
                        menu->controls_reset = false;
                    }
                    bd.rumble = menu->rumble();
                    if (!bd.rumble && before.rumble) input.stop_rumble();
                    // PAD: ANY, then the connected controllers (the chosen one stays listed while away)
                    std::vector<std::string> names{"ANY"}, guids{""};
                    for (const auto& d : input.devices()) {
                        names.push_back(d.name);
                        guids.push_back(d.guid);
                    }
                    if (!bd.pad_guid.empty() && std::find(guids.begin(), guids.end(), bd.pad_guid) == guids.end()) {
                        names.push_back("NOT CONNECTED");
                        guids.push_back(bd.pad_guid);
                    }
                    const int choice = menu->pad_choice();
                    if (names == menu->pad_names && choice >= 0 && size_t(choice) < guids.size()) bd.pad_guid = guids[size_t(choice)];
                    const int want = int(std::find(guids.begin(), guids.end(), bd.pad_guid) - guids.begin());
                    bool changed = false;
                    if (names != menu->pad_names || want != choice) {
                        menu->set_pad_choice(want, names);
                        changed = menu->page == scr::Menu::kControlsPage;
                    }
                    for (int a = 0; a < scr::input::kActionCount; ++a) {
                        auto line = [&](bool on_pad) {
                            std::string t = scr::input::action_short(a);
                            if (capture_action == a && capture_pad == on_pad) return t + (on_pad ? " PRESS BUTTON" : " PRESS KEY");
                            const auto& slots = on_pad ? bd.pad[size_t(a)] : bd.keys[size_t(a)];
                            bool any = false;
                            for (const auto& b : slots)
                                if (b.kind != scr::input::Bind::kNone) {
                                    t += " " + b.name();
                                    any = true;
                                }
                            return any ? t : t + " -";
                        };
                        const std::string k = line(false), pd = line(true);
                        if (k != menu->key_text[size_t(a)]) changed |= menu->page == scr::Menu::kKeyboardPage;
                        if (pd != menu->pad_text[size_t(a)]) changed |= menu->page == scr::Menu::kControllerPage;
                        menu->key_text[size_t(a)] = k;
                        menu->pad_text[size_t(a)] = pd;
                    }
                    if (changed && menu->mode == 0) menu->refresh();
                    if (!(bd == before) && !shared.controls_path.empty()) bd.save(shared.controls_path);
                    if (capture_action >= 0 || nav_block) {
                        // waiting for a key (the menu stands still), then for everything to be let go
                        if (capture_action < 0 && !mk.esc && !mk.enter && !mk.up && !mk.down && !mk.left && !mk.right)
                            nav_block = false;
                        mk = scr::MenuKeys{};
                    }
                }
                menu->update(mk, clock.frame_time());
                // The main page's way out on a pad (ours): Back quits as Esc does there; a pad
                // without Back holds B for 2 seconds (the ring in the corner fills, shown after
                // 0.2 s so a tap shows nothing). Game time: headless runs count frames.
                {
                    const bool top_page = menu->mode == 0 && menu->page == 0 && menu->page_stack.empty() && capture_action < 0;
                    const bool back = input.back_held() || script_back;
                    if (top_page && back && !pad_back_was) menu->quit = true;
                    pad_back_was = back;
                    if (top_page && (input.b_held_without_back() || script_b)) {
                        quit_hold += clock.frame_time();
                        if (quit_hold >= kQuitHold) menu->quit = true;
                    } else {
                        quit_hold = 0;
                    }
                }
                if (menu->music_midi() != shared.music_midi) {
                    // CD MUSIC / MIDI MUSIC: the menu tune again from the other source, saved
                    shared.music_midi = menu->music_midi();
                    start_music(scr::kCdTrackMenu, scr::kMusicMenu);
                    save_menu();
                }
                if (menu->volumes() != shared.volumes) {
                    // the SOUND page: heard (the menu's music and clicks) and saved straight away
                    shared.volumes = menu->volumes();
                    apply_group_volumes();
                    save_menu();
                }
                if (const scr::VideoSettings mv = menu->video(); !(mv == menu_video) || menu->video_apply) {
                    // OPTIONS > VIDEO: saved and used straight away (distance, detail, far models
                    // and mipmaps are for the races)
                    menu_video = mv;
                    shared.saved_video = mv;
                    save_menu();
                    if (mv.vsync != opt.vsync && !opt.headless) SDL_SetRenderVSync(renderer, mv.vsync ? 1 : 0);
                    scr::VideoSettings now = video_from(opt);
                    const bool new_picture = mv.fullscreen != now.fullscreen || mv.width != now.width || mv.height != now.height;
                    now.widescreen = mv.widescreen;
                    now.view_distance = mv.view_distance;
                    now.vsync = mv.vsync;
                    now.frame_limit = mv.frame_limit;
                    now.lod = mv.lod;
                    now.mipmaps = mv.mipmaps;
                    if (new_picture) now = mv;
                    apply_video(opt, now, true);
                    if (new_picture) {
                        // fullscreen and RES: the window and the picture at once
                        SDL_Log("video: %s, %s, %s", mv.fullscreen ? "fullscreen" : "window", mv.widescreen ? "widescreen" : "4:3",
                                mv.width ? (std::to_string(mv.width) + "x" + std::to_string(mv.height)).c_str() : "native");
                        if (!opt.headless) apply_window(window, opt);
                    }
                    resize_menu_picture();
                    menu->video_apply = false;
                }
                for (int sfx : menu->sounds) mixer.play(sfx);
                // (only a picture that will be shown: a slow picture at a large size must not
                // keep the steps from catching up, or nothing would be shown at all)
                if (!skip_draw) menu->draw(fb, renderer3d);
                if (!skip_draw && quit_hold > kQuitHoldShow) {
                    // the hold to quit: a ring in the bottom right corner filling clockwise from the top
                    const scr::Palette& pal = fb.palette();
                    auto nearest = [&](int r, int g, int b) {
                        int best = 0, bd2 = 1 << 30;
                        for (int i = 0; i < 256; ++i) {
                            const int dr = pal[size_t(i)].r - r, dg = pal[size_t(i)].g - g, db = pal[size_t(i)].b - b;
                            const int d2 = dr * dr + dg * dg + db * db;
                            if (d2 < bd2) { bd2 = d2; best = i; }
                        }
                        return uint8_t(best);
                    };
                    const uint8_t lit = nearest(255, 255, 255), dim = nearest(70, 70, 70);
                    const float fill = std::min(1.0f, quit_hold / kQuitHold);
                    const int h = fb.height(), w = fb.width();
                    // (round on the screen: the pixels' shape, 5/6 as wide as high at 320x200)
                    const float px = fb.wide ? 1.0f : 4.0f * float(h) / (3.0f * float(w));
                    const float ro = float(h) / 14.0f, ri = ro * 0.6f, rx = ro / px;
                    const int cx = w - int(rx * 1.7f), cy = h - int(ro * 1.7f);
                    for (int y = int(float(cy) - ro) - 1; y <= int(float(cy) + ro) + 1; ++y)
                        for (int x = int(float(cx) - rx) - 1; x <= int(float(cx) + rx) + 1; ++x) {
                            if (x < 0 || y < 0 || x >= w || y >= h) continue;
                            const float dx = (float(x - cx) + 0.5f) * px, dy = float(y - cy) + 0.5f, d = std::sqrt(dx * dx + dy * dy);
                            if (d > ro || d < ri) continue;
                            float a = std::atan2(dx, -dy) / 6.2831853f;  // 0 at the top, clockwise
                            if (a < 0) a += 1.0f;
                            fb.at(x, y) = a <= fill ? lit : dim;
                        }
                }
                if (menu->quit) {
                    running = false;
                    save_menu();
                }
                if (menu->want_demo && opt.menu_keys.empty() && opt.frames == 0) {
                    // frontend_run ends after 30 idle seconds: the intro demo again
                    running = false;
                    shared.to_demo = true;
                    menu->want_demo = false;  // (taken: back in the menu, the 30 seconds start again)
                    save_menu();
                }
                if (menu->race) {
                    SDL_Log("menu: race on MAP%d%s, car %d, %d laps, level %d, driver %s", menu->race->map,
                            menu->race->mirrored ? " (reversed)" : "", menu->race->car, menu->race->laps,
                            menu->race->level, menu->race->name.c_str());
                    shared.race = *menu->race;
                    menu->race.reset();
                    running = false;
                    save_menu();
                    // frontend_start_race's loading screen
                    menu->draw_loading(fb, shared.race->map);
                }
            } else if (title) {
                scr::blit_pcx(*title, fb);
            } else {
                scr::draw_test_pattern(fb, frame);
            }
            stepped = !skip_draw;
            if (step && !opt.record_audio.empty()) {
                // One frame of sound per simulated frame (2 ticks = 1/50 s at 44.1 kHz).
                const int n = int(44100 * 2 / 100);
                const size_t at = recorded.size();
                recorded.resize(at + size_t(n) * 2);
                mixer.mix(recorded.data() + at, n, 44100);
            }
            if ((step || opt.tween_test) && opt.capture_every && frame >= opt.capture_from && frame % opt.capture_every == 0) {
                scr::SurfacePtr s = scr::framebuffer_to_surface(fb);
                char name[64];
                std::snprintf(name, sizeof name, "/fb_%06llu%s.png", static_cast<unsigned long long>(frame), step ? "" : "_t");
                save_or_log(s.get(), opt.capture_dir + name);
            }
            if (step) ++frame;
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
        mark(stepped ? scr::prof::Hud : scr::prof::Other);
        if (stepped) {
            void* pixels; int pitch;
            if (tex_palette) {
                std::array<SDL_Color, 256> colors;
                for (size_t i = 0; i < 256; ++i) colors[i] = {fb.palette()[i].r, fb.palette()[i].g, fb.palette()[i].b, 255};
                if (!tex_colors_set || std::memcmp(colors.data(), tex_colors.data(), sizeof colors) != 0) {
                    SDL_SetPaletteColors(tex_palette, colors.data(), 0, 256);
                    tex_colors = colors;
                    tex_colors_set = true;
                }
                SDL_UpdateTexture(tex, nullptr, fb.pixels(), fb.width());
            } else if (SDL_LockTexture(tex, nullptr, &pixels, &pitch)) {
                fb.to_rgba(static_cast<uint8_t*>(pixels), pitch, renderer3d.pool());
                SDL_UnlockTexture(tex);
            }
            mark(scr::prof::Upload);
        }

        int ow, oh;
        SDL_GetRenderOutputSize(renderer, &ow, &oh);
        // (a menu made before WIDESCREEN went on keeps its 4:3 shape: its pixels need not be square)
        const SDL_FRect dst = present_rect(ow, oh, menu && !fb.wide && opt.aspect == "wide" ? "4:3" : opt.aspect, fb.width(), fb.height());
        // A picture at least twice the size it is shown at (a render size above the screen's) is
        // halved on the graphics card until it is less than that: each pass of the linear filter
        // at exactly half the size averages 2x2 pixels, so every pixel counts (a box filter, as
        // supersampling wants); the last step is the linear filter's.
        SDL_Texture* shown = tex;
        if (!tex_palette && (!opt.headless || opt.hidden)) {
            int cw = fb.width(), ch = fb.height();
            for (size_t level = 0; cw >= 2 * int(dst.w) && ch >= 2 * int(dst.h) && cw >= 2 && ch >= 2; ++level) {
                const int nw = cw / 2, nh = ch / 2;
                if (halves.size() <= level) halves.push_back(nullptr);
                SDL_Texture*& t = halves[level];
                float tw = 0, th = 0;
                if (t) SDL_GetTextureSize(t, &tw, &th);
                if (!t || int(tw) != nw || int(th) != nh) {
                    if (t) SDL_DestroyTexture(t);
                    t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, nw, nh);
                    if (!t) break;
                    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
                }
                SDL_SetTextureScaleMode(shown, SDL_SCALEMODE_LINEAR);
                if (!SDL_SetRenderTarget(renderer, t)) break;
                // (source: the whole even-sized part, so 2x2 blocks line up)
                const SDL_FRect src{0, 0, float(nw * 2), float(nh * 2)};
                SDL_RenderTexture(renderer, shown, &src, nullptr);
                shown = t;
                cw = nw;
                ch = nh;
            }
            SDL_SetRenderTarget(renderer, nullptr);
            SDL_SetTextureScaleMode(shown, dst.w < float(cw) ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (menu && opt.aspect == "wide" && (dst.w + 1.0f < float(ow) || dst.h + 1.0f < float(oh))) {
            if (menu->backdrop() != bars_of) {
                bars_of = menu->backdrop();
                if (bars_of) make_bars(*bars_of);
            }
            if (bars_tex && bars_of) {
                // (fading with the menu)
                const auto m = uint8_t(std::clamp(menu->brightness, 0.0f, 1.0f) * 255.0f);
                SDL_SetTextureColorMod(bars_tex, m, m, m);
                SDL_RenderTexture(renderer, bars_tex, nullptr, nullptr);
            }
        }
        SDL_RenderTexture(renderer, shown, nullptr, &dst);

        const bool last_frame = opt.frames && frame >= opt.frames;
        if (last_frame && player) {
            const scr::Car& c = player->race.player;
            SDL_Log("car: pos %d,%d,%d yaw %.3f speed %.0f gear %d rpm %.0f wall %d", c.pos_x, c.pos_y, c.pos_z, double(c.yaw),
                    double(c.speed), c.gear, double(c.engine_rpm), int(c.wall_contact));
        }
        // (the view, to look at the same one again with --scene track --camera, or in the original)
        if (last_frame && track)
            SDL_Log("camera: %d,%d,%d,%d,%d,%d", int(cam.x), int(cam.y), int(cam.z), int(cam.yaw), int(cam.pitch), int(cam.roll));
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
        // Interactive runs present only new pictures (vsync paces the loop then); with nothing
        // new and no step due, wait a little instead of spinning.
        if (stepped || opt.headless) SDL_RenderPresent(renderer);
        else if (!catching_up) SDL_Delay(1);
        mark(scr::prof::Present);
        if (stepped) scr::prof::frame_done(step);
        if (opt.perf && prof_last >= prof_report_at) {
            prof_report_at = prof_last + std::chrono::seconds(5);
            const std::string rep = scr::prof::report();
            if (!rep.empty()) std::fputs(rep.c_str(), stderr);
        }
    }
    if (opt.perf) {
        const std::string rep = scr::prof::report();
        if (!rep.empty()) std::fputs(rep.c_str(), stderr);
    }

    SDL_Log("frames=%llu", static_cast<unsigned long long>(frame));
    if (opt.perf && perf_frames)
        SDL_Log("render3d: %.2f ms average, %.2f ms worst over %d frames at %dx%d", perf_total / perf_frames, perf_max,
                perf_frames, fb.width(), fb.height());
    shared.frames_run = frame;
    if (positions) std::fclose(positions);
    if (player && (replaying || !opt.record_file.empty())) {
        const scr::Car& pc = player->race.player;
        SDL_Log("replay: frame %zu, player at %d %d %d", replaying ? replay_pos : replay.frames.size(), pc.pos_x, pc.pos_y, pc.pos_z);
    }
    if (!opt.record_file.empty() && player) {
        replay.car = player->car_index;
        replay.laps = opt.laps;
        replay.level = opt.level;
        replay.game_mode = opt.game_mode;
        replay.race_options = opt.race_options;
        replay.finish_recording(input.replay_controls());
        if (!replay.save(opt.record_file)) exit_code = 1;
    }
    if (!opt.record_audio.empty() && !write_wav(opt.record_audio, recorded, 44100)) exit_code = 1;
    if (audio_stream) SDL_DestroyAudioStream(audio_stream);
    race_audio.stop();
    mixer.set_music(nullptr);
    SDL_DestroyTexture(tex);
    if (bars_tex) SDL_DestroyTexture(bars_tex);
    for (SDL_Texture* t : halves)
        if (t) SDL_DestroyTexture(t);
    for (SDL_Texture* t : halves)
        if (t) SDL_DestroyTexture(t);
    if (tex_palette) SDL_DestroyPalette(tex_palette);
    return exit_code;
}

int main(int argc, char** argv) {
#ifdef SCREAMER_GUI_SUBSYSTEM
    attach_parent_console();  // (release packages: no console window of their own)
#endif
    Options opt;
    if (!parse_args(argc, argv, opt)) return 2;
    install_debug_report(opt.headless);  // headless runs (tests): assertions to stderr, no dialog

    if (opt.headless) {
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        if (!opt.hidden) {
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
            if (opt.renderer.empty()) opt.renderer = "software";
        }
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
    if (!game_dir && !opt.headless && opt.scene.empty()) {
        // A plain start without the game: say where it goes rather than show the test pattern.
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Screamer HD: game files not found",
            "Screamer HD needs the files of your own copy of the original game (DOS, GOG or Steam).\n\n"
            "Copy the game's folder (the one with the GFX and DATA folders in it) into a folder\n"
            "named SCREAMER next to screamer.exe, or put screamer.exe and its DLLs into the\n"
            "game's folder, or start screamer.exe with --data \"C:\\path\\to\\Screamer\".\n\n"
            "See README.md for details.",
            nullptr);
        SDL_Quit();
        return 1;
    }

    // Without --scene the game starts with the intro demo, then the menu (game_main:
    // attract_mode_run, frontend_run), when the game files are there.
    if (opt.scene.empty() && game_dir) opt.scene = "attract";
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

    Shared shared;
    if (!opt.save_dir.empty()) {
        // --save-dir: CHOICE.DAT, ghosts and replays there (headless checks)
        shared.choice_path = opt.save_dir + "/CHOICE.DAT";
        shared.replay_dir = opt.save_dir + "/REPLAYS/";
    } else if (!opt.headless) {
        if (char* pref = SDL_GetPrefPath("", "ScreamerHD")) {
            // The saves of the builds before the name Screamer HD (%APPDATA%\ScreamerRecomp)
            // come over once, if there are none here yet.
            std::error_code ec;
            const std::filesystem::path here(pref);
            const std::filesystem::path old = here.parent_path().parent_path() / "ScreamerRecomp";
            if (!std::filesystem::exists(here / "CHOICE.DAT", ec) && std::filesystem::is_directory(old, ec)) {
                std::filesystem::copy(old, here, std::filesystem::copy_options::recursive |
                                                     std::filesystem::copy_options::skip_existing, ec);
                SDL_Log("saves: copied from %s%s", old.string().c_str(), ec ? " (not all)" : "");
            }
            shared.choice_path = std::string(pref) + "CHOICE.DAT";
            shared.replay_dir = std::string(pref) + "REPLAYS/";
            SDL_free(pref);
        }
    }
    // The controls: CONTROLS.INI next to CHOICE.DAT; the first time, the keys of the game's
    // CONFIG/CONTROLS.CFG (SETUP.EXE's) come over. Headless runs: the defaults, nothing saved.
    shared.input.init();
    shared.input.bindings = scr::input::Bindings::defaults();
    {
        const auto cfg = game_dir ? scr::resolve_game_file(*game_dir, "CONFIG/CONTROLS.CFG") : std::nullopt;
        const bool have_cfg = cfg && shared.input.original.load(*cfg);
        if (!shared.choice_path.empty()) {
            shared.controls_path = shared.choice_path.substr(0, shared.choice_path.size() - std::strlen("CHOICE.DAT")) + "CONTROLS.INI";
            if (!shared.input.bindings.load(shared.controls_path)) {
                if (have_cfg) {
                    shared.input.bindings.import_original(shared.input.original);
                    SDL_Log("controls: the keys of %s", cfg->c_str());
                }
                shared.input.bindings.save(shared.controls_path);
            }
        }
    }
    // The SOUND and VIDEO pages: saved after our CHOICE.DAT's own bytes; --volumes and the
    // command line's video options win for this run.
    if (!shared.choice_path.empty()) {
        scr::ChoiceFile choice;  // (without the file: the defaults, a NATIVE picture)
        if (choice.load(shared.choice_path)) {
            std::copy(choice.volume.begin(), choice.volume.end(), shared.volumes.begin());
            shared.music_midi = choice.music_midi != 0;
        }
        shared.saved_video = choice.video;
        if (!opt.headless) apply_video(opt, choice.video, false);  // (headless runs: the command line's picture only)
    }
    if (!opt.volumes.empty()) {
        size_t i = 0;
        for (const char* p = opt.volumes.c_str(); *p && i < shared.volumes.size(); ++i) {
            char* end = nullptr;
            shared.volumes[i] = std::clamp(int(std::strtol(p, &end, 10)), 0, scr::kVolumeMax);
            if (end == p) break;
            p = *end == ',' ? end + 1 : end;
        }
    }
    SDL_WindowFlags wflags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (opt.fullscreen) wflags |= SDL_WINDOW_FULLSCREEN;
    if (opt.hidden) wflags |= SDL_WINDOW_HIDDEN | SDL_WINDOW_NOT_FOCUSABLE;
    SDL_Window* window = SDL_CreateWindow("Screamer HD", opt.width, opt.height, wflags);
    if (!window) { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "window: %s", SDL_GetError()); return 1; }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, opt.renderer.empty() ? nullptr : opt.renderer.c_str());
    if (!renderer) { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "renderer: %s", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(renderer, opt.headless || !opt.vsync ? 0 : 1);
    if (!opt.headless && !opt.was_given("--size")) apply_window(window, opt);
    SDL_Log("video driver: %s, renderer: %s", SDL_GetCurrentVideoDriver(), SDL_GetRendererName(renderer));

    int exit_code = 0;
    if (!opt.replay_file.empty()) {
        // --replay FILE
        if (!apply_replay_header(opt)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "cannot read replay %s", opt.replay_file.c_str());
            return 1;
        }
        opt.scene = "drive";
    }
    for (;;) {
        shared.back_to_menu = false;
        shared.restart_race = false;
        shared.attract_done = false;
        shared.to_demo = false;
        shared.play_replay = false;
        if (opt.scene == "attract") {
            // attract_mode_setup: the track last chosen in the menu (CHOICE.DAT), difficulty
            // B, CPU cars only
            opt.scene = "drive";
            opt.attract = true;
            opt.level = 1;
            opt.reversed = false;
            opt.race_options = 0;
            // (CHOICE.DAT +0x434, DAT_001207f4: each demo shows the next track)
            if (!shared.choice_path.empty()) {
                scr::ChoiceFile choice;
                if (choice.load(shared.choice_path)) {
                    const int t = std::clamp(choice.get(0x434), 0, 5);
                    const int map = t + 1;
                    choice.set(0x434, t + 1 > 5 ? 0 : t + 1);
                    choice.save(shared.choice_path);
                    opt.track = "MAP" + std::to_string(map);
                }
            }
        }
        exit_code = run_scene(opt, window, renderer, game_dir, title, shared);
        if (opt.attract) {
            opt.attract = false;
            if (exit_code == 0 && shared.attract_done) {
                opt.scene = "menu";
                opt.hold.clear();
                continue;
            }
        }
        if (exit_code != 0) break;
        if (opt.frames) {
            // --frames counts over all the scenes of the run
            if (shared.frames_run >= opt.frames) break;
            opt.frames -= shared.frames_run;
        }
        if (shared.to_demo) {
            opt.scene = "attract";
            continue;
        }

        if (shared.play_replay) {
            // the race just run, again from its inputs, then PLAY AGAIN?
            opt.replay_file = shared.outcomes.back().replay;
            opt.instant_replay = true;
            continue;
        }
        opt.replay_file.clear();
        opt.instant_replay = false;
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
            opt.reversed = shared.race->mirrored;
            opt.race_options = shared.race->race_options;
            opt.car_index = shared.race->car;
            opt.laps = shared.race->laps;
            opt.level = shared.race->level;
            opt.detail = shared.race->high_detail ? "H" : "L";
            opt.moving_scenery = shared.race->detail > 0;
            opt.speech = shared.race->speech;
            opt.music = shared.race->music;
            opt.from_menu = true;
            opt.game_mode = shared.race->game_mode;
            opt.ghost = shared.race->ghost;
            if (!shared.choice_path.empty())
                opt.ghost_dir = shared.choice_path.substr(0, shared.choice_path.size() - std::strlen("CHOICE.DAT"));
            opt.start.clear();
            opt.menu_keys.clear();
            if (!shared.race->replay.empty()) {
                // the records page's replay: the race of REPLAY<track><n>.RPL
                opt.replay_file = shared.race->replay;
                apply_replay_header(opt);
            }
            continue;
        }
        break;
    }
    shared.input.shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
