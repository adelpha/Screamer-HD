#include "testing.h"

#include "core/gamedata.h"
#include "core/image.h"
#include "core/texture_pack.h"
#include "core/track.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <filesystem>
#include <set>

TEST_CASE("textures: replacement file names") {
    uint64_t h = 0;
    int f = 0;
    CHECK(scr::parse_texture_name("MAP2_m12_0123456789ab.png", h, f));
    CHECK(h == 0x0123456789abull);
    CHECK(f == -1);
    CHECK(scr::parse_texture_name("whatever_ABCDEF012345_f7.PNG", h, f));
    CHECK(h == 0xabcdef012345ull);
    CHECK(f == 7);
    CHECK(scr::parse_texture_name("_000000000001.png", h, f));
    CHECK(!scr::parse_texture_name("MAP2_m12_0123456789ab.bmp", h, f));
    CHECK(!scr::parse_texture_name("MAP2_m12_0123456789a.png", h, f));    // 11 digits
    CHECK(!scr::parse_texture_name("MAP2_m12_0123456789ag.png", h, f));   // not hex
    CHECK(!scr::parse_texture_name("MAP2_m120123456789ab.png", h, f));    // no underscore
    CHECK(scr::hash_name(0x0123456789abull) == "0123456789ab");
}

TEST_CASE("textures: resampling") {
    scr::RgbaImage a;
    a.width = 2;
    a.height = 1;
    a.pixels = {255, 0, 0, 255, 0, 0, 255, 0};  // red, a hole
    const scr::RgbaImage up = scr::resample(a, 4, 2);  // whole multiples: texels repeated
    CHECK(up.width == 4);
    CHECK(up.pixels[0] == 255);
    CHECK(up.pixels[4] == 255);
    CHECK(up.pixels[8 + 3] == 0);
    CHECK(up.pixels[16 + 4] == 255);  // second row
    const scr::RgbaImage down = scr::resample(up, 2, 1);  // and back by box filtering
    CHECK(down.pixels[0] == 255);
    CHECK(down.pixels[3] == 255);
    CHECK(down.pixels[7] == 0);
    const scr::RgbaImage one = scr::resample(a, 1, 1);  // the hole does not darken the colour
    CHECK(one.pixels[0] == 255);
    CHECK(one.pixels[3] == 128);
}

TEST_CASE("textures: a folder of replacements") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "scr_texture_pack_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "cars" / "AUTO00");
    const auto write = [&](const fs::path& p, int w, int h, uint8_t r) {
        scr::SurfacePtr s(SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* o = static_cast<uint8_t*>(s->pixels) + y * s->pitch + x * 4;
                o[0] = r;
                o[1] = 1;
                o[2] = 2;
                o[3] = 255;
            }
        REQUIRE(scr::save_image(s.get(), p.string()));
    };
    write(dir / "a_00000000000a.png", 8, 4, 10);
    write(dir / "cars" / "AUTO00" / "b_00000000000b_f0.png", 4, 4, 20);
    write(dir / "cars" / "AUTO00" / "b_00000000000b_f1.png", 4, 4, 21);
    write(dir / "notes.png", 4, 4, 0);  // not named as a replacement
    const scr::TexturePack p = scr::TexturePack::scan(dir.string());
    CHECK(p.size() == 2);
    CHECK(p.has(0xa));
    CHECK(!p.has(0xc));
    CHECK(p.find(0xc, 0xa) == 0xa);  // by its texels when not by its colours
    CHECK(p.find(0xb, 0xa) == 0xb);
    CHECK(p.find(0xc, 0xd) == 0);
    auto a = p.load(0xa, 3);  // one file: every frame
    REQUIRE(a.has_value());
    CHECK(a->width == 8);
    CHECK(a->height == 4);
    CHECK(a->pixels[0] == 10);
    auto b1 = p.load(0xb, 1);
    REQUIRE(b1.has_value());
    CHECK(b1->pixels[0] == 21);
    CHECK(!p.load(0xb, 2).has_value());  // no file for frame 2
    CHECK(scr::TexturePack::scan((dir / "missing").string()).empty());
    fs::remove_all(dir);
}

TEST_CASE("textures: a car's texture is the same texels on every track, in each track's colours") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto p1 = scr::resolve_game_file(*dir, "GFX/MAP1H.DAT");
    auto p2 = scr::resolve_game_file(*dir, "GFX/MAP2H.DAT");
    if (!p1 || !p2) SKIP("MAP1H.DAT / MAP2H.DAT not present");
    auto t1 = scr::load_track(*p1, "MAP1");
    auto t2 = scr::load_track(*p2, "MAP2");
    REQUIRE(t1.has_value());
    REQUIRE(t2.has_value());
    auto car1 = scr::load_object(*p1, "AUTO00.3DX");
    auto car2 = scr::load_object(*p2, "AUTO00.3DX");
    REQUIRE(car1.has_value());
    REQUIRE(car2.has_value());
    // the car's materials by their texels: the same set on both tracks
    const auto index_hashes = [](const scr::Track& t, const scr::Model& m) {
        std::set<uint64_t> s;
        for (const auto& part : m.parts)
            for (const auto& f : part.faces)
                if (const scr::Material* mat = t.materials->find(f.material); mat && scr::texture_replaceable(*mat))
                    s.insert(scr::texture_index_hash(*t.materials, *mat));
        return s;
    };
    const auto a = index_hashes(*t1, *car1), b = index_hashes(*t2, *car2);
    CHECK(a.size() > 10);
    size_t common = 0;
    for (uint64_t h : a) common += b.count(h);
    CHECK(common * 4 >= a.size() * 3);  // (AUTO00: 15 of 19; a few texels differ from track to track)
    // and every texture's two hashes tell the materials apart about as well as their offsets
    std::set<uint64_t> colours;
    size_t n = 0;
    for (const auto& m : t2->materials->materials())
        if (scr::texture_replaceable(m)) {
            ++n;
            colours.insert(scr::texture_hash(*t2->materials, m, t2->palette));
        }
    CHECK(colours.size() * 10 >= n * 8);
}
