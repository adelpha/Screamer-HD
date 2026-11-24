#include "testing.h"

#include "core/archive.h"
#include "core/gamedata.h"
#include "core/model3d.h"
#include "core/render3d.h"
#include "core/track.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

template <typename T>
void put(std::vector<uint8_t>& v, size_t at, T x) {
    if (v.size() < at + sizeof(T)) v.resize(at + sizeof(T), 0);
    std::memcpy(v.data() + at, &x, sizeof(T));
}

// One part with a single triangle, laid out like a .3DX object.
std::vector<uint8_t> tiny_object() {
    std::vector<uint8_t> o(0x200, 0);
    put<int32_t>(o, 0x14, 1);       // parts
    put<int32_t>(o, 0x18, 0x40);    // part offset
    const size_t p = 0x40;
    put<int32_t>(o, p + 0x1c, 0x100);  // vertices
    put<int32_t>(o, p + 0x24, 0x140);  // uvs
    put<int32_t>(o, p + 0x28, 0x80);   // draw order 0 (part-relative)
    std::memcpy(o.data() + p + 0x68, "tri", 4);
    put<int16_t>(o, p + 0x80, 0x90);   // face at part + 0x90
    put<int16_t>(o, p + 0x82, -1);
    const size_t f = p + 0x90;
    put<uint16_t>(o, f + 4, 0x30);     // material 3
    put<uint16_t>(o, f + 6, 3);
    put<uint16_t>(o, f + 8, 0);
    put<uint16_t>(o, f + 10, 6);
    put<uint16_t>(o, f + 12, 12);
    put<int32_t>(o, f + 0x18, -16384);  // normal (0, -1, 0)
    const int16_t verts[] = {0, 0, 0, 100, 0, 0, 0, 0, 100, 0x7fff};
    for (size_t i = 0; i < 10; ++i) put<int16_t>(o, 0x100 + 2 * i, verts[i]);
    const int16_t uvs[] = {0, 0, 0, 255, 0, 0, 0, 255, 0};
    for (size_t i = 0; i < 9; ++i) put<int16_t>(o, 0x140 + 2 * i, uvs[i]);
    return o;
}

} // namespace

TEST_CASE("model3d: parts, vertices, uvs and faces") {
    auto o = tiny_object();
    auto m = scr::parse_model(o.data(), o.size());
    REQUIRE(m.has_value());
    REQUIRE(m->parts.size() == 1);
    const auto& part = m->parts[0];
    CHECK(part.name == "tri");
    CHECK(part.vertices.size() == 3);
    CHECK(part.vertices[1].x == 100);
    CHECK(part.uvs[1][0] == 255);
    REQUIRE(part.faces.size() == 1);
    CHECK(part.faces[0].material == 0x30);
    CHECK(part.faces[0].count == 3);
    CHECK(part.faces[0].vtx[2] == 2);
    CHECK(part.faces[0].normal[1] == -16384);
}

TEST_CASE("model3d: the traffic cone from MAP2L.DAT") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto path = scr::resolve_game_file(*dir, "GFX/MAP2L.DAT");
    REQUIRE(path.has_value());
    auto a = scr::Archive::open(*path);
    REQUIRE(a.has_value());
    auto bytes = a->read("AZCONO.3DX");
    REQUIRE(bytes.has_value());
    auto m = scr::parse_model(bytes->data(), bytes->size());
    REQUIRE(m.has_value());
    REQUIRE(m->parts.size() == 1);
    CHECK(m->parts[0].name == "azcono");
    CHECK(m->parts[0].vertices.size() == 24);
    CHECK(m->parts[0].faces.size() == 6);
}

TEST_CASE("track: MAP2 loads and renders") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto path = scr::resolve_game_file(*dir, "GFX/MAP2L.DAT");
    REQUIRE(path.has_value());
    auto t = scr::load_track(*path, "MAP2");
    REQUIRE(t.has_value());
    CHECK(t->objects.size() == 356);
    CHECK(t->placements.size() > 1000);
    REQUIRE(t->materials.has_value());
    CHECK(t->materials->materials().size() == 322);
    REQUIRE(t->background.has_value());
    CHECK(t->background->width == 512);
    CHECK(t->background->height == 256);

    // The camera of the intro race at the start gate (read from the running original).
    scr::Camera cam;
    cam.x = 48459; cam.y = 196; cam.z = 7124; cam.yaw = 472; cam.pitch = 2005;
    scr::Framebuffer fb(320, 200);
    scr::Renderer r;
    auto stats = r.draw_track(*t, cam, fb);
    CHECK(stats.objects > 20);
    CHECK(stats.faces > 500);
}

TEST_CASE("track: MAP2 high detail, track lines and ground height") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto path = scr::resolve_game_file(*dir, "GFX/MAP2H.DAT");
    REQUIRE(path.has_value());
    auto t = scr::load_track(*path, "MAP2");
    REQUIRE(t.has_value());
    CHECK(t->shade_table.size() == 64 * 256);
    CHECK(t->points.size() > 100);
    REQUIRE(t->sections.size() > 50);
    // Cross-sections: the centre lies between the two edges.
    for (const auto& s : t->sections) {
        CHECK(std::abs(2 * s.x - s.left_x - s.right_x) < 64);
        CHECK(std::abs(2 * s.z - s.left_z - s.right_z) < 64);
    }
    // The start straight of the intro race is flat at y = 0 (cars sit at y = 0 there).
    auto y = scr::ground_height(*t, 52039.0f, 6947.0f);
    REQUIRE(y.has_value());
    CHECK(std::abs(*y) < 4.0f);
    CHECK(!scr::ground_height(*t, -900000.0f, -900000.0f).has_value());
}

TEST_CASE("track: car objects, part placements and paint frames") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto path = scr::resolve_game_file(*dir, "GFX/MAP2H.DAT");
    REQUIRE(path.has_value());
    auto car = scr::load_object(*path, "AUTO00.3DX");
    REQUIRE(car.has_value());
    REQUIRE(car->parts.size() == 12);
    // Wheels sit at their part placement (226, 109, 490 for the rear left).
    bool found = false;
    for (const auto& p : car->parts)
        if (p.name == "rli") {
            found = true;
            CHECK(p.offset[0] == 226);
            CHECK(p.offset[1] == 109);
            CHECK(p.offset[2] == 490);
        }
    CHECK(found);
    // Car paint frames (8 per material) from the animation word, as in the intro demo.
    CHECK(scr::material_frame(8, 0xe000, 0) == 7);
    CHECK(scr::material_frame(8, 0x4000, 0) == 2);
    // Track placements (-1) follow the global counter; 16 frames over 0x4000.
    CHECK(scr::material_frame(16, -1, 0x4000 + 0x2000) == 8);
}
