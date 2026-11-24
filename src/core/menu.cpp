#include "core/menu.h"

#include "core/archive.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scr {

namespace {

constexpr float kPi = 3.141592741f;
constexpr float kToUnits = 1024.0f / 3.141592741f;

std::vector<std::string> text_lines(const Archive& a, const std::string& name) {
    std::vector<std::string> out;
    auto d = a.read(name);
    if (!d) return out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        if (!cur.empty() && cur[0] != '*') out.push_back(cur);
        cur.clear();
    };
    for (uint8_t c : *d) {
        if (c == '\n') flush();
        else cur.push_back(char(c));
    }
    flush();
    return out;
}

// The page tables at 0x102de4 (text index, action, value, max).
std::vector<std::vector<Menu::Entry>> default_pages() {
    return {
        {{0, 0, 0, 0}, {1, 1, 0, 0}, {2, 2, 0, 0}, {3, 3, 0, 0}, {4, 4, 0, 0}},
        {{5, 1234, 2, 2}, {8, 1234, 1, 2}, {13, 1234, 0, 3}, {17, 1234, 1, 1}, {19, 1234, 1, 1}, {21, 21, 0, 0}},
        {{3, 3, 0, 0}, {23, 23, 0, 0}},
        {{24, 24, 0, 0}, {25, 25, 0, 0}},
        {{28, 28, 0, 0}, {29, 29, 0, 0}, {30, 30, 0, 0}},
        {{26, 26, 0, 0}, {27, 27, 0, 0}},
        {{31, 31, 0, 0}, {32, 31, 0, 1}, {33, 31, 0, 2}, {34, 31, 0, 3}, {35, 31, 0, 4}, {36, 31, 0, 5}},
    };
}

// Fly-in variants (0x102218): start x, y, z and the spiral radii.
constexpr int kVariants[8][5] = {
    {200, 200, -4500, 1000, 1000}, {3000, -3000, 1500, 200, 800}, {1000, 1000, -8000, 3000, 3000},
    {3000, 0, 5000, 0, 0},         {2000, 50, 4500, 0, 0},        {0, 0, -10500, 500, 800},
    {-2000, 2000, 3500, 0, 0},     {50, 5000, 4500, 3000, 3000},
};

} // namespace

bool MenuAssets::load(const std::string& menu_dat, const std::string& graf_dat, const std::string& local_dat,
                      bool hires) {
    res = hires ? 'H' : 'L';
    graf_path = graf_dat;
    auto m = Archive::open(menu_dat);
    auto g = Archive::open(graf_dat);
    auto l = Archive::open(local_dat);
    if (!m || !g || !l) return false;
    auto mlb = m->read("MENU.MLB");
    auto col = m->read("MENU.COL");
    if (!mlb || !col || col->size() < 768) return false;
    scene.materials = MaterialLibrary::parse(std::move(*mlb));
    if (!scene.materials) return false;
    scene.palette = palette_from_vga(col->data());
    scene.darken = build_darken_table(col->data(), 0.6f);
    auto model = [&](const std::string& n) -> std::optional<Model> {
        auto b = m->read(n);
        return b ? parse_model(b->data(), b->size()) : std::nullopt;
    };
    auto let = model("ALETTER.3DX");
    if (!let) return false;
    letter = std::move(*let);
    for (int i = 0; i < 7; ++i) {
        char n[16];
        std::snprintf(n, sizeof n, "AAUTO%02d.3DX", i);
        if (auto c = model(n)) cars.push_back(std::move(*c));
    }
    for (int i = 1; i <= 6; ++i)
        if (auto c = model("AMAP" + std::to_string(i) + ".3DX")) maps.push_back(std::move(*c));
    const std::string r(1, res);
    if (auto f = g->read("MENU" + r + ".FNT")) {
        if (auto p = HudFonts::parse(*f)) fonts = std::move(*p);
    }
    if (auto s = g->read("MENU" + r + ".SPR")) {
        if (auto p = HudSprites::parse(*s)) sprites = std::move(*p);
    }
    texts = text_lines(*l, "MENU.TXT");
    cardat = text_lines(*l, "CARDAT.TXT");
    trackdat = text_lines(*l, "TRACKDAT.TXT");
    return !texts.empty();
}

const PcxImage* MenuAssets::backdrop(const std::string& name) {
    const std::string file = name + std::string(1, res) + ".PCX";
    if (auto it = backdrops_.find(file); it != backdrops_.end()) return &it->second;
    auto g = Archive::open(graf_path);
    if (!g) return nullptr;
    auto d = g->read(file);
    if (!d) return nullptr;
    auto img = decode_pcx(*d);
    if (!img) return nullptr;
    return &backdrops_.emplace(file, std::move(*img)).first->second;
}

Menu::Menu(MenuAssets& assets) : a_(assets), pages(default_pages()) {}

void Menu::show_title() {
    // frontend_show_title
    const auto width = [&](int i) { return size_t(i) < a_.sprites.sprites.size() ? a_.sprites.sprites[size_t(i)].w : 0; };
    logo1_x_ = float(((320 - width(15)) >> 1) - 400);
    logo2_x_ = float(((320 - width(14)) >> 1) - 800);
    page_stack.clear();
    built_page_ = -1;
    fade_rate_ = 0.6f;
    cam_rate_ = 0.6f;
    brightness = 0;
    cam_pow_ = true;
    fade_dir_ = 1;
    from_[0] = from_[1] = 0;
    to_[0] = to_[1] = 0;
    from_[2] = to_[2] = 0x1004;
    wave_ = 0;
    cam_pitch_ = cam_yaw_from_ = cam_yaw_to_ = cam_roll_ = 0;
    cam_t_ = 0;
    flipping_ = 0;
    for (Item& it : items) it.ch = -1;
    sounds.push_back(0);
}

void Menu::draw_text(const std::string& s) {
    // menu_item_draw_text: letters 0x280 apart, centred on the page, rows 0x180 apart.
    int x;
    if (block_.centred == 0) {
        x = 0x1200 - block_.start * 0x280;
    } else {
        const int len = int(s.size());
        x = 0x1200 - int(unsigned((len - 1) * -0x280 + 0x2400) >> 1);
    }
    const int y = block_.ypos * -0x180 + 0x820;
    size_t slot = 0;
    for (unsigned char c : s) {
        if (c < 0x20 || c > 0x7f) return;
        while (slot < items.size() && items[slot].ch != -1) ++slot;
        if (slot >= items.size()) return;
        Item& it = items[slot++];
        it.ch = c - 0x20;
        it.row = block_.row;
        it.x1 = x;
        it.x0 = block_.x0 ? block_.x0 : x;
        x -= 0x280;
        it.y1 = y;
        it.y0 = block_.y0 ? block_.y0 : y;
        it.z1 = 0;
        it.z0 = block_.z0;
        it.turned = block_.turned;
        it.angle = block_.turned ? kPi : 0.0f;
        it.ay = it.az = 0;
        it.sx = block_.sx;
        it.sy = block_.sy;
        it.k = 1;
        it.speed = 1;
        it.timer = block_.phase;
        block_.phase += 0.01f;
        // The next letter flies in from a start point turned by 0.09 rad.
        const double c0 = std::cos(double(block_.spin)), s0 = std::sin(double(block_.spin));
        const int px = block_.x0, py = block_.y0;
        block_.x0 = int(double(py) * s0 + double(px) * c0);
        block_.y0 = int(double(py) * c0 - double(px) * s0);
    }
}

void Menu::update_item_static(Item& it) {
    // menu_item_update: straight to the place on the page.
    if (it.ch == -1) return;
    it.ix = it.x1;
    it.iy = it.y1;
    it.iz = it.z1;
    if (it.row == selected[size_t(page)]) it.angle = kPi / 2;
    it.iax = it.angle;
    it.iay = it.ay;
    it.iaz = it.az;
    it.k = 0;
    it.timer = 0;
}

void Menu::update_item_zoom(Item& it, float dt) {
    // menu_item_update_zoom
    if (it.ch == -1) return;
    it.timer -= dt;
    if (it.timer <= 0) it.timer = 0;
    const double k = it.k;
    const double ease = 1.0 - std::cos(k * 1.570796371f);
    it.ix = int(std::cos(k * 6.283185482f) * double(float(it.sx) * it.k) + ease * double(it.x0 - it.x1) + double(it.x1));
    it.iy = int(std::sin(k * 6.283185482f) * double(float(it.sy) * it.k) + ease * double(it.y0 - it.y1) + double(it.y1));
    it.iz = int(ease * double(it.z0 - it.z1) + double(it.z1));
    float px = 0.5f;
    if (it.turned) px = (float(it.ix) + 4608.0f) * float(0.0001085069444444444);
    float py = (float(it.iy) + 2080.0f) * float(0.0002403846153846154);
    if (it.turned == 0) py = py * kPi + wave_ * 0.25f;
    else py = py * kPi + wave_;
    it.iz = int(std::sin(double(px * 6.283185482f + py)) * 150.0 + double(it.iz));
    it.iax = it.angle;
    it.iay = it.ay;
    it.iaz = it.az;
    if (it.k != 0) {
        it.k -= dt * it.speed;
        if (it.k < 0) it.k = 0;
    }
    if (it.turned == 0 && flipping_ == 0) {
        if (it.angle != 0) {
            it.angle -= dt * 12.566370964f;
            if (it.angle < 0.05f) it.angle = 0;
        }
    } else if (it.angle != kPi) {
        if (flipping_ && 1.570796371f < it.angle) built_page_ = -2;
        it.angle = dt * 12.566370964f + it.angle;
        if (kPi < it.angle) it.angle = kPi;
    }
}

void Menu::build_page_items(float dt) {
    // menu_build_page_items
    const auto& entries = pages[size_t(page)];
    if (built_page_ != page) {
        for (Item& it : items) it.ch = -1;
        block_.row = 0;
        block_.ypos = 8 - int(entries.size());
        block_.centred = 1;
        block_.phase = 0;
        block_.x0 = kVariants[variant_][0];
        block_.y0 = kVariants[variant_][1];
        block_.z0 = kVariants[variant_][2];
        block_.spin = 0.09f;
        block_.sx = kVariants[variant_][3];
        block_.sy = kVariants[variant_][4];
        for (size_t i = 0; i < entries.size(); ++i) {
            block_.turned = int(i) != selected[size_t(page)];
            const int t = entries[i].text + entries[i].value;
            draw_text(size_t(t) < a_.texts.size() ? a_.texts[size_t(t)] : std::string());
            ++block_.row;
            block_.ypos += 2;
        }
        if (++variant_ > 7) variant_ = 0;
    }
    // menu_hide_page_items: the selected line faces front
    for (Item& it : items) it.turned = it.row == selected[size_t(page)] ? 0 : 1;
    wave_ = dt * 9.42477798f + wave_;
    if (25.132741928f < wave_) wave_ = 0;
    if (built_page_ == -2) {
        // after an option changed: the new text straight in place, the line flips back
        for (Item& it : items) update_item_static(it);
        built_page_ = page;
        flipping_ = 0;
    } else {
        for (Item& it : items) update_item_zoom(it, dt);
        if (built_page_ != -2) built_page_ = page;
    }
}

void Menu::activate() {
    // frontend_activate_item (main pages)
    const auto& entries = pages[size_t(page)];
    const int sel = selected[size_t(page)];
    if (sel < 0 || size_t(sel) >= entries.size()) return;
    const int action = entries[size_t(sel)].action;
    auto push = [&](int to) {
        page_stack.push_back(page);
        page = to;
        sounds.push_back(0);
    };
    switch (action) {
    case 1: push(5); break;   // championship
    case 2: push(4); break;   // special modes
    case 4: push(1); break;   // options
    case 21: push(6); break;  // view records
    case 1234:
        // menu_cycle_option
        if (flipping_ == 0) {
            Entry& e = pages[size_t(page)][size_t(sel)];
            if (e.max < ++e.value) e.value = 0;
            flipping_ = 1;
        }
        sounds.push_back(2);
        break;
    default: sounds.push_back(3); break;  // races: not ported yet
    }
}

void Menu::handle_keys(const MenuKeys& k) {
    // frontend_handle_keys (page 0 kind): one action per key press
    const int key = k.esc ? 0x20 : k.enter ? 0x10 : k.down ? 2 : k.up ? 1 : 0;
    if (key == 0 || key == last_key_) {
        last_key_ = key;
        return;
    }
    last_key_ = key;
    int& sel = selected[size_t(page)];
    if (key == 0x20) {
        if (page_stack.empty()) {
            quit = true;
            return;
        }
        page = page_stack.back();
        page_stack.pop_back();
        sounds.push_back(0);
    } else if (key == 0x10) {
        activate();
    } else if (key == 1) {
        if (sel != 0) {
            --sel;
            sounds.push_back(1);
        }
    } else if (key == 2) {
        if (sel < int(pages[size_t(page)].size()) - 1) {
            ++sel;
            sounds.push_back(1);
        }
    }
}

void Menu::update_camera(float dt) {
    // frontend_update_camera: yaw by the shortest way, everything blended by f (t^4 or t).
    int yf = int(cam_yaw_from_ * kToUnits) & 0x7ff, yt = int(cam_yaw_to_ * kToUnits) & 0x7ff;
    if (yf < yt && 0x400 <= yt - yf) yf += 0x800;
    if (yt < yf && 0x400 <= yf - yt) yt += 0x800;
    const float f = cam_pow_ ? float(std::pow(double(cam_t_), 4.0)) : cam_t_;
    cam_.pitch = float(int(cam_pitch_ * kToUnits) & 0x7ff);
    cam_.roll = float(int(cam_roll_ * kToUnits) & 0x7ff);
    cam_.yaw = float(int(float(yf - yt) * f + float(yt)));
    cam_.x = float(int(float(from_[0] - to_[0]) * f + float(to_[0])));
    cam_.y = float(int(float(from_[1] - to_[1]) * f + float(to_[1])));
    cam_.z = float(int(float(from_[2] - to_[2]) * f + float(to_[2])));
    if (cam_t_ != 0) {
        cam_t_ -= dt * cam_rate_;
        if (cam_t_ < 0) {
            std::memcpy(from_, to_, sizeof from_);
            cam_yaw_from_ = cam_yaw_to_;
            cam_t_ = 0;
        }
    }
}

void Menu::fade(float dt) {
    // frontend_fade_update
    if (fade_dir_ == 1) {
        brightness += dt * fade_rate_;
        if (1.0f <= brightness) {
            brightness = 1;
            fade_dir_ = 0;
        }
    } else if (fade_dir_ == -1) {
        brightness -= dt * fade_rate_;
        if (brightness < 0) {
            brightness = 0;
            fade_dir_ = 0;
        }
    }
}

void Menu::update(const MenuKeys& keys, float dt) {
    sounds.clear();
    handle_keys(keys);
    update_camera(dt);
    build_page_items(dt);
    // menu_draw_title_scroll: the logos slide in
    const auto width = [&](int i) { return size_t(i) < a_.sprites.sprites.size() ? a_.sprites.sprites[size_t(i)].w : 0; };
    const float l1 = float((320 - width(15)) >> 1), l2 = float((320 - width(14)) >> 1);
    if (logo1_x_ < l1) logo1_x_ = dt * 350.0f + logo1_x_;
    if (l1 < logo1_x_) logo1_x_ = l1;
    if (logo2_x_ < l2) logo2_x_ = dt * 700.0f + logo2_x_;
    if (l2 < logo2_x_) logo2_x_ = l2;
    fade(dt);
}

void Menu::draw(Framebuffer& fb, Renderer& renderer) {
    const PcxImage* bg = a_.backdrop("MENU1");
    cam_.center_y = 100.0f;
    renderer.begin(a_.scene, cam_, fb, 0);
    if (bg) {
        // the backdrop (scaled to the framebuffer when it is not 320x200)
        for (int y = 0; y < fb.height(); ++y)
            for (int x = 0; x < fb.width(); ++x)
                fb.at(x, y) = bg->pixels[size_t(y * bg->height / fb.height()) * size_t(bg->width) + size_t(x * bg->width / fb.width())];
        fb.palette() = bg->palette();
    }
    for (const Item& it : items) {
        if (it.ch == -1) continue;
        Pose p;
        p.x = float(it.ix);
        p.y = float(it.iy);
        p.z = float(it.iz);
        p.ax = float(int(it.iax * kToUnits) & 0x7ff);
        p.ay = float(int(it.iay * kToUnits) & 0x7ff);
        p.az = float(int(it.iaz * kToUnits) & 0x7ff);
        p.animation = 0x35e * it.ch + 0x226;
        renderer.draw_model(a_.letter, p);
    }
    renderer.finish();
    HudSurface s(fb);
    s.sprite(a_.sprites, 15, int(logo1_x_ + 4.0f), 0);
    s.sprite(a_.sprites, 14, int(logo2_x_), 0x10);
    // the palette fade
    for (auto& c : fb.palette()) {
        c.r = uint8_t(float(c.r) * brightness);
        c.g = uint8_t(float(c.g) * brightness);
        c.b = uint8_t(float(c.b) * brightness);
    }
}

} // namespace scr
