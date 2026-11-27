#pragma once
// The front end (frontend_* and menu_* in STARTL.EXE): a 3D scene over a PCX backdrop.
//
// Every menu line is spelled with ALETTER.3DX blocks (GFX/MENU.DAT), one object per letter
// (up to 80); the letter is a texture frame: instance animation = 0x35e * (char - 0x20) + 0x226.
// A page builds its items with a fly-in start position (one of 8 variants, table 0x102218),
// then each frame the letters spiral in (k goes 1 -> 0), the selected line shows the front
// face, the others are turned over (angle pi) and all of them bob on a sine wave.
// The camera (g_camera) blends between two positions with t^4 or t.
//
// Modes (DAT_0011fac0): 0 the text pages, 1 a transition (fade out, then the next mode's
// backdrop and setup), 2 car selection (MENU4L.PCX, 14 spinning AAUTO models 4000 apart),
// 3 track selection (MENU3L.PCX, 12 AMAP models 40000 apart), 4 name entry (MENU2L.PCX,
// the alphabet as a ring of bouncing letters), 5 championship, 6 dialogs, 8 network lobby.
// Normal game: name -> track -> car -> race.
//
// Page tables (0x102de4): per item { text index, action, value, max }; the text shown is
// MENU.TXT[index + value]; action 1234 (0x4d2) cycles the value (options).
//   0 main: NORMAL GAME, CHAMPIONSHIP, SPECIAL MODES, NETWORK GAME, OPTIONS
//   1 options: detail, difficulty, laps, speech, music, VIEW RECORDS (ours: difficulty, laps, VIDEO,
//     SOUND, VIEW RECORDS; the detail is on VIDEO, speech and music are volumes on SOUND)
//   2 network, 3 championship (new/resume), 4 special modes, 5 network (new/resume),
//   6 records (the six tracks), 7 (ours) SOUND: the four volumes (Enter / left / right) and
//   CD MUSIC / MIDI MUSIC, 8 (ours) VIDEO: fullscreen, widescreen, resolution, view distance,
//   detail, vsync, frame limit, far models, mipmaps

#include "core/hud.h"
#include "core/model3d.h"
#include "core/pcx.h"
#include "core/render3d.h"
#include "core/track.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace scr {

struct MenuCar {
    std::string name;
    int transmission = 0, max_speed = 0, grip = 0, acceleration = 0;  // CARDAT.TXT
};

struct MenuAssets {
    // menu_dat: GFX/MENU.DAT, graf_dat: GFX/GRAF.DAT, local_dat: DATA/LOCAL.DAT.
    // hires: the 640x400 sprites, fonts and backdrops (*H) of STARTH.EXE.
    bool load(const std::string& menu_dat, const std::string& graf_dat, const std::string& local_dat,
              bool hires = false);
    const PcxImage* backdrop(const std::string& name);  // e.g. "MENU1" -> MENU1L.PCX

    Track scene;           // MENU.MLB materials for the renderer
    Model letter;          // ALETTER.3DX
    std::vector<Model> cars;   // AAUTO00..06
    std::vector<Model> maps;   // AMAP1..6
    HudFonts fonts;        // MENUL.FNT
    HudSprites sprites;    // MENUL.SPR
    std::vector<std::string> texts;  // MENU.TXT
    std::vector<MenuCar> car_info;   // CARDAT.TXT, one per car model (each in two paints)
    std::vector<std::string> track_names;  // TRACKDAT.TXT
    std::vector<std::vector<std::string>> champ_text;  // CHAMP.TXT blocks (separated by '#')
    std::string graf_path;
    char res = 'L';
    int scale() const { return res == 'H' ? 2 : 1; }  // HudSurface scale of these graphics

private:
    std::map<std::string, PcxImage> backdrops_;
};

// CONFIG/CHOICE.DAT: the game's saved menu state, a 0x460-byte image of the globals from
// 0x1203c0 (read_choice_dat / write_choice_dat):
//   +0x000 car, +0x004 track, +0x008 name[4], +0x00c detail, +0x010 difficulty, +0x014 laps,
//   +0x018..0x020 championship, +0x03c 6 ints, +0x06c best laps per track (6 x 0x50: float
//   time[5], char name[5][4], int flags[5], int mode[5]), +0x24c best total times (same,
//   +0x28 the replay number), +0x42c cars unlocked, +0x430 view, +0x438 speech, +0x43c music,
//   +0x444..0x44c music/sfx/cd volume, +0x450 radar, +0x454 tracks unlocked, +0x458 tracks
//   seen, +0x45c difficulty max.
// Ours follow the 0x460 bytes (the original reads only those): "SCRX", int32 count, then that
// many int32 settings of the SOUND page: the four volumes (announcer, engine, effects,
// music; 0..kVolumeMax), then the music source (0 CD, 1 MIDI). A file without them (the
// original's), or with fewer, gets the defaults for the rest.
// Then (ours) the VIDEO page: fullscreen, widescreen, render width and height (0: the screen's),
// view distance in tenths (0: no limit), vsync, frame limit, far models simple, mipmaps.
constexpr int kVolumeMax = 10;

// The VIDEO page's settings (main applies them; the detail stays in CHOICE.DAT's own bytes).
struct VideoSettings {
    bool fullscreen = false;
    bool widescreen = false;     // --aspect wide; off: the original's 4:3 picture
    int width = 0, height = 0;   // the render size; 0: as many pixels as the window has (NATIVE)
    float view_distance = 2.0f;  // times the original's far clip; 0: no limit
    bool vsync = true;
    int frame_limit = 0;         // pictures a second at most (0: no limit)
    bool lod = false;            // far models in the original's simpler versions (off: full detail)
    bool mipmaps = true;
    bool operator==(const VideoSettings&) const = default;
};
// The VIDEO page's render sizes: {0, 0} (NATIVE) first, then 4:3 sizes (with the original's
// 320x200 and 640x400) and with widescreen also 16:9, 16:10 and wider, up to 8K.
std::vector<std::pair<int, int>> video_resolutions(bool widescreen);

struct ChoiceFile {
    std::array<uint8_t, 0x460> raw{};
    std::array<int32_t, 4> volume{kVolumeMax, kVolumeMax, kVolumeMax, kVolumeMax};
    int32_t music_midi = 0;  // 1: the MIDI tunes on the FM synth even with the CD image there
    VideoSettings video;
    ChoiceFile();  // the defaults read_choice_dat uses without a file
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    int32_t get(size_t o) const;
    void set(size_t o, int32_t v);
    float getf(size_t o) const;
    void setf(size_t o, float v);
    std::string str(size_t o) const;  // char[4]
    void setstr(size_t o, const std::string& v);
};

// The menus' 2D drawing in the original's 320x200 units. With the 640x400 graphics (STARTH.EXE)
// the positions are doubled when drawn and widths and heights measured in its fonts halved,
// as STARTH does it; the glyphs and sprites keep their own pixels.
class MenuUi {
public:
    MenuUi(HudSurface& s, int scale) : s_(s), k_(scale) {}
    int scale() const { return k_; }
    void text(const HudFonts& f, int x, int y, const std::string& t, int font, int mode) {
        s_.text(f, x * k_, y * k_, t, font, mode);
    }
    int measure(const HudFonts& f, const std::string& t, int font) const { return s_.measure(f, t, font) / k_; }
    // centred on the screen (font_draw_centered: the full width of the surface)
    void centre(const HudFonts& f, const std::string& t, int y, int font, int mode) {
        s_.text(f, (s_.width() - s_.measure(f, t, font)) >> 1, y * k_, t, font, mode);
    }
    void sprite(const HudSprites& sp, int index, int x, int y) { s_.sprite(sp, index, x * k_, y * k_); }
    void darken(int x, int y, int w, int h, const std::array<uint8_t, 256>& t) { s_.darken(x * k_, y * k_, w * k_, h * k_, t); }

private:
    HudSurface& s_;
    int k_;
};

// Keys the menu reacts to, as held this frame (frontend_handle_keys acts on changes).
struct MenuKeys {
    bool esc = false, enter = false, up = false, down = false, left = false, right = false;
};

// What the menu asks the game to run (frontend_start_race).
struct MenuRace {
    int game_mode = 1;   // 1 normal game, 2 championship, 4 time attack, 0x20 cone carnage, 0x40 slalom
    bool ghost = false;  // time attack: race the saved ghost (RESUME)
    int map = 1;         // 1..6
    bool mirrored = false;  // tracks 7..12: the same maps the other way round
    int car = 0;         // CARS_DATA record (the two paints of each car are next to each other)
    int laps = 3;
    int level = 1;       // difficulty: opponent set 'A' + level
    bool high_detail = true;  // DETAIL option: MID and HI load GFX/MAPnH.DAT, LOW MAPnL.DAT
    int detail = 2;           // 0 low (no moving scenery), 1 mid, 2 hi (the original's far clip: we draw it all)
    bool speech = true, music = true;
    std::string name;
    // g_race_options: bits 0-1 the obstacles' column of OSTACLES (MONTY 1, JOINT 2), 4 the
    // clock stands still (CLOCK), 0x10 mirror (MIRRO: picture and steering), 0x20 upside down
    // (UPDOW)
    int race_options = 0;
    std::string replay;  // the records page: play this REPLAY<track><n>.RPL (game_mode | 0x80) instead
};

class Menu {
public:
    explicit Menu(MenuAssets& assets);

    // frontend_show_title
    void show_title();
    // frontend_text_input: a letter typed anywhere in the menus. Five letters matching one
    // of MENU.DTR's CHEATS (TAZOR VTELO MONTY JOINT CLOCK ABURN INVER MIRRO UPDOW) apply it
    // (sound 8); a letter that fits none starts again. Returns true when a cheat took.
    bool type_letter(char c);
    int race_options = 0;  // g_race_options (the cheats; see MenuRace)
    bool want_demo = false;  // left alone for 30 seconds: back to the intro demo
    // The saved state (read_choice_dat) and back (write_choice_dat).
    void load_choice(const ChoiceFile& c);
    ChoiceFile save_choice() const;
    // After a race (menu_replay_files): the best lap and, for 3-lap races, the total time go
    // into the track's top five under the player's name.
    // With the race's replay saved, a new total record gets a replay number (1..5, one no
    // other record of the track has); the replay becomes REPLAY<map-1><n>.RPL. Returns it, or 0.
    int race_result(int map, float best_lap, float total_time, int laps, bool finished, bool replay_saved = false);
    // Back to the records page after one of its replays.
    void show_records();
    // Where REPLAY<track><n>.RPL are looked for (the records page's "R"): ours, then the game's.
    std::vector<std::string> replay_dirs;
    // frontend_start_race's loading screen: LOAD<n>L.PCX and "LOADING".
    void draw_loading(Framebuffer& fb, int map);
    // One frame of frontend_draw_frame (keys, camera, page) at frame time dt.
    void update(const MenuKeys& keys, float dt);
    void draw(Framebuffer& fb, Renderer& renderer);
    const PcxImage* backdrop() const { return backdrop_; }  // the page's backdrop (ours: widescreen's side bars)

    // Sound effects requested this frame (indices into MENU.DTR's SOUND_FX: menu1..menu9).
    std::vector<int> sounds;
    bool quit = false;            // Esc on the main page
    std::optional<MenuRace> race; // set when a race should start
    float brightness = 0;         // DAT_0011d220: the palette fade

    struct Entry {
        int text, action, value, max;
    };
    std::vector<std::vector<Entry>> pages;
    // Ours: the SOUND page (OPTIONS > SOUND) - announcer, player's engine, other effects and
    // music volumes, 0..kVolumeMax (saved after CHOICE.DAT's own bytes).
    std::array<int, 4> volumes() const;
    void set_volumes(const std::array<int, 4>& v);
    // and its CD MUSIC / MIDI MUSIC line: true plays the MIDI tunes even with the CD image there
    bool music_midi() const;
    void set_music_midi(bool midi);
    // Ours: the VIDEO page (OPTIONS > VIDEO) and its ADVANCED page (kVideoPage + 1).
    static constexpr int kVideoPage = 8;
    enum VideoLine : int { kFullscreenLine, kWidescreenLine, kResolutionLine, kDistanceLine, kDetailLine, kVsyncLine,
                           kFpsLine, kFarModelsLine, kMipmapsLine };
    VideoSettings video() const;
    void set_video(const VideoSettings& v);
    // Set when Esc leaves the VIDEO page (main applies every change at once and clears it).
    bool video_apply = false;
    int page = 0;                    // DAT_0011fb5c
    std::array<int, 20> selected{};  // DAT_0011fb04: selected line per page
    std::vector<int> page_stack;     // DAT_0012034c
    int mode = 0;                    // DAT_0011fac0
    int car = 0;                     // DAT_001203c0
    int track = 0;                   // DAT_001203c4
    int cars_unlocked = 0;           // DAT_001207ec: the last car is 0xb + this
    int tracks_unlocked = 0;         // DAT_00120814: the last track is 2 + this
    std::string name = "AAA";        // DAT_001203c8
    int game_mode = 0;               // g_game_mode (1 normal, 2 championship race, -1 new championship)

    // The championship (frontend_show_champ_pcx, champ_draw_standings, champ_advance,
    // champ_store_race_result): a script of steps (table 0x1022b8). Each step is
    // { kind, league or points needed, track, CHAMP.TXT block, next, next on failure, backdrop,
    // race number, font }; kind 0 shows a text and leads to a race, 3 the standings after race n,
    // 1 a "next league" text (unlocks tracks), 2 the end (all won, or lost).
    struct ChampStep {
        int kind, value, track, text, next, fail;
        const char* backdrop;
        int race, font;
    };
    int champ_step = 0;                   // DAT_001203d8
    int champ_prev = 0;                   // DAT_001203dc (RESUME starts here)
    std::array<int, 6> champ_pos{};       // DAT_001203fc: finishing position per race, -1 none
    std::array<float, 6> champ_time{};    // DAT_001203e4
    std::array<int, 6> champ_points{};    // DAT_00120414
    int champ_failed = 0;                 // DAT_00120800: 1 out of time, 2 cannot qualify any more
    // After the race the menu asked for: the championship takes the result, others the title.
    void after_race(bool finished, bool failed, int position, float total_time);

    struct Item {
        int row = 0;              // +0x04
        int ch = -1;              // +0x08: character - 0x20, -1 unused
        int turned = 0;           // +0x0c: 1 shows the back (not the selected line)
        float angle = 0;          // +0x10 rotation about x
        float ay = 0, az = 0;     // +0x14, +0x18
        int x0 = 0, y0 = 0, z0 = 0;   // +0x34.. start
        int x1 = 0, y1 = 0, z1 = 0;   // +0x40.. place on the page
        int sx = 0, sy = 0;       // +0x4c, +0x50 spiral radius
        float k = 1, speed = 1;   // +0x58 progress, +0x5c
        float timer = 0;          // +0x60
        int ix = 0, iy = 0, iz = 0;  // the object instance
        float iax = 0, iay = 0, iaz = 0;
    };
    std::array<Item, 200> items{};  // (the original has 80; ours has longer pages)

    // The spinning models of the selection pages (0x11d54c cars, 0x11d904 maps).
    struct Shown {
        int x = 0, y = 0, z = 0;
        float ax = 0, ay = 0, az = 0;
        bool visible = false;
    };
    std::array<Shown, 14> car_models{};
    std::array<Shown, 12> map_models{};

private:
    void build_page_items(float dt);
    void draw_text(const std::string& s);
    void update_item_zoom(Item& it, float dt);
    void update_item_slide(Item& it, float dt);
    void update_item_static(Item& it);
    void hide_objects();
    void handle_keys(const MenuKeys& keys);
    void activate();
    void init_state(int next);
    void show_page_background();
    void setup_car_page();
    void setup_track_page();
    void setup_name_page();
    void name_page_update(float dt);
    void update_camera(float dt);
    void fade(float dt);
    void start_race();
    void draw_title_scroll(MenuUi& s);
    void draw_car_select(MenuUi& s);
    void draw_track_select(MenuUi& s);
    void draw_name_entry(MenuUi& s);
    void draw_records(MenuUi& s);
    void setup_champ_page();
    void draw_champ(MenuUi& s);
    void champ_advance();
    void setup_records_page();
    int option(int item) const;
    Entry& option_entry(int item);
    Entry& video_line(int line);  // VideoLine: VIDEO's lines, then its ADVANCED page's
    void cycle(int sel, int step);
    void set_resolutions(bool widescreen, int w, int h);
    std::vector<std::pair<int, int>> res_list_{{0, 0}};  // the RES line's sizes (its value indexes them)
    std::string entry_text(const Entry& e) const;
    void change_volume(int sel, int step);

    MenuAssets& a_;
    const PcxImage* backdrop_ = nullptr;
    int built_page_ = -1;     // DAT_0011fb60 (-2: rebuild after an option change)
    int variant_ = 0;         // DAT_0011fab8
    int flipping_ = 0;        // DAT_0011fabc
    int last_key_ = 0;        // DAT_0011fb64
    float wave_ = 0;          // DAT_0011d244
    int draw_mode_ = 0;       // DAT_001203b4
    int next_mode_ = 0;       // DAT_0011fac8
    int after_name_ = 3;      // DAT_0011d234
    int name_cursor_ = 0;     // DAT_0011d240
    std::array<char, 5> cheat_buf_{};  // DAT_00120828: the letters typed so far
    int cheat_len_ = 0;       // DAT_00120878
    float letter_ = 0;        // DAT_0011d238
    int tracks_seen_ = 0;     // DAT_00120818
    uint32_t frame_ = 0;      // g_frame_counter (blinking)
    ChoiceFile choice_;       // records and what this menu does not use itself
    int records_track_ = 0;   // DAT_00120988
    int records_row_ = 0;     // DAT_0012098c
    float idle_ = 0;          // DAT_0011d248: seconds without a key (texts move on after 10)
    int champ_total_ = 0;     // DAT_001203e0
    bool ghost_ = false;      // DAT_0011d260
    std::array<uint8_t, 256> darken_{};  // build_darken_tables for the dialog's backdrop
    struct Block {
        int row = 0, ypos = 0, start = 0, centred = 1, turned = 0;
        int x0 = 0, y0 = 0, z0 = 0, sx = 0, sy = 0;
        float spin = 0.09f, phase = 0;
    } block_;
    // camera: from -> to with t (pitch and roll are not blended)
    float cam_t_ = 0, cam_rate_ = 0.6f;
    bool cam_pow_ = true;          // DAT_0011d23c: t^4
    float cam_pitch_ = 0, cam_roll_ = 0, cam_yaw_from_ = 0, cam_yaw_to_ = 0;
    int from_[3]{}, to_[3]{};
    float fade_rate_ = 0.6f;      // DAT_00120374
    int fade_dir_ = 0;            // DAT_0011fac4: 1 in, -1 out, 0 done
    float logo1_x_ = 0, logo2_x_ = 0;
    Camera cam_;
};

} // namespace scr
