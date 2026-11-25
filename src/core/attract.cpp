#include "core/attract.h"

#include "core/archive.h"
#include "core/sim_clock.h"

#include <algorithm>

namespace scr {

namespace {
float lerp_toward(float value, float target, int k, int pow_row) { return (value - target) * pow_table(pow_row, k) + target; }
} // namespace

bool AttractOverlay::load(const std::string& graf_dat, const std::string& local_dat) {
    auto g = Archive::open(graf_dat);
    auto l = Archive::open(local_dat);
    if (!g || !l) return false;
    auto fnt = g->read("ATTRL.FNT");
    auto spr = g->read("LOGOL.SPR");
    auto txt = l->read("ATTR.TXT");
    if (!fnt || !spr || !txt) return false;
    auto f = HudFonts::parse(*fnt);
    auto s = HudSprites::parse(*spr);
    if (!f || !s) return false;
    font_ = std::move(*f);
    logo_ = std::move(*s);
    // text_load: one line each, '*' lines are comments
    lines_.clear();
    std::string line;
    for (char c : *txt) {
        if (c == 13) continue;
        if (c == 10) {
            if (line.empty() || line[0] != '*') lines_.push_back(line);
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty() && line[0] != '*') lines_.push_back(line);
    // race_load_attract
    state_ = first_ = page_ = 0;
    leaving_ = false;
    timer_ = 0;
    logo_x_ = -320.0f;
    logo_y_ = 0;
    fade_ = 1.0f;
    line_x_.fill(350.0f);
    return !lines_.empty();
}

void AttractOverlay::update(float dt, int pow_row) {
    if (state_ != 3 && !lines_.empty()) {
        // attract_mode_draw_logo: the first two pages slide their own logo in at 4 and 28,
        // later pages the third one at 4 (and the last page's slides off to the left)
        if (page_ < 2) {
            logo_y_ = float(page_ * 0x18 + 4);
            logo_x_ = lerp_toward(logo_x_, float(-(int(leaving_) * 320)), 0x50, pow_row);
        }
        if ((page_ == 0x12 && leaving_) || 1 < page_) {
            logo_y_ = 4.0f;
            logo_x_ = lerp_toward(logo_x_, page_ == 0x12 && leaving_ ? -340.0f : 0.0f, page_ == 0x12 && leaving_ ? 0x50 : 0x5a, pow_row);
        }
        // attract_mode_scroll_text: the page's lines slide in, stay, slide out
        int i = 0;
        int k = first_;
        while (k < int(lines_.size()) && lines_[size_t(k)] != "#" && i < int(line_x_.size())) {
            if (line_x_[size_t(i)] < -359.0f) timer_ = 4.0f;
            else line_x_[size_t(i)] = float(int(lerp_toward(line_x_[size_t(i)], float(int(leaving_) * -400), 0x5a, pow_row)));
            ++i;
            ++k;
        }
        timer_ += dt;
        if (4.0f <= timer_) {
            timer_ = 0;
            leaving_ = !leaving_;
            if (!leaving_) {
                first_ = k + 1;  // past the "#"
                ++page_;
                line_x_.fill(350.0f);
            }
            if (first_ >= int(lines_.size())) {
                // the last page has gone: fade out (state 3 has the fade bit)
                state_ = 3;
                page_ = first_ = 0;
                line_x_.fill(350.0f);
            }
        }
    }
    if (state_ & 2) {
        // attract_mode_fade_out
        fade_ -= dt;
        if (fade_ <= 0.0f) {
            fade_ = 0.0f;
            state_ = 4;
        }
    }
}

void AttractOverlay::draw(Framebuffer& fb) const {
    if (state_ == 3 || lines_.empty()) return;
    HudSurface s(fb);
    const int frame = page_ < 2 ? page_ : 2;
    if (size_t(frame) < logo_.sprites.size()) {
        const int x = (320 - logo_.sprites[size_t(frame)].w) / 2 + int(logo_x_);
        s.sprite(logo_, frame, x, int(logo_y_));
    }
    int i = 0;
    for (int k = first_; k < int(lines_.size()) && lines_[size_t(k)] != "#" && i < int(line_x_.size()); ++k, ++i) {
        const int w = s.measure(font_, lines_[size_t(k)], 0);
        s.text(font_, int(line_x_[size_t(i)]) + ((0x140 - w) >> 1), i * 0x14, lines_[size_t(k)], 0, 2);
    }
}

} // namespace scr
