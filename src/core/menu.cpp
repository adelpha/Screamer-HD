#include "core/menu.h"

#include "core/archive.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace scr {

namespace {

constexpr float kPi = 3.141592741f;
constexpr float kToUnits = 1024.0f / 3.141592741f;
constexpr float kRingStep = 0.241660982f;  // 2 pi / 26

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

// MENU.TXT lines used by the pages.
enum Text : int {
    kAutomatic = 37, kMaxSpeed = 39, kGrip = 40, kAcceleration = 41, kCarSelection = 44, kTrackSelection = 45,
    kEnterName = 46, kLoading = 47, kNewTrack = 53,
};

// The lap counts of the LAP NUMBER option (0x1021e0).
constexpr int kLaps[4] = {3, 5, 10, 25};

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
    const auto cardat = text_lines(*l, "CARDAT.TXT");
    for (size_t i = 0; i + 6 <= cardat.size(); i += 6)
        car_info.push_back({cardat[i], std::atoi(cardat[i + 1].c_str()), std::atoi(cardat[i + 2].c_str()),
                            std::atoi(cardat[i + 3].c_str()), std::atoi(cardat[i + 4].c_str())});
    const auto trackdat = text_lines(*l, "TRACKDAT.TXT");
    for (size_t i = 0; i + 4 <= trackdat.size(); i += 4) track_names.push_back(trackdat[i]);
    return !texts.empty() && !cars.empty() && !maps.empty();
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

Menu::Menu(MenuAssets& assets) : pages(default_pages()), a_(assets) {}

int Menu::option(int item) const { return pages[1][size_t(item)].value; }

void Menu::hide_objects() {
    // frontend_hide_objects
    for (Item& it : items) it.ch = -1;
    for (Shown& s : car_models) s.visible = false;
    for (Shown& s : map_models) s.visible = false;
}

void Menu::show_title() {
    // frontend_show_title
    backdrop_ = a_.backdrop("MENU1");
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
    from_[0] = from_[1] = to_[0] = to_[1] = 0;
    from_[2] = to_[2] = 0x1004;
    wave_ = 0;
    cam_pitch_ = cam_yaw_from_ = cam_yaw_to_ = cam_roll_ = 0;
    cam_t_ = 0;
    flipping_ = 0;
    mode = draw_mode_ = 0;
    hide_objects();
    sounds.push_back(0);
}

void Menu::init_state(int next) {
    // frontend_init_state: fade out (and spin the camera) into the next mode
    fade_rate_ = 2.0f;
    cam_rate_ = 0.6f;
    cam_t_ = 1.0f;
    to_[2] = 20000;
    cam_roll_ = -12.566371f;
    fade_dir_ = -1;
    draw_mode_ = mode;
    next_mode_ = next;
    mode = 1;
}

void Menu::show_page_background() {
    // frontend_show_page_background
    switch (mode) {
    case 0: show_title(); break;
    case 2: setup_car_page(); break;
    case 3: setup_track_page(); break;
    case 4: setup_name_page(); break;
    default: break;
    }
}

void Menu::setup_car_page() {
    // frontend_screen_menu4: the cars in a row 4000 apart, the camera in front of the selected one
    backdrop_ = a_.backdrop("MENU4");
    cam_pow_ = true;
    fade_rate_ = 1.5f;
    cam_rate_ = 0.95f;
    fade_dir_ = 1;
    to_[2] = from_[2] = 0x1004;
    cam_pitch_ = -0.3926991f;
    from_[0] = to_[0] = car * -4000;
    from_[1] = to_[1] = 500;
    cam_yaw_from_ = cam_yaw_to_ = cam_roll_ = 0;
    cam_t_ = 0;
    hide_objects();
    for (size_t i = 0; i < car_models.size(); ++i) {
        Shown& s = car_models[i];
        s = {};
        s.x = int(i) * -4000;
        s.z = 0xb54;
        s.ay = 1.5707964f;
        s.visible = true;
    }
}

void Menu::setup_track_page() {
    // frontend_screen_menu3: the track models 40000 apart
    backdrop_ = a_.backdrop("MENU3");
    cam_pow_ = true;
    fade_rate_ = 1.5f;
    cam_rate_ = 0.95f;
    fade_dir_ = 1;
    from_[1] = to_[1] = 0x12c0;
    from_[2] = to_[2] = 13000;
    cam_yaw_from_ = cam_yaw_to_ = cam_roll_ = 0;
    cam_t_ = 0;
    from_[0] = to_[0] = track * -40000;
    cam_pitch_ = -0.6283185f;
    hide_objects();
    for (size_t i = 0; i < map_models.size(); ++i) {
        Shown& s = map_models[i];
        s = {};
        s.x = int(i) * -40000;
        s.z = 0xb54;
        s.ay = 1.5707964f;
        s.visible = true;
    }
}

void Menu::setup_name_page() {
    // frontend_screen_menu2: the alphabet as a ring of radius 3500, the camera 4500 out
    backdrop_ = a_.backdrop("MENU2");
    cam_pow_ = false;
    fade_rate_ = 1.5f;
    cam_rate_ = 8.0f;
    name_cursor_ = 0;
    letter_ = float(name.empty() ? 0 : std::clamp(name[0] - 'A', 0, 25));
    fade_dir_ = 1;
    if (letter_ == 0) {
        to_[0] = from_[0] = 0;
        cam_yaw_to_ = 0;
        to_[2] = 0x1194;
    } else {
        const double a = double(letter_ * -kRingStep);
        to_[0] = from_[0] = int(float(std::sin(a)) * 4500.0f);
        to_[2] = int(float(std::cos(a)) * 4500.0f);
        cam_yaw_to_ = letter_ * kRingStep;
    }
    to_[1] = from_[1] = 0x708;
    cam_pitch_ = -0.6283185f;
    cam_roll_ = 0;
    cam_yaw_from_ = cam_yaw_to_;
    from_[2] = to_[2];
    cam_t_ = 0;
    hide_objects();
    float px = 0, pz = 3500.0f;
    const float py = 160.0f;
    for (int i = 0; i < 26; ++i) {
        Item& it = items[size_t(i)];
        it.ch = i + 0x21;
        it.row = i + 0x41;
        it.x0 = int(px);
        it.x1 = it.x0;
        it.y0 = int(py);
        it.y1 = int(float((i & 1) * 500) + py * 16.0f);
        it.timer = float(double(i) * 0.04);
        it.z0 = int(pz);
        it.z1 = it.z0;
        it.angle = 2.2f;
        it.ay = float(i) * kRingStep;
        it.az = 0;
        it.turned = 1;
        it.k = 0;
        it.speed = 1.0f;
        // the next letter: the start point turned by one step about y
        const double a = double(float(i + 1) * -kRingStep);
        const float c = float(std::cos(a)), s = float(std::sin(a));
        px = 3500.0f * s;
        pz = 3500.0f * c;
    }
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
        // the next letter flies in from a start point turned by 0.09 rad
        const double c0 = std::cos(double(block_.spin)), s0 = std::sin(double(block_.spin));
        const int px = block_.x0, py = block_.y0;
        block_.x0 = int(double(py) * s0 + double(px) * c0);
        block_.y0 = int(double(py) * c0 - double(px) * s0);
    }
}

void Menu::update_item_static(Item& it) {
    // menu_item_update: straight to the place on the page
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

void Menu::update_item_slide(Item& it, float dt) {
    // menu_item_update_slide: the name ring's letters drop in and bounce
    if (it.ch == -1) return;
    it.timer -= dt;
    if (0 < it.timer) return;
    it.timer = 0;
    it.ix = it.x1;
    it.iy = int(std::pow(double(it.k), 2.0) * double(it.y0 - it.y1) + double(it.y1));
    it.iz = it.z1;
    it.iax = it.angle;
    it.iay = it.ay;
    it.iaz = it.az;
    if (it.speed != -1.0f) {
        if (it.speed != 0) {
            it.k = float(double(dt) * 2.5 + double(it.k));
            if (1.0f <= it.k) {
                it.k = 1;
                it.speed = 0;
                it.y1 = int(double(it.y1) * 0.4);  // the next bounce is lower
                if (it.y1 - it.y0 < 0) {
                    it.speed = -1;
                    it.y1 = it.y0;
                }
            }
        } else {
            it.k -= dt * 2.0f;
            if (it.k < 0) {
                it.k = 0;
                it.speed = 1;
            }
        }
    }
    if (it.turned) {
        if (it.angle != 2.199114918708801f) {
            it.angle = dt * 12.566370964f + it.angle;
            if (2.199114918708801f < it.angle) it.angle = 2.199114918708801f;
        }
    } else if (it.angle != -0.6283185482025146f) {
        it.angle -= dt * 12.566370964f;
        if (it.angle < -0.6283185482025146f) it.angle = -0.6283185482025146f;
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

void Menu::name_page_update(float dt) {
    // menu_letters_update: the selected letter tips towards the camera
    const int row = int(letter_) + 0x41;
    for (Item& it : items) it.turned = it.row == row ? 0 : 1;
    for (Item& it : items) update_item_slide(it, dt);
    if (letter_ == 0) {
        to_[0] = 0;
        cam_yaw_to_ = 0;
        to_[2] = 0x1194;
    } else {
        const double a = double(letter_ * -kRingStep);
        to_[0] = int(float(std::sin(a)) * 4500.0f);
        to_[2] = int(float(std::cos(a)) * 4500.0f);
        cam_yaw_to_ = letter_ * kRingStep;
    }
}

void Menu::start_race() {
    // frontend_load_attack_map / frontend_start_race for a normal game
    MenuRace r;
    r.game_mode = game_mode;
    r.map = track + 1;
    if (6 < r.map) {
        r.map -= 6;
        r.mirrored = true;
    }
    r.car = car;
    r.laps = kLaps[std::clamp(option(2), 0, 3)];
    r.level = option(1) == 3 ? 4 : option(1);
    r.high_detail = option(0) == 2;
    r.speech = option(3) == 1;
    r.music = option(4) == 1;
    r.name = name;
    race = r;
}

void Menu::activate() {
    // frontend_activate_item
    if (mode != 0) {
        if (mode == 2) {
            start_race();
        } else if (mode == 3) {
            draw_mode_ = mode;
            tracks_seen_ = tracks_unlocked;
            fade_dir_ = -1;
            brightness = 0;
            fade_rate_ = 1.5f;
            mode = 1;
            next_mode_ = 2;
        } else if (mode == 4) {
            draw_mode_ = mode;
            brightness = 0;
            fade_rate_ = 1.5f;
            next_mode_ = after_name_;
            fade_dir_ = -1;
            mode = 1;
        }
        return;
    }
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
    case 0:  // NORMAL GAME: name, track, car
        game_mode = 1;
        after_name_ = 3;
        init_state(4);
        sounds.push_back(3);
        break;
    case 1: push(5); break;   // championship (not ported yet)
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
    default: break;
    }
}

void Menu::handle_keys(const MenuKeys& k) {
    // frontend_handle_keys: one action per key press (the last key is remembered)
    const int key = k.esc ? 0x20 : k.enter ? 0x10 : k.down ? 2 : k.up ? 1 : k.left ? 4 : k.right ? 8 : 0;
    if (key == 0 || key == last_key_) {
        last_key_ = key;
        return;
    }
    if (key == 0x20) {
        last_key_ = key;
        switch (mode) {
        case 0:
            if (page_stack.empty()) {
                quit = true;
                return;
            }
            page = page_stack.back();
            page_stack.pop_back();
            sounds.push_back(0);
            break;
        case 2:  // back to the track page
            fade_dir_ = -1;
            brightness = 0;
            fade_rate_ = 1.5f;
            next_mode_ = 3;
            draw_mode_ = mode;
            mode = 1;
            break;
        case 3:  // back to the name page
            fade_dir_ = -1;
            brightness = 0;
            fade_rate_ = 1.5f;
            next_mode_ = 4;
            draw_mode_ = mode;
            mode = 1;
            tracks_seen_ = tracks_unlocked;
            break;
        default: {
            // frontend_return_to_title (the page stays where it was)
            const auto stack = page_stack;
            built_page_ = -1;
            brightness = 0;
            show_title();
            page_stack = stack;
            break;
        }
        }
        return;
    }
    if (key == 0x10) {
        last_key_ = key;
        activate();
        return;
    }
    if (key == 1 || key == 2) {
        last_key_ = key;
        if (mode == 0) {
            int& sel = selected[size_t(page)];
            if (key == 1 && sel != 0) {
                --sel;
                sounds.push_back(1);
            } else if (key == 2 && sel < int(pages[size_t(page)].size()) - 1) {
                ++sel;
                sounds.push_back(1);
            }
        } else if (mode == 4) {
            if (key == 1 && name_cursor_ != 0) {
                --name_cursor_;
                sounds.push_back(5);
            } else if (key == 2 && name_cursor_ < 2) {
                ++name_cursor_;
                sounds.push_back(5);
            }
        }
        return;
    }
    // left / right
    if (mode == 2 || mode == 3) {
        const bool left = key == 4;
        const int step = mode == 2 ? 4000 : 40000;
        int& sel = mode == 2 ? car : track;
        const int last = mode == 2 ? 0xb + cars_unlocked : tracks_unlocked + 2;
        if (cam_t_ < 0.8f && (left ? sel != 0 : sel < last)) {
            sounds.push_back(6);
            cam_t_ = 1.0f;
            last_key_ = key;
            from_[0] = int(cam_.x);
            to_[0] += left ? step : -step;
            sel += left ? -1 : 1;
        }
    } else if (mode == 4 && cam_t_ < 0.1f) {
        last_key_ = key;
        cam_yaw_from_ = cam_yaw_to_;
        std::memcpy(from_, to_, sizeof from_);
        letter_ += key == 8 ? 1.0f : -1.0f;
        if (25 < letter_) letter_ = 0;
        if (letter_ < 0) letter_ = 25;
        cam_t_ = 1.0f;
        if (name.size() < 3) name.resize(3, 'A');
        name[size_t(name_cursor_)] = char(int(letter_) + 'A');
    }
}

void Menu::update_camera(float dt) {
    // frontend_update_camera: yaw by the shortest way, everything blended by f (t^4 or t)
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
    ++frame_;
    handle_keys(keys);
    update_camera(dt);
    // frontend_update_page
    switch (mode) {
    case 0: build_page_items(dt); break;
    case 1:
        if (from_[2] != to_[2]) cam_roll_ = dt * kPi + cam_roll_;
        if (fade_dir_ == 0) {
            mode = draw_mode_ = next_mode_;
            show_page_background();
        }
        break;
    case 2:
        for (Shown& s : car_models) {
            s.ay = dt * 1.570796371f + s.ay;
            if (6.283185482f <= s.ay) s.ay = 0;
        }
        break;
    case 3:
        for (Shown& s : map_models) {
            s.ay = dt * 0.785398185f + s.ay;
            if (6.283185482f <= s.ay) s.ay = 0;
        }
        break;
    case 4: name_page_update(dt); break;
    default: break;
    }
    if (draw_mode_ == 0) {
        // menu_draw_title_scroll: the logos slide in
        const auto width = [&](int i) { return size_t(i) < a_.sprites.sprites.size() ? a_.sprites.sprites[size_t(i)].w : 0; };
        const float l1 = float((320 - width(15)) >> 1), l2 = float((320 - width(14)) >> 1);
        if (logo1_x_ < l1) logo1_x_ = dt * 350.0f + logo1_x_;
        if (l1 < logo1_x_) logo1_x_ = l1;
        if (logo2_x_ < l2) logo2_x_ = dt * 700.0f + logo2_x_;
        if (l2 < logo2_x_) logo2_x_ = l2;
    }
    fade(dt);
}

void Menu::draw_title_scroll(HudSurface& s) {
    s.sprite(a_.sprites, 15, int(logo1_x_ + 4.0f), 0);
    s.sprite(a_.sprites, 14, int(logo2_x_), 0x10);
}

void Menu::draw_car_select(HudSurface& s) {
    // menu_draw_car_select
    const auto text = [&](int i) { return size_t(i) < a_.texts.size() ? a_.texts[size_t(i)] : std::string(); };
    const auto centre = [&](const std::string& t, int y, int font, int m) {
        s.text(a_.fonts, (320 - s.measure(a_.fonts, t, font)) >> 1, y, t, font, m);
    };
    const int h0 = a_.fonts.fonts.empty() ? 8 : a_.fonts.fonts[0].height;
    const int h1 = a_.fonts.fonts.size() > 1 ? a_.fonts.fonts[1].height : 8;
    centre(text(kCarSelection), 2, 3, 2);
    if (size_t(car) >= a_.car_info.size()) return;
    const MenuCar& c = a_.car_info[size_t(car)];
    centre(c.name, h0 * 3, 0, 2);
    centre(text(kAutomatic + c.transmission), h0 * -5 + 200, 0, 2);
    const int w = s.measure(a_.fonts, std::string(size_t(std::max({text(kMaxSpeed).size(), text(kGrip).size(), text(kAcceleration).size()})), 'A'), 1);
    const int x = (320 - (w + 0x40)) >> 1;
    int y = h1 * -5 + 0xd2;
    s.text(a_.fonts, x, y, text(kMaxSpeed), 1, 2);
    char kmh[32];
    std::snprintf(kmh, sizeof kmh, "%d KMH", c.max_speed);
    s.text(a_.fonts, x + w, y, kmh, 1, 2);
    y += h1;
    s.text(a_.fonts, x, y, text(kGrip), 1, 2);
    s.sprite(a_.sprites, c.grip + 4, x + w, y + 1);
    y += h1;
    s.text(a_.fonts, x, y, text(kAcceleration), 1, 2);
    s.sprite(a_.sprites, c.acceleration + 9, x + w, y + 1);
    // the arrows, lit while the camera moves that way
    if (car != 0) s.sprite(a_.sprites, (to_[0] <= from_[0] || cam_t_ <= 0.5f) ? 0 : 2, 8, 100);
    if (0xb + cars_unlocked != car) s.sprite(a_.sprites, (from_[0] <= to_[0] || cam_t_ <= 0.5f) ? 1 : 3, 0xee, 100);
}

void Menu::draw_track_select(HudSurface& s) {
    // menu_draw_track_select
    const auto text = [&](int i) { return size_t(i) < a_.texts.size() ? a_.texts[size_t(i)] : std::string(); };
    const auto centre = [&](const std::string& t, int y, int font, int m) {
        s.text(a_.fonts, (320 - s.measure(a_.fonts, t, font)) >> 1, y, t, font, m);
    };
    const int h2 = a_.fonts.fonts.size() > 2 ? a_.fonts.fonts[2].height : 8;
    centre(text(kTrackSelection), 2, 3, 2);
    if (size_t(track) < a_.track_names.size()) centre(a_.track_names[size_t(track)], h2 * -2 + 200, 2, 2);
    if (tracks_seen_ < tracks_unlocked && (frame_ & 0x3f) < 0x20) centre(text(kNewTrack), 0x20, 1, 2);
    if (track != 0) s.sprite(a_.sprites, (to_[0] <= from_[0] || cam_t_ <= 0.5f) ? 0 : 2, 8, 100);
    if (tracks_unlocked + 2 != track) s.sprite(a_.sprites, (from_[0] <= to_[0] || cam_t_ <= 0.5f) ? 1 : 3, 0xee, 100);
}

void Menu::draw_name_entry(HudSurface& s) {
    // menu_draw_name_entry
    const auto text = [&](int i) { return size_t(i) < a_.texts.size() ? a_.texts[size_t(i)] : std::string(); };
    s.text(a_.fonts, (320 - s.measure(a_.fonts, text(kEnterName), 3)) >> 1, 2, text(kEnterName), 3, 2);
    const int x = (320 - s.measure(a_.fonts, name, 2)) >> 1;
    s.text(a_.fonts, x, 0x4c, name, 2, 2);
    s.text(a_.fonts, x + name_cursor_ * 0x15, 0x58, "_", 2, 2);
}

void Menu::draw(Framebuffer& fb, Renderer& renderer) {
    cam_.center_y = 100.0f;
    renderer.begin(a_.scene, cam_, fb, 0);
    if (backdrop_) {
        // the backdrop (nearest-scaled when the framebuffer is not its size)
        const PcxImage& bg = *backdrop_;
        for (int y = 0; y < fb.height(); ++y) {
            const uint8_t* row = bg.pixels.data() + size_t(y * bg.height / fb.height()) * size_t(bg.width);
            for (int x = 0; x < fb.width(); ++x) fb.at(x, y) = row[x * bg.width / fb.width()];
        }
        fb.palette() = bg.palette();
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
    auto shown = [&](const Shown& s, const Model& m, int32_t animation) {
        if (!s.visible) return;
        Pose p;
        p.x = float(s.x);
        p.y = float(s.y);
        p.z = float(s.z);
        p.ax = float(int(s.ax * kToUnits) & 0x7ff);
        p.ay = float(int(s.ay * kToUnits) & 0x7ff);
        p.az = float(int(s.az * kToUnits) & 0x7ff);
        p.animation = animation;
        renderer.draw_model(m, p);
    };
    for (size_t i = 0; i < car_models.size(); ++i)
        if (i / 2 < a_.cars.size()) shown(car_models[i], a_.cars[i / 2], (i & 1) ? 0xe000 : 0x6000);
    for (size_t i = 0; i < map_models.size(); ++i)
        if (i % 6 < a_.maps.size()) shown(map_models[i], a_.maps[i % 6], 0x6000);
    renderer.finish();
    HudSurface s(fb);
    switch (draw_mode_) {
    case 0: draw_title_scroll(s); break;
    case 2: draw_car_select(s); break;
    case 3: draw_track_select(s); break;
    case 4: draw_name_entry(s); break;
    default: break;
    }
    for (auto& c : fb.palette()) {
        c.r = uint8_t(float(c.r) * brightness);
        c.g = uint8_t(float(c.g) * brightness);
        c.b = uint8_t(float(c.b) * brightness);
    }
}

} // namespace scr
