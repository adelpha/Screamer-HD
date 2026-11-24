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
//   1 options: detail, difficulty, laps, speech, music, VIEW RECORDS
//   2 network, 3 championship (new/resume), 4 special modes, 5 network (new/resume),
//   6 records (the six tracks)

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
    std::string graf_path;
    char res = 'L';

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
struct ChoiceFile {
    std::array<uint8_t, 0x460> raw{};
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

// Keys the menu reacts to, as held this frame (frontend_handle_keys acts on changes).
struct MenuKeys {
    bool esc = false, enter = false, up = false, down = false, left = false, right = false;
};

// What the menu asks the game to run (frontend_start_race).
struct MenuRace {
    int game_mode = 1;   // 1 normal game
    int map = 1;         // 1..6
    bool mirrored = false;  // tracks 7..12: the same maps the other way round
    int car = 0;         // CARS_DATA record (the two paints of each car are next to each other)
    int laps = 3;
    int level = 1;       // difficulty: opponent set 'A' + level
    bool high_detail = true;  // DETAIL option (HI: GFX/MAPnH.DAT)
    bool speech = true, music = true;
    std::string name;
};

class Menu {
public:
    explicit Menu(MenuAssets& assets);

    // frontend_show_title
    void show_title();
    // The saved state (read_choice_dat) and back (write_choice_dat).
    void load_choice(const ChoiceFile& c);
    ChoiceFile save_choice() const;
    // After a race (menu_replay_files): the best lap and, for 3-lap races, the total time go
    // into the track's top five under the player's name.
    void race_result(int map, float best_lap, float total_time, int laps, bool finished);
    // frontend_start_race's loading screen: LOAD<n>L.PCX and "LOADING".
    void draw_loading(Framebuffer& fb, int map);
    // One frame of frontend_draw_frame (keys, camera, page) at frame time dt.
    void update(const MenuKeys& keys, float dt);
    void draw(Framebuffer& fb, Renderer& renderer);

    // Sound effects requested this frame (indices into MENU.DTR's SOUND_FX: menu1..menu9).
    std::vector<int> sounds;
    bool quit = false;            // Esc on the main page
    std::optional<MenuRace> race; // set when a race should start
    float brightness = 0;         // DAT_0011d220: the palette fade

    struct Entry {
        int text, action, value, max;
    };
    std::vector<std::vector<Entry>> pages;
    int page = 0;                    // DAT_0011fb5c
    std::array<int, 20> selected{};  // DAT_0011fb04: selected line per page
    std::vector<int> page_stack;     // DAT_0012034c
    int mode = 0;                    // DAT_0011fac0
    int car = 0;                     // DAT_001203c0
    int track = 0;                   // DAT_001203c4
    int cars_unlocked = 0;           // DAT_001207ec: the last car is 0xb + this
    int tracks_unlocked = 0;         // DAT_00120814: the last track is 2 + this
    std::string name = "AAA";        // DAT_001203c8
    int game_mode = 0;               // g_game_mode

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
    std::array<Item, 80> items{};

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
    void draw_title_scroll(HudSurface& s);
    void draw_car_select(HudSurface& s);
    void draw_track_select(HudSurface& s);
    void draw_name_entry(HudSurface& s);
    void draw_records(HudSurface& s);
    void setup_records_page();
    int option(int item) const;

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
    float letter_ = 0;        // DAT_0011d238
    int tracks_seen_ = 0;     // DAT_00120818
    uint32_t frame_ = 0;      // g_frame_counter (blinking)
    ChoiceFile choice_;       // records and what this menu does not use itself
    int records_track_ = 0;   // DAT_00120988
    int records_row_ = 0;     // DAT_0012098c
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
