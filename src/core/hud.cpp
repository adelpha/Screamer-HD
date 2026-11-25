#include "core/hud.h"

#include "core/archive.h"
#include "core/dtr.h"
#include "core/sim_clock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace scr {
namespace {

int32_t rd32(const std::vector<uint8_t>& d, size_t o) {
    int32_t v = 0;
    if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4);
    return v;
}

constexpr float kPi = 3.1415927410125732f;

std::string clock_text(float t) {
    // "%02d:%02d:%02d": minutes, seconds, hundredths (hud_draw)
    const int s = int(t);
    const int hundredths = int((t - float(s)) * 100.0f);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", s / 60, s % 60, hundredths);
    return buf;
}

// The game's sin/cos table: 2048 steps, 1.14 fixed point.
int sin14(int a) { return int(std::lrint(std::sin(double(a & 0x7ff) * 3.14159265358979 / 1024.0) * 16384.0)); }
int cos14(int a) { return int(std::lrint(std::cos(double(a & 0x7ff) * 3.14159265358979 / 1024.0) * 16384.0)); }
int shr14(int64_t v) {  // draw_map_objects: arithmetic >> 14, then one more down when negative
    int r = int(v >> 14);
    return r < 0 ? r - 1 : r;
}

} // namespace

std::optional<HudFonts> HudFonts::parse(const std::vector<uint8_t>& d) {
    if (d.empty()) return std::nullopt;
    HudFonts f;
    const int n = d[0] + 1;
    size_t o = 1;
    for (int i = 0; i < n; ++i, o += 12) f.fonts.push_back({rd32(d, o), rd32(d, o + 4), rd32(d, o + 8), {}});
    for (HudFont& font : f.fonts) {
        for (int c = 0; c < font.count; ++c, o += 8) font.glyphs.push_back({rd32(d, o), rd32(d, o + 4)});
    }
    const int32_t size = rd32(d, o);
    o += 4;
    if (size < 0 || o + size_t(size) > d.size()) return std::nullopt;
    f.data.assign(d.begin() + std::ptrdiff_t(o), d.begin() + std::ptrdiff_t(o + size_t(size)));
    return f;
}

std::optional<HudSprites> HudSprites::parse(const std::vector<uint8_t>& d) {
    const int32_t n = rd32(d, 0), size = rd32(d, 4);
    const size_t data_at = 8 + size_t(n) * 20;
    if (n <= 0 || size < 0 || data_at + size_t(size) > d.size()) return std::nullopt;
    HudSprites s;
    for (int32_t i = 0; i < n; ++i) {
        const size_t o = 8 + size_t(i) * 20;
        s.sprites.push_back({rd32(d, o), rd32(d, o + 4), rd32(d, o + 8), rd32(d, o + 12), uint32_t(rd32(d, o + 16))});
    }
    s.data.assign(d.begin() + std::ptrdiff_t(data_at), d.begin() + std::ptrdiff_t(data_at + size_t(size)));
    return s;
}

HudSurface::HudSurface(Framebuffer& fb) : fb_(fb), sx_(float(fb.width()) / 320.0f), sy_(float(fb.height()) / 200.0f) {}

void HudSurface::set_clip(int x0, int y0, int x1, int y1) {
    clip_[0] = std::max(0, x0);
    clip_[1] = std::max(0, y0);
    clip_[2] = std::min(320, x1);
    clip_[3] = std::min(200, y1);
}

void HudSurface::pixel(int x, int y, uint8_t c) {
    if (x < clip_[0] || y < clip_[1] || x >= clip_[2] || y >= clip_[3]) return;
    const int x0 = int(float(x) * sx_), x1 = std::max(x0 + 1, int(float(x + 1) * sx_));
    const int y0 = int(float(y) * sy_), y1 = std::max(y0 + 1, int(float(y + 1) * sy_));
    for (int py = y0; py < y1 && py < fb_.height(); ++py)
        for (int px = x0; px < x1 && px < fb_.width(); ++px) fb_.at(px, py) = c;
}

void HudSurface::line(int x0, int y0, int x1, int y1, uint8_t c) {
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const int stx = x0 < x1 ? 1 : -1, sty = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 4096; ++guard) {
        pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += stx; }
        if (e2 <= dx) { err += dx; y0 += sty; }
    }
}

void HudSurface::rle(const std::vector<uint8_t>& p) {
    size_t i = 0;
    int pos = 0;
    while (i + 4 <= p.size()) {
        const int32_t n = rd32(p, i);
        i += 4;
        if (n == 0) break;
        if (n < 0) {
            pos -= n;
            continue;
        }
        for (int32_t k = 0; k < n && i < p.size(); ++k, ++i, ++pos) pixel(pos % 320, pos / 320, p[i]);
    }
}

void HudSurface::sprite(const HudSprites& s, int index, int x, int y) {
    if (index < 0 || size_t(index) >= s.sprites.size()) return;
    const HudSprite& sp = s.sprites[size_t(index)];
    for (int r = 0; r < sp.h; ++r)
        for (int c = 0; c < sp.w; ++c) {
            const size_t o = size_t(sp.offset) + size_t(r) * size_t(sp.w) + size_t(c);
            if (o < s.data.size() && s.data[o]) pixel(x + sp.x + c, y + sp.y + r, s.data[o]);
        }
}

int HudSurface::text(const HudFonts& f, int x, int y, const std::string& str, int font, int mode) {
    if (font < 0 || size_t(font) >= f.fonts.size()) return x;
    const HudFont& fn = f.fonts[size_t(font)];
    int maxw = 1;
    for (const auto& g : fn.glyphs) maxw = std::max(maxw, g[0]);
    const int cell = maxw + 2;  // monospaced cells (control char 1)
    bool mono = false, pad = true;
    for (unsigned char ch : str) {
        const int c = int(ch);
        if (c < fn.first || c >= fn.first + fn.count) {
            if (c == 1) mono = true;
            else if (c == 2) mono = false;
            else if (c == 3) pad = false;
            else if (c == 4) pad = true;
            else if (c == ' ') x += cell / 2;
            continue;
        }
        const auto& g = fn.glyphs[size_t(c - fn.first)];
        const int w = g[0];
        int gx = x + (pad ? 1 : 0);
        if (mono) gx = x + (cell - (w + 1)) / 2;
        const int row_bytes = (w + 1) / 2;
        for (int r = 0; r < fn.height; ++r)
            for (int px = 0; px < w; ++px) {
                const size_t o = size_t(g[1]) + size_t(r) * size_t(row_bytes) + size_t(px / 2);
                if (o >= f.data.size()) continue;
                const int v = (px & 1) == 0 ? f.data[o] >> 4 : f.data[o] & 0xf;
                if (v) pixel(gx + px, y + r, uint8_t(v | (mode << 4)));
            }
        x += mono ? cell : w + 1 + (pad ? 1 : 0);
    }
    return x;
}

int HudSurface::measure(const HudFonts& f, const std::string& str, int font) const {
    if (font < 0 || size_t(font) >= f.fonts.size()) return 0;
    const HudFont& fn = f.fonts[size_t(font)];
    int maxw = 1;
    for (const auto& g : fn.glyphs) maxw = std::max(maxw, g[0]);
    int x = 0;
    bool mono = false;
    for (unsigned char ch : str) {
        const int c = int(ch);
        if (c < fn.first || c >= fn.first + fn.count) {
            if (c == 1) mono = true;
            if (c == 2) mono = false;
            if (c == ' ') x += (maxw + 2) / 2;
            continue;
        }
        x += mono ? maxw + 2 : fn.glyphs[size_t(c - fn.first)][0] + 2;
    }
    return x;
}

void HudSurface::darken(int x, int y, int w, int h, const std::array<uint8_t, 256>& table) {
    // In framebuffer pixels, so the 3D view underneath keeps its resolution.
    const int x0 = int(float(x) * sx_), x1 = int(float(x + w) * sx_);
    const int y0 = int(float(y) * sy_), y1 = int(float(y + h) * sy_);
    for (int py = std::max(0, y0); py < y1 && py < fb_.height(); ++py)
        for (int px = std::max(0, x0); px < x1 && px < fb_.width(); ++px) fb_.at(px, py) = table[fb_.at(px, py)];
}

bool Hud::load(const std::string& game_dat) {
    auto a = Archive::open(game_dat);
    if (!a) return false;
    auto fnt = a->read("GAMEL.FNT");
    auto spr = a->read("GAMEL.SPR");
    auto let = a->read("IMPLET_L.PKC");
    auto res = a->read("IMPRES_L.PKC");
    if (!fnt || !spr || !let || !res) return false;
    auto f = HudFonts::parse(*fnt);
    auto s = HudSprites::parse(*spr);
    if (!f || !s) return false;
    fonts = std::move(*f);
    sprites = std::move(*s);
    labels = std::move(*let);
    gauges = std::move(*res);
    if (auto cd = a->read("CDFRONTL.PKC")) cdfront = std::move(*cd);
    return true;
}

void Hud::draw_cd_panel(Framebuffer& fb, const CdPanel& p) {
    // pause_menu
    HudSurface s(fb);
    s.rle(cdfront);
    // the CD's tracks (font 5, mode 9); the playing one flashes; a second row after ten
    int x = 0, y = 0;
    for (int i = 0; i <= p.last_track - p.first_track; ++i) {
        if (i == 10 && y == 0) {
            y = 7;
            x = 0;
        }
        const int n = p.first_track + i;
        if (n != p.track || !p.blink) s.text(fonts, x + 0x40, y + 0x4e, "" + std::to_string(n), 5, 9);
        x += 0xd;
    }
    // the selected button (x, sprite, sprite when pressed: table 0x101f1c)
    static constexpr int kButtons[7][3] = {{59, 7, 11},    {83, 8, 12},    {113, 9, 13}, {137, 10, 14},
                                           {167, 57, 59},  {167, 58, 60},  {217, 61, 62}};
    const int b = std::clamp(p.button, 0, 6);
    s.sprite(sprites, p.pressed ? kButtons[b][2] : kButtons[b][1], kButtons[b][0], 0x61);
    if (3 < b) {
        // hud_draw_message_box: the volume as a bar of lines, 3 pixels per 1024
        static const char* kLabels[3] = {"CD", "FX", "MU"};
        static constexpr uint8_t kColours[4] = {0x16, 0xd6, 0xd0, 0xd0};  // 0x101fa0
        const int m = std::clamp(p.mode, 0, 2);
        s.text(fonts, 0x40, 0x45, kLabels[m], 5, 3);
        for (int i = 0; i < p.volume[m] / 0x400; ++i) {
            const int lx = i * 3 + 0x55;
            s.line(lx, 0x45, lx, 0x4a, kColours[std::min(i / 10, 3)]);
        }
    }
}

bool Hud::load_texts(const std::string& local_dat) {
    auto a = Archive::open(local_dat);
    auto t = a ? a->read("GAME.TXT") : std::nullopt;
    if (!t) return false;
    texts.clear();
    std::string line;
    for (char c : *t) {
        if (c == 13) continue;
        if (c == 10) {
            if (line.empty() || line[0] != '*') texts.push_back(line);
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty() && line[0] != '*') texts.push_back(line);
    return true;
}

bool Hud::load_cockpit(const std::string& game_dat, const std::string& id, const std::string& cars_dtr,
                       const std::string& section) {
    auto a = Archive::open(game_dat);
    if (!a) return false;
    auto pkc = a->read("COCK" + id + "L.PKC");
    if (!pkc) return false;
    auto values = DtrFile(cars_dtr).section(section);
    if (!values || values->size() < 29) return false;
    auto v = [&](size_t i) { return (*values)[i].i; };
    layout.needle_len = v(0);
    layout.gear_sprite = v(1);
    layout.speed_x = v(2); layout.speed_y = v(3);
    layout.rpm_x = v(5); layout.rpm_y = v(6);
    for (size_t g = 0; g < 7; ++g) layout.gear[g] = {v(8 + 3 * g), v(9 + 3 * g)};
    cockpit = std::move(*pkc);
    return true;
}

void Hud::draw_needle(HudSurface& s, int cx, int cy, int len, float angle, uint8_t color) {
    // hud_draw_needle: a line from the pivot and from two points one pixel either side of it
    // to the tip; angle 0 points 2 radians anticlockwise of up.
    float a = angle - 2.0f;
    const int tx = int(std::sin(double(a)) * len + cx), ty = int(double(cy) - std::cos(double(a)) * len);
    a += 1.5707963705062866f;
    const int ax = int(std::sin(double(a)) + cx), ay = int(double(cy) - std::cos(double(a)));
    a += -kPi;
    const int bx = int(std::sin(double(a)) + cx), by = int(double(cy) - std::cos(double(a)));
    s.line(cx, cy, tx, ty, color);
    s.line(ax, ay, tx, ty, color);
    s.line(bx, by, tx, ty, color);
}

void Hud::draw(Framebuffer& fb, const HudState& st, const std::array<uint8_t, 256>& darken075, float frame_time,
               int pow_row, int view, int radar, bool advance) {
    (void)frame_time;
    HudSurface s(fb);
    // hud_draw_cockpit: the labels, then the gauges of the chosen view.
    s.rle(labels);
    if (view == 1 && !cockpit.empty()) {
        // The dashboard: speed and rev needles (lerp k 0x5a), the gear as a sprite.
        s.rle(cockpit);
        const float speed_target = st.speed / st.maxspd * 4.084070563316345f;
        if (advance) speed_needle_ = (speed_needle_ - speed_target) * pow_table(pow_row, 0x5a) + speed_target;
        draw_needle(s, layout.speed_x, layout.speed_y, layout.needle_len, speed_needle_, 0xff);
        float rpm_target = st.rpm / st.maxrpm * 3.612831652164459f;
        if (3.612831652164459f < rpm_target) rpm_target = 3.612831652164459f;
        if (advance) rpm_needle_ = (rpm_needle_ - rpm_target) * pow_table(pow_row, 0x5a) + rpm_target;
        draw_needle(s, layout.rpm_x, layout.rpm_y, layout.needle_len, rpm_needle_, 0xff);
        const size_t g = size_t(std::clamp(st.gear, 0, 6));
        s.sprite(sprites, st.gear + layout.gear_sprite, layout.gear[g][0], layout.gear[g][1]);
    } else if (view == 2) {
        s.rle(gauges);
        char buf[16];
        std::snprintf(buf, sizeof buf, "\x01%03d", int(st.speed * 0.017f));  // km/h
        s.text(fonts, 0x10f, 0xbb, buf, 2, 7);
        std::snprintf(buf, sizeof buf, "\x01%d", st.gear);
        s.text(fonts, 0x130, 0xbb, buf, 2, 1);
        float target = st.rpm / st.maxrpm * kPi;
        if (kPi < target) target = kPi;
        if (advance) rpm_needle_ = (rpm_needle_ - target) * pow_table(pow_row, 0x5a) + target;
        draw_needle(s, 0x10f, 0xa8, 0x1e, rpm_needle_, 0x75);
    }
    // hud_draw: time left (centred), lap n/N, position, three times.
    char buf[64];
    std::snprintf(buf, sizeof buf, "%d", int(st.time_left));
    s.text(fonts, (320 - s.measure(fonts, buf, 6)) / 2, 8, buf, 6, 2);
    std::snprintf(buf, sizeof buf, "%d%c", st.lap < st.laps ? st.lap + 1 : st.lap, ':');
    const int lap_w = s.measure(fonts, buf, 1);
    s.text(fonts, 8, 0xb, buf, 1, 5);
    std::snprintf(buf, sizeof buf, "%d", st.laps);
    s.text(fonts, lap_w + 5, 0x14, buf, 4, 5);
    std::snprintf(buf, sizeof buf, "%d", st.position);
    s.text(fonts, 0x114, 0xb, buf, 1, 5);
    s.text(fonts, 8, 0x38, clock_text(st.total_time), 3, 2);
    s.text(fonts, 8, 0x48, clock_text(st.lap_time), 3, 2);
    s.text(fonts, 8, 0x58, st.best_lap, 3, 2);
    // hud_draw_for_view: the radar.
    if (radar > 0) {
        const int zoom = radar == 1 ? 0x18 : radar == 2 ? 0x30 : 0x48;
        s.darken(4, 0x7c, 0x48, 0x48, darken075);
        s.set_clip(4, 0x7c, 0x4c, 0xc4);
        const int yaw = int(st.player_yaw * 1024.0f / kPi);
        const int sn = sin14(yaw), cs = cos14(yaw);
        auto map = [&](int x, int z, int& ox, int& oy) {
            const int dx = (x - st.player_x) >> 2, dz = (z - st.player_z) >> 2;
            const int a = shr14(int64_t(dx) * cs), b = shr14(int64_t(dz) * sn);
            const int c = shr14(int64_t(dz) * cs - int64_t(dx) * sn);
            ox = 0x28 - shr14(int64_t(zoom) * (a + b));
            oy = shr14(int64_t(zoom) * c) + 0xa0;
        };
        for (const auto& l : st.radar_lines) {
            int x0, y0, x1, y1;
            map(l[0], l[1], x0, y0);
            map(l[2], l[3], x1, y1);
            s.line(x0, y0, x1, y1, 0xff);
        }
        const float k = float(zoom) * 3.0518e-05f;
        const double c = std::cos(double(st.player_yaw)), sv = std::sin(double(st.player_yaw));
        for (const auto& p : st.cpu_cars) {
            const int dx = p[0] - st.player_x, dz = p[1] - st.player_z;
            const int rx = int(double(dz) * sv + double(dx) * c), rz = int(double(dz) * c - double(dx) * sv);
            s.sprite(sprites, 5, int(39.0f - float(rx) * k), int(float(rz) * k + 159.0f));
        }
        s.sprite(sprites, 4, 0x26, 0x9e);
        s.reset_clip();
    }
    // race_end_screen (race_end_draw_mode1, or "TIME OVER" when out of time)
    auto centred = [&](const std::string& t, int y) { s.text(fonts, (320 - s.measure(fonts, t, 0)) / 2, y, t, 0, 2); };
    if (st.end == 1 && (st.mode == 4 || st.mode == 0x20 || st.mode == 0x40) && texts.size() > 15) {
        // race_end_draw_attack / _carnage / _checkpoints
        centred(texts[st.won ? 14 : 13], 0x5a);
        if (st.perfect && st.mode != 4) centred(texts[15], 0x78);
    } else if (st.end == 1 && texts.size() > 11) {
        if (st.won) centred(texts[11], 0x3e);
        const size_t pos = size_t(std::clamp(st.position, 1, 10));
        centred(texts[pos] + " " + texts[0], 0x5a);
        centred(clock_text(st.total_time), 0x78);
    } else if (st.end == 2 && texts.size() > 22) {
        centred(texts[22], 0x5a);
    }
    // race_countdown_update: the start lights.
    if (st.countdown_stage >= 0) s.sprite(sprites, st.countdown_stage, 0x78, 0x36);
}

} // namespace scr
