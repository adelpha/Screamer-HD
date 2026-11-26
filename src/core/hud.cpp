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

// The game's sin/cos table (g_sincos_table): 2048 steps, 1.15 fixed point (0x7fff is 1.0),
// read as signed words with no shift in draw_map_objects. (Truncating x * 32768 gives all but
// a few dozen of its entries exactly, those one off.)
int table15(double v) { return std::clamp(int(v * 32768.0), -32768, 32767); }
int sin15(int a) { return table15(std::sin(double(a & 0x7ff) * 3.14159265358979 / 1024.0)); }
int cos15(int a) { return table15(std::cos(double(a & 0x7ff) * 3.14159265358979 / 1024.0)); }
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

HudSurface::HudSurface(Framebuffer& fb, int scale)
    : fb_(fb), w_(320 * std::max(1, scale)), h_(200 * std::max(1, scale)), sx_(float(fb.width()) / float(w_)),
      sy_(float(fb.height()) / float(h_)), ys_(size_t(h_) + 1) {
    // A wide picture (square pixels): the w_ columns keep their 4:3 shape, height * 4/3
    // pixels of the picture's width, and what is left over lies to the sides.
    const float sx43 = float(fb.height()) * (4.0f / 3.0f) / float(w_);
    if (fb.wide && sx43 < sx_) {
        sx_ = sx43;
        spare_ = fb.width() - int(float(w_) * sx_);
        pad_ = int(std::ceil(float(spare_) / sx_)) + 1;
    }
    // where each of the w_ x h_ pixels starts in the framebuffer (and, on a wide picture,
    // the columns off either side of them)
    xs_.resize(size_t(w_ + 2 * pad_) + 1);
    for (int x = -pad_; x <= w_ + pad_; ++x)
        xs_[size_t(x + pad_)] = x >= 0 ? int(float(x) * sx_) : int(std::floor(float(x) * sx_));
    for (int y = 0; y <= h_; ++y) ys_[size_t(y)] = int(float(y) * sy_);
    reset_clip();
}

int HudSurface::offset(int x) const {
    switch (anchor_) {
    case HudAnchor::Left: return 0;
    case HudAnchor::Right: return spare_;
    case HudAnchor::Sides: return x < w_ / 3 ? 0 : x < w_ - w_ / 3 ? spare_ / 2 : spare_;
    default: return spare_ / 2;
    }
}

void HudSurface::set_anchor(HudAnchor a) {
    anchor_ = a;
    reset_clip();
}

void HudSurface::reset_clip() {
    // everything of the picture that the w_ columns and those beside them cover
    clip_[0] = anchor_ == HudAnchor::Centre || anchor_ == HudAnchor::Right ? -pad_ : 0;
    clip_[2] = anchor_ == HudAnchor::Centre || anchor_ == HudAnchor::Left ? w_ + pad_ : w_;
    clip_[1] = 0;
    clip_[3] = h_;
}

void HudSurface::set_clip(int x0, int y0, int x1, int y1) {
    clip_[0] = std::max(0, x0);
    clip_[1] = std::max(0, y0);
    clip_[2] = std::min(w_, x1);
    clip_[3] = std::min(h_, y1);
}

void HudSurface::pixel(int x, int y, uint8_t c) {
    if (x < clip_[0] || y < clip_[1] || x >= clip_[2] || y >= clip_[3]) return;
    const int off = spare_ ? offset(x) : 0;
    const int xa = xs_[size_t(x + pad_)] + off;
    int x0 = std::max(0, xa), x1 = std::min(fb_.width(), std::max(xa + 1, xs_[size_t(x + pad_) + 1] + off));
    if (extend_ && x == 0) x0 = 0;
    if (extend_ && x == w_ - 1) x1 = fb_.width();
    const int y0 = ys_[size_t(y)], y1 = std::min(fb_.height(), std::max(y0 + 1, ys_[size_t(y) + 1]));
    if (x0 >= x1) return;
    for (int py = y0; py < y1; ++py) std::memset(&fb_.at(x0, py), c, size_t(x1 - x0));
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
        for (int32_t k = 0; k < n && i < p.size(); ++k, ++i, ++pos) pixel(pos % w_, pos / w_, p[i]);
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
    const int off = spare_ ? offset(x) : 0;
    const int x0 = int(float(x) * sx_) + off, x1 = int(float(x + w) * sx_) + off;
    const int y0 = int(float(y) * sy_), y1 = int(float(y + h) * sy_);
    const int xa = std::max(0, x0), xb = std::min(x1, fb_.width());
    const int ya = std::max(0, y0), yb = std::min(y1, fb_.height());
    if (xa >= xb) return;
    // (the table in a local: the bytes written could be anything as far as the compiler knows)
    const std::array<uint8_t, 256> t = table;
    for (int py = ya; py < yb; ++py) {
        uint8_t* const row = &fb_.at(xa, py);
        const int n = xb - xa;
        for (int i = 0; i < n; ++i) row[i] = t[row[i]];
    }
}

void HudSurface::darken_all(const std::array<uint8_t, 256>& table) {
    const std::array<uint8_t, 256> t = table;
    for (int y = 0; y < fb_.height(); ++y) {
        uint8_t* const row = &fb_.at(0, y);
        for (int x = 0; x < fb_.width(); ++x) row[x] = t[row[x]];
    }
}

bool Hud::load(const std::string& game_dat, bool hires) {
    auto a = Archive::open(game_dat);
    if (!a) return false;
    const char r = hires ? 'H' : 'L';
    auto fnt = a->read(std::string("GAME") + r + ".FNT");
    auto spr = a->read(std::string("GAME") + r + ".SPR");
    auto let = a->read(std::string("IMPLET_") + r + ".PKC");
    auto res = a->read(std::string("IMPRES_") + r + ".PKC");
    if (!fnt || !spr || !let || !res) return false;
    scale = hires ? 2 : 1;
    auto f = HudFonts::parse(*fnt);
    auto s = HudSprites::parse(*spr);
    if (!f || !s) return false;
    fonts = std::move(*f);
    sprites = std::move(*s);
    labels = std::move(*let);
    gauges = std::move(*res);
    if (auto cd = a->read(std::string("CDFRONT") + r + ".PKC")) cdfront = std::move(*cd);
    return true;
}

void Hud::draw_cd_panel(Framebuffer& fb, const CdPanel& p) {
    // pause_menu (STARTH.EXE: every position doubled)
    const int k = scale;
    HudSurface s(fb, k);
    s.rle(cdfront);
    // the CD's tracks (font 5, mode 9); the playing one flashes; a second row after ten
    int x = 0, y = 0;
    for (int i = 0; i <= p.last_track - p.first_track; ++i) {
        if (i == 10 && y == 0) {
            y = 7;
            x = 0;
        }
        const int n = p.first_track + i;
        if (n != p.track || !p.blink) s.text(fonts, (x + 0x40) * k, (y + 0x4e) * k, "" + std::to_string(n), 5, 9);
        x += 0xd;
    }
    // the selected button (x, sprite, sprite when pressed: table 0x101f1c)
    static constexpr int kButtons[7][3] = {{59, 7, 11},    {83, 8, 12},    {113, 9, 13}, {137, 10, 14},
                                           {167, 57, 59},  {167, 58, 60},  {217, 61, 62}};
    const int b = std::clamp(p.button, 0, 6);
    s.sprite(sprites, p.pressed ? kButtons[b][2] : kButtons[b][1], kButtons[b][0] * k, 0x61 * k);
    if (3 < b) {
        // hud_draw_message_box: the volume as a bar of lines, 3 pixels per 1024
        static const char* kLabels[3] = {"CD", "FX", "MU"};
        static constexpr uint8_t kColours[4] = {0x16, 0xd6, 0xd0, 0xd0};  // 0x101fa0
        const int m = std::clamp(p.mode, 0, 2);
        s.text(fonts, 0x40 * k, 0x45 * k, kLabels[m], 5, 3);
        for (int i = 0; i < p.volume[m] / 0x400; ++i) {
            const int lx = (i * 3 + 0x55) * k;
            s.line(lx, 0x45 * k, lx, 0x4a * k, kColours[std::min(i / 10, 3)]);
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
    const char r = scale > 1 ? 'H' : 'L';
    auto pkc = a->read("COCK" + id + r + ".PKC");
    if (!pkc) return false;
    auto values = DtrFile(cars_dtr).section(section + r);
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
    // to the tip; angle 0 points 2 radians anticlockwise of up. (STARTH.EXE doubles the pivot
    // and the length, the two points stay one pixel out.)
    cx *= scale;
    cy *= scale;
    len *= scale;
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
    // STARTH.EXE: the same, every position doubled for its 640x400 graphics
    const int k = scale;
    HudSurface s(fb, k);
    // On a wide picture (ours) everything stays by its own edge of the screen: lap and times
    // on the left, position and gauges on the right, the time left and messages in the middle.
    // hud_draw_cockpit: the labels, then the gauges of the chosen view.
    s.set_anchor(HudAnchor::Sides);
    s.rle(labels);
    s.set_anchor(HudAnchor::Centre);
    if (view == 1 && !cockpit.empty()) {
        // The dashboard: speed and rev needles (lerp k 0x5a), the gear as a sprite.
        // (wide: the dashboard in the middle, its outermost columns carried on to the edges)
        s.set_extend(true);
        s.rle(cockpit);
        s.set_extend(false);
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
        s.set_anchor(HudAnchor::Right);
        s.rle(gauges);
        char buf[16];
        std::snprintf(buf, sizeof buf, "\x01%03d", int(st.speed * 0.017f));  // km/h
        s.text(fonts, 0x10f * k, 0xbb * k, buf, 2, 7);
        std::snprintf(buf, sizeof buf, "\x01%d", st.gear);
        s.text(fonts, 0x130 * k, 0xbb * k, buf, 2, 1);
        float target = st.rpm / st.maxrpm * kPi;
        if (kPi < target) target = kPi;
        if (advance) rpm_needle_ = (rpm_needle_ - target) * pow_table(pow_row, 0x5a) + target;
        draw_needle(s, 0x10f, 0xa8, 0x1e, rpm_needle_, 0x75);
    }
    // hud_draw: time left (centred), lap n/N, position, three times.
    char buf[64];
    s.set_anchor(HudAnchor::Centre);
    std::snprintf(buf, sizeof buf, "%d", int(st.time_left));
    s.text(fonts, (s.width() - s.measure(fonts, buf, 6)) / 2, 8 * k, buf, 6, 2);
    s.set_anchor(HudAnchor::Left);
    std::snprintf(buf, sizeof buf, "%d%c", st.lap < st.laps ? st.lap + 1 : st.lap, ':');
    const int lap_w = s.measure(fonts, buf, 1);
    s.text(fonts, 8 * k, 0xb * k, buf, 1, 5);
    std::snprintf(buf, sizeof buf, "%d", st.laps);
    s.text(fonts, lap_w + 5 * k, 0x14 * k, buf, 4, 5);
    std::snprintf(buf, sizeof buf, "%d", st.position);
    s.set_anchor(HudAnchor::Right);
    s.text(fonts, 0x114 * k, 0xb * k, buf, 1, 5);
    s.set_anchor(HudAnchor::Left);
    s.text(fonts, 8 * k, 0x38 * k, clock_text(st.total_time), 3, 2);
    s.text(fonts, 8 * k, 0x48 * k, clock_text(st.lap_time), 3, 2);
    s.text(fonts, 8 * k, 0x58 * k, st.best_lap, 3, 2);
    // hud_draw_for_view: the radar.
    if (radar > 0) {
        const int zoom = (radar == 1 ? 0x18 : radar == 2 ? 0x30 : 0x48) * k;
        s.darken(4 * k, 0x7c * k, 0x48 * k, 0x48 * k, darken075);
        s.set_clip(4 * k, 0x7c * k, 0x4c * k, 0xc4 * k);
        const int yaw = int(st.player_yaw * 1024.0f / kPi);
        // the track lines at zoom / 32768 per unit (>> 2, then 1.15 sin/cos, then zoom >> 14),
        // the same scale as the CPU cars' dots below
        const int sn = sin15(yaw), cs = cos15(yaw);
        auto map = [&](int x, int z, int& ox, int& oy) {
            const int dx = (x - st.player_x) >> 2, dz = (z - st.player_z) >> 2;
            const int a = shr14(int64_t(dx) * cs), b = shr14(int64_t(dz) * sn);
            const int c = shr14(int64_t(dz) * cs - int64_t(dx) * sn);
            ox = 0x28 * k - shr14(int64_t(zoom) * (a + b));
            oy = shr14(int64_t(zoom) * c) + 0xa0 * k;
        };
        for (const auto& l : st.radar_lines) {
            int x0, y0, x1, y1;
            map(l[0], l[1], x0, y0);
            map(l[2], l[3], x1, y1);
            s.line(x0, y0, x1, y1, 0xff);
        }
        // (STARTH.EXE: its doubled zoom times 1.5259e-5, then the whole position times 2)
        const float dk = float(zoom / k) * 3.0518e-05f;
        const double c = std::cos(double(st.player_yaw)), sv = std::sin(double(st.player_yaw));
        for (const auto& p : st.cpu_cars) {
            const int dx = p[0] - st.player_x, dz = p[1] - st.player_z;
            const int rx = int(double(dz) * sv + double(dx) * c), rz = int(double(dz) * c - double(dx) * sv);
            s.sprite(sprites, 5, int((39.0f - float(rx) * dk) * float(k)), int((float(rz) * dk + 159.0f) * float(k)));
        }
        s.sprite(sprites, 4, 0x26 * k, 0x9e * k);
    }
    s.set_anchor(HudAnchor::Centre);
    // race_end_screen (race_end_draw_mode1, or "TIME OVER" when out of time)
    auto centred = [&](const std::string& t, int y) { s.text(fonts, (s.width() - s.measure(fonts, t, 0)) / 2, y * k, t, 0, 2); };
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
    if (st.countdown_stage >= 0) s.sprite(sprites, st.countdown_stage, 0x78 * k, 0x36 * k);
}

} // namespace scr
