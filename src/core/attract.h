#pragma once
// The intro demo's overlay (attract_mode_overlay): the SCREAMER logo sliding in (LOGOL.SPR),
// the credits of ATTR.TXT page by page sliding across (ATTRL.FNT), and the fade at the end.
// A page is the lines up to a "#" line; each stays 4 seconds, then slides off for 4 more.
// The demo ends after the last page, or when a key (space, Enter, Esc) or the throttle is
// pressed: a one-second fade (attract_mode_fade_out), then the menu.

#include "core/framebuffer.h"
#include "core/hud.h"

#include <array>
#include <string>
#include <vector>

namespace scr {

class AttractOverlay {
public:
    // graf_dat: GFX/GRAF.DAT (ATTRL.FNT, LOGOL.SPR); local_dat: DATA/LOCAL.DAT (ATTR.TXT).
    bool load(const std::string& graf_dat, const std::string& local_dat);
    // frame_time in seconds; pow_row: SimClock::pow_table_row (lerp_toward)
    void update(float frame_time, int pow_row);
    void draw(Framebuffer& fb) const;
    void leave() { state_ |= 2; }      // a key: fade out
    bool done() const { return state_ == 4; }
    float brightness() const { return fade_; }  // 1 .. 0 while fading

private:
    HudFonts font_;
    HudSprites logo_;
    std::vector<std::string> lines_;
    int state_ = 0;        // DAT_0011ce40: 3 the credits are over, 2 fading, 4 done
    int first_ = 0;        // DAT_0011ce38: the page's first line
    int page_ = 0;         // DAT_0011ce50
    bool leaving_ = false; // DAT_0011ce3c
    float timer_ = 0;      // DAT_0011ce44
    float logo_x_ = -320;  // DAT_0011ce48
    float logo_y_ = 0;     // DAT_0011ce4c
    float fade_ = 1;       // DAT_0011d220
    std::array<float, 20> line_x_{};  // DAT_0011ce54
};

} // namespace scr
