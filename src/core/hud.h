#pragma once
// The race HUD (hud_draw and friends in STARTL.EXE) and the 2D graphics formats it uses,
// all from GFX/GAME.DAT:
//
//   GAMEL.FNT  uint8 font count - 1; per font { int32 height, first char, char count };
//              per font char count * { int32 width, int32 data offset }; int32 data size, data.
//              A glyph is `height` rows of (width + 1) / 2 bytes, two 4-bit pixels per byte
//              (high nibble first); a pixel p != 0 is drawn as colour p | mode << 4.
//   GAMEL.SPR  int32 count, int32 data size; count * { int32 x offset, y offset, width, height,
//              data offset }; data: width * height bytes per sprite, 0 transparent.
//              0-3 start lights, 4/5 radar dots (player / CPU car), 16+ cockpit gear digits.
//   *.PKC      run-length overlays for the whole 320x200 screen (rle_copy): int32 n > 0 copy
//              n bytes, n < 0 skip -n bytes, 0 ends. IMPLET_L: the labels (LAP, POSITION, TOTAL,
//              ...), IMPRES_L: rev counter and speed/gear boxes, COCKnn[A|B]L: dashboards.
//
// Everything is laid out for 320x200; HudSurface maps those coordinates onto a framebuffer of
// any size (nearest pixel).

#include "core/framebuffer.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scr {

struct HudFont {
    int height = 0, first = 0, count = 0;
    std::vector<std::array<int, 2>> glyphs;  // width, data offset
};

class HudFonts {
public:
    static std::optional<HudFonts> parse(const std::vector<uint8_t>& fnt);
    std::vector<HudFont> fonts;
    std::vector<uint8_t> data;
};

struct HudSprite {
    int x = 0, y = 0, w = 0, h = 0;
    uint32_t offset = 0;
};

class HudSprites {
public:
    static std::optional<HudSprites> parse(const std::vector<uint8_t>& spr);
    std::vector<HudSprite> sprites;
    std::vector<uint8_t> data;
};

// Draws in the original's 320x200 coordinates onto a framebuffer of any size.
class HudSurface {
public:
    explicit HudSurface(Framebuffer& fb);
    void set_clip(int x0, int y0, int x1, int y1);  // 320x200 units, end exclusive
    void reset_clip() { set_clip(0, 0, 320, 200); }
    void pixel(int x, int y, uint8_t c);
    void line(int x0, int y0, int x1, int y1, uint8_t c);  // draw_clipped_line
    void rle(const std::vector<uint8_t>& pkc);               // rle_copy
    void sprite(const HudSprites& s, int index, int x, int y);
    // font_draw_string: returns the x after the text.
    int text(const HudFonts& f, int x, int y, const std::string& s, int font, int mode);
    int measure(const HudFonts& f, const std::string& s, int font) const;
    void darken(int x, int y, int w, int h, const std::array<uint8_t, 256>& table);  // draw_darkened_rect

private:
    Framebuffer& fb_;
    float sx_, sy_;
    int clip_[4]{0, 0, 320, 200};
};

// Cockpit layout from CARS.DTR (COCKnnL): needle length, gear sprite base, pivots, gear spots.
struct CockpitLayout {
    int needle_len = 24, gear_sprite = 16;
    int speed_x = 0, speed_y = 0, rpm_x = 0, rpm_y = 0;
    std::array<std::array<int, 2>, 7> gear{};
};

// What the HUD shows, filled from the race each frame.
struct HudState {
    int lap = 0, laps = 3;               // g_player_car.lap, DAT_0011d2ac + 1
    int position = 1;
    float time_left = 0;                 // big number at the top
    float total_time = 0;                // TOTAL
    float lap_time = 0;                  // LAP TIME
    std::string best_lap = "00:00:00";   // BEST LAP
    float speed = 0, maxspd = 1, rpm = 0, maxrpm = 1;
    int gear = 0;
    int countdown_stage = -1;            // start lights 0..3, -1 when over
    int player_x = 0, player_z = 0;
    float player_yaw = 0;
    std::vector<std::array<int, 2>> cpu_cars;                       // positions for the radar
    std::vector<std::array<int, 4>> radar_lines;                    // track lines (x1, z1, x2, z2)
    // The end of the race (race_end_screen): 1 finished (won = first), 2 out of time.
    int end = 0;
    bool won = false;
    // race_end_screen picks the text by mode: 1/2 the position (mode 1), 4 / 0x20 / 0x40
    // "YOU MADE IT!" or "TRY AGAIN", and in cone carnage and slalom "PERFECT!" below when
    // every cone was hit / every gate passed without touching a cone.
    int mode = 1;
    bool perfect = false;
};

// The P-key panel (pause_menu): a car stereo (CDFRONTL.PKC) over the frozen race, with the
// CD's track numbers, one highlighted button (play, eject, previous, next, volume -, volume +,
// mode) and, on the volume buttons, a bar for the volume the mode selects (CD, FX, MU).
struct CdPanel {
    int button = 0;          // 0..6
    bool pressed = false;    // the button is being held / was just used
    int mode = 0;            // 0 CD, 1 FX (effects), 2 MU (MIDI music)
    int volume[3] = {0x6000, 0x6000, 0x6000};
    int first_track = 2, last_track = 8, track = 3;
    bool blink = false;      // the current track flashes
};

class Hud {
public:
    bool load(const std::string& game_dat);  // GFX/GAME.DAT
    // The dashboard of a car: COCK<id>L.PKC from GAME.DAT and the layout section from CARS.DTR.
    bool load_cockpit(const std::string& game_dat, const std::string& id, const std::string& cars_dtr,
                      const std::string& section);
    // GAME.TXT from DATA/LOCAL.DAT (text_load): "POSITION", "1ST".."10TH", "GREAT!", ...
    bool load_texts(const std::string& local_dat);
    std::vector<std::string> texts;
    std::vector<uint8_t> cockpit;
    CockpitLayout layout;
    // view: 1 dashboard (COCKnn), 2 compact gauges (IMPRES, the default); radar 0 (off) .. 3.
    void draw(Framebuffer& fb, const HudState& s, const std::array<uint8_t, 256>& darken075, float frame_time,
              int pow_row, int view = 2, int radar = 1);

    void draw_cd_panel(Framebuffer& fb, const CdPanel& p);
    std::vector<uint8_t> cdfront;  // CDFRONTL.PKC

    HudFonts fonts;
    HudSprites sprites;
    std::vector<uint8_t> labels, gauges;  // IMPLET_L.PKC, IMPRES_L.PKC

private:
    void draw_needle(HudSurface& s, int cx, int cy, int len, float angle, uint8_t color);
    float speed_needle_ = 0, rpm_needle_ = 0;
};

} // namespace scr
