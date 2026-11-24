#pragma once
// The front end (frontend_* and menu_* in STARTL.EXE): a 3D scene over a PCX backdrop.
//
// Every menu line is spelled with ALETTER.3DX blocks (GFX/MENU.DAT), one object per letter
// (up to 80); the letter is a texture frame: instance animation = 0x35e * (char - 0x20) + 0x226.
// A page builds its items with a fly-in start position (one of 8 variants, table 0x102218),
// then each frame the letters spiral in (k goes 1 -> 0), the selected line shows the front
// face, the others are turned over (angle pi) and all of them bob on a sine wave.
// The camera (g_camera) blends between two positions with t^4 (title) or t.
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

struct MenuAssets {
    // gen_dat: DATA/GEN.DAT, local_dat: DATA/LOCAL.DAT, menu_dat: GFX/MENU.DAT,
    // graf_dat: GFX/GRAF.DAT. hires: the 640x400 sprites, fonts and backdrops (*H).
    bool load(const std::string& menu_dat, const std::string& graf_dat, const std::string& local_dat,
              bool hires = false);
    const PcxImage* backdrop(const std::string& name);  // e.g. "MENU1" -> MENU1L.PCX

    Track scene;           // materials (MENU.MLB) and the 3D objects for the renderer
    Model letter;          // ALETTER.3DX
    std::vector<Model> cars;   // AAUTO00..06 (CARS order)
    std::vector<Model> maps;   // AMAP1..6
    HudFonts fonts;        // MENUL.FNT
    HudSprites sprites;    // MENUL.SPR
    std::vector<std::string> texts;   // MENU.TXT
    std::vector<std::string> cardat;  // CARDAT.TXT
    std::vector<std::string> trackdat;  // TRACKDAT.TXT
    std::string graf_path;
    char res = 'L';

private:
    std::map<std::string, PcxImage> backdrops_;
};

// Keys the menu reacts to, as held this frame (frontend_handle_keys acts on changes).
struct MenuKeys {
    bool esc = false, enter = false, up = false, down = false, left = false, right = false;
};

class Menu {
public:
    explicit Menu(MenuAssets& assets);

    // frontend_show_title
    void show_title();
    // One frame of frontend_draw_frame (keys, camera, page) at frame time dt.
    void update(const MenuKeys& keys, float dt);
    void draw(Framebuffer& fb, Renderer& renderer);

    // Sound effects requested this frame (indices into MENU.DTR's SOUND_FX: menu1..menu9).
    std::vector<int> sounds;
    bool quit = false;       // Esc on the main page
    float brightness = 0;    // DAT_0011d220: the palette fade

    // Page state (names after what the globals hold).
    struct Entry {
        int text, action, value, max;
    };
    std::vector<std::vector<Entry>> pages;
    int page = 0;                 // DAT_0011fb5c
    std::array<int, 20> selected{};  // DAT_0011fb04: selected line per page
    std::vector<int> page_stack;  // DAT_0012034c

    struct Item {
        int row = 0;              // +0x04
        int ch = -1;              // +0x08: character - 0x20, -1 unused
        int turned = 0;           // +0x0c: 1 shows the back (not the selected line)
        float angle = 0;          // +0x10 rotation about x
        float ay = 0, az = 0;     // +0x14, +0x18
        int x0 = 0, y0 = 0, z0 = 0;   // +0x34.. fly-in start
        int x1 = 0, y1 = 0, z1 = 0;   // +0x40.. place on the page
        int sx = 0, sy = 0;       // +0x4c, +0x50 spiral radius
        float k = 1, speed = 1;   // +0x58 progress (1 -> 0), +0x5c
        float timer = 0;          // +0x60
        int ix = 0, iy = 0, iz = 0;  // the object instance's position
        float iax = 0, iay = 0, iaz = 0;
    };
    std::array<Item, 80> items{};

private:
    void build_page_items(float dt);
    void draw_text(const std::string& s);
    void update_item_zoom(Item& it, float dt);
    void update_item_static(Item& it);
    void handle_keys(const MenuKeys& keys);
    void activate();
    void update_camera(float dt);
    void fade(float dt);

    MenuAssets& a_;
    int built_page_ = -1;     // DAT_0011fb60 (-2: rebuild after an option change)
    int variant_ = 0;         // DAT_0011fab8
    int flipping_ = 0;        // DAT_0011fabc
    int last_key_ = 0;        // DAT_0011fb64
    float wave_ = 0;          // DAT_0011d244
    // draw_text's parameter block (0x11fad0)
    struct Block {
        int row = 0, ypos = 0, start = 0, centred = 1, turned = 0;
        int x0 = 0, y0 = 0, z0 = 0, sx = 0, sy = 0;
        float spin = 0.09f, phase = 0;
    } block_;
    // camera (from -> to with t)
    float cam_t_ = 0, cam_rate_ = 0.6f;
    bool cam_pow_ = true;          // DAT_0011d23c: t^4
    float cam_pitch_ = 0, cam_yaw_from_ = 0, cam_roll_ = 0, cam_yaw_to_ = 0;
    int from_[3]{}, to_[3]{};
    float fade_rate_ = 0.6f;
    int fade_dir_ = 0;            // DAT_0011fac4: 1 in, -1 out, 0 done
    // the two SCREAMER logos sliding in (DAT_00120820/24)
    float logo1_x_ = 0, logo2_x_ = 0;
    Camera cam_;
};

} // namespace scr
