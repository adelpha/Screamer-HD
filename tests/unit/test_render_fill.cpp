#include "testing.h"

#include "core/gamedata.h"
#include "core/mipmaps.h"
#include "core/render3d.h"
#include "core/render3d_fill.h"
#include "core/track.h"

#include <cmath>
#include <cstring>
#include <set>

// The renderer's pixel fillers on a made-up scene (no game data): every kind of material,
// texture sizes that are and are not powers of two, coordinates far outside the texture, a
// texture at the very end of the library, depth biases, the view-distance fade. The SSE2 and
// AVX2 builds, any number of threads and both ways of clearing must give the same picture.

namespace {

uint32_t next_random(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}

template <typename T>
void put(std::vector<uint8_t>& v, size_t at, T x) {
    std::memcpy(v.data() + at, &x, sizeof(T));
}

struct Texture {
    int type, width, height;
};
// type: 0 textured, 3 texel 0 transparent, 8 flat, 12 darken, 14 translucent
const Texture kTextures[] = {
    {0, 64, 64}, {0, 48, 40}, {3, 37, 61}, {8, 16, 16}, {12, 16, 16}, {14, 48, 40}, {0, 5, 3}, {3, 64, 32}, {0, 200, 100},
};
constexpr size_t kTextureCount = sizeof kTextures / sizeof kTextures[0];

// A material library like MAPn.MLB; the last texture's pixels end with the file.
scr::MaterialLibrary make_library() {
    uint32_t seed = 12345;
    std::vector<uint8_t> mlb(kTextureCount * 16 + kTextureCount * 12, 0);
    for (size_t i = 0; i < kTextureCount; ++i) {
        const Texture& t = kTextures[i];
        const size_t desc = kTextureCount * 16 + i * 12;
        const size_t pixels = mlb.size();
        mlb.resize(pixels + size_t(t.height - 1) * 256 + size_t(t.width), 0);
        for (int y = 0; y < t.height; ++y)
            for (int x = 0; x < t.width; ++x) {
                // (blocks of a colour, some of them 0: holes in the transparent ones)
                const uint32_t r = next_random(seed);
                mlb[pixels + size_t(y) * 256 + size_t(x)] = uint8_t(((x / 3 + y / 2) % 7 == 0) ? 0 : 16 + (r % 200));
            }
        put<int32_t>(mlb, i * 16, t.type * 16);
        put<int32_t>(mlb, i * 16 + 4, int32_t(desc));
        put<int32_t>(mlb, i * 16 + 8, t.width);
        put<int32_t>(mlb, i * 16 + 12, t.height);
        put<int32_t>(mlb, desc, 1);                    // frames
        put<int32_t>(mlb, desc + 4, int32_t(40 + i));  // average colour
        put<int32_t>(mlb, desc + 8, int32_t(pixels));
    }
    auto lib = scr::MaterialLibrary::parse(std::move(mlb));
    return std::move(*lib);
}

// A fan of quads, one per material, each with its own range of texture coordinates.
scr::Model make_model(int16_t uv_lo, int16_t uv_hi, int16_t size) {
    scr::Model m;
    scr::Part part;
    part.name = "quad";
    for (size_t i = 0; i < kTextureCount; ++i) {
        const int16_t x0 = int16_t((int(i) % 3 - 1) * size), y0 = int16_t((int(i) / 3 - 1) * size);
        const int16_t z = int16_t(int(i) * 7);
        const uint16_t v0 = uint16_t(part.vertices.size());
        part.vertices.push_back({x0, y0, z});
        part.vertices.push_back({int16_t(x0 + size), int16_t(y0 + size / 5), int16_t(z - size / 2)});
        part.vertices.push_back({int16_t(x0 + size + size / 7), int16_t(y0 + size), z});
        part.vertices.push_back({int16_t(x0 - size / 9), int16_t(y0 + size - size / 6), int16_t(z + size / 3)});
        part.uvs.push_back({uv_lo, uv_lo});
        part.uvs.push_back({uv_hi, int16_t(uv_lo + 3)});
        part.uvs.push_back({int16_t(uv_hi - 5), uv_hi});
        part.uvs.push_back({int16_t(uv_lo + 11), int16_t(uv_hi + 9)});
        scr::Face f;
        f.color = uint8_t(100 + i);
        f.material = uint16_t(i * 16);
        f.count = 4;
        f.vtx = {v0, uint16_t(v0 + 1), uint16_t(v0 + 2), uint16_t(v0 + 3), 0, 0};
        // (no normal: the face is drawn from both sides)
        part.faces.push_back(f);
    }
    m.parts.push_back(std::move(part));
    scr::compute_bounds(m);
    return m;
}

scr::Track make_track() {
    scr::Track t;
    t.materials = make_library();
    t.objects.push_back(make_model(0, 255, 300));         // the texture once
    t.objects.push_back(make_model(-700, 1900, 500));     // repeated, from below zero
    t.objects.push_back(make_model(-32000, 32000, 900));  // coordinates of thousands of texels
    uint32_t seed = 777;
    for (int i = 0; i < 60; ++i) {
        scr::Placement p;
        p.object = i % 3;
        p.x = int(next_random(seed) % 5000) - 2500;
        p.y = int(next_random(seed) % 3000) - 1500;
        p.z = -400 - int(next_random(seed) % 9000);
        p.ax = int(next_random(seed) % 2048);
        p.ay = int(next_random(seed) % 2048);
        p.az = int(next_random(seed) % 2048);
        p.slot = i % 4 == 0 ? 0 : 1;  // (slot 0: road pieces, their edges sealed)
        t.placements.push_back(p);
    }
    t.shade_table.resize(64 * 256);
    for (auto& b : t.shade_table) b = uint8_t(next_random(seed));
    for (size_t i = 0; i < 256; ++i) t.darken[i] = uint8_t(255 - i);
    scr::PcxImage bck;
    bck.width = 512;
    bck.height = 256;
    bck.pixels.resize(512 * 256);
    for (auto& b : bck.pixels) b = uint8_t(next_random(seed) % 16);
    bck.vga_pal.resize(768, 0);
    t.background = std::move(bck);
    for (size_t i = 0; i < 256; ++i) t.palette[i] = {uint8_t(next_random(seed)), uint8_t(next_random(seed)), uint8_t(next_random(seed))};
    t.mips = std::make_shared<const scr::MipSet>(scr::build_mipmaps(*t.materials, t.palette));
    return t;
}

struct Options {
    scr::Renderer::FillPath path = scr::Renderer::FillPath::Auto;
    int threads = 0;
    bool clear_at_finish = true;
    bool fade = true;
    // as the renderer drew before the painted lines were looked after: every face edge of a
    // road piece sealed, texel coordinates wrapping at every polygon's edges
    bool old_look = true;
    bool mipmaps = false;
    bool wide = false;  // square pixels (Framebuffer::wide)
};

std::vector<uint8_t> draw(const scr::Track& track, int width, int height, const Options& o) {
    scr::Renderer r;
    if (!r.set_fill_path(o.path)) return {};
    r.set_threads(o.threads);
    r.set_seal_all_edges(o.old_look);
    r.set_clamp_edges(!o.old_look);
    r.set_mipmaps(o.mipmaps);
    scr::Camera cam;
    cam.z = 600;
    if (o.fade) {
        cam.far_depth = 8000.0f;
        cam.fade_depth = 5000.0f;
    }
    scr::Framebuffer fb(width, height);
    fb.wide = o.wide;
    fb.clear(7);
    r.begin(track, cam, fb, 3, o.clear_at_finish);
    r.draw_placements();
    // models of their own, as the cars are drawn: nearer and further in the depth test
    scr::Pose pose;
    pose.z = -900;
    pose.ay = 300;
    pose.depth_bias = 64.0f;
    r.draw_model(track.objects[1], pose);
    pose.x = 400;
    pose.z = -2500;
    pose.depth_bias = -4096.0f;
    pose.billboard = true;
    r.draw_model(track.objects[0], pose);
    r.finish();
    return std::vector<uint8_t>(fb.pixels(), fb.pixels() + size_t(width) * size_t(height));
}

uint64_t fnv(const std::vector<uint8_t>& v) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : v) h = (h ^ b) * 1099511628211ull;
    return h;
}

} // namespace

TEST_CASE("render fill: u and v wrapped to the texture as a plain remainder") {
    std::vector<int32_t> in;
    for (int i = -20000; i <= 20000; ++i) in.push_back(i);
    for (int i : {-2147483647 - 1, -2147483647, -65537, -65536, -32769, -32768, 32767, 32768, 65535, 65536, 2147483647}) in.push_back(i);
    scr::Renderer probe;
    const bool avx2 = probe.set_fill_path(scr::Renderer::FillPath::Avx2);
    std::vector<int32_t> a(in.size()), b(in.size());
    int wrong = 0;
    auto check = [&](int size) {
        scr::detail::wrap_coords_sse2(in.data(), a.data(), in.size(), size);
        if (avx2) scr::detail::wrap_coords_avx2(in.data(), b.data(), in.size(), size);
        for (size_t k = 0; k < in.size(); ++k) {
            int r = in[k] % size;
            if (r < 0) r += size;
            if (a[k] != r || (avx2 && b[k] != r)) ++wrong;
        }
    };
    for (int size = 1; size <= 300; ++size) check(size);
    for (int size : {511, 640, 1000, 4097, 16383, 16384, 16385, 21845, 21846, 32766, 32767}) check(size);
    CHECK(wrong == 0);
}

TEST_CASE("render fill: the same picture from every build of the filler, any threads") {
    const scr::Track track = make_track();
    const int sizes[][2] = {{320, 200}, {333, 211}, {96, 64}, {801, 500}};
    for (const auto& size : sizes) {
        const int w = size[0], h = size[1];
        Options o;
        o.path = scr::Renderer::FillPath::Sse2;
        o.threads = 1;
        const std::vector<uint8_t> ref = draw(track, w, h, o);
        REQUIRE(ref.size() == size_t(w) * size_t(h));
        // something of everything is in the picture
        CHECK(std::set<uint8_t>(ref.begin(), ref.end()).size() > 100);
        for (int threads : {2, 3, 8}) {
            o.threads = threads;
            CHECK(draw(track, w, h, o) == ref);
        }
        o.threads = 1;
        o.clear_at_finish = false;
        CHECK(draw(track, w, h, o) == ref);
        o.clear_at_finish = true;
        o.path = scr::Renderer::FillPath::Avx2;
        for (int threads : {1, 5}) {
            o.threads = threads;
            const std::vector<uint8_t> other = draw(track, w, h, o);
            if (other.empty()) break;  // a CPU without AVX2
            CHECK(other == ref);
        }
    }
    // without the fade (every pixel takes the plain way)
    Options plain;
    plain.fade = false;
    plain.path = scr::Renderer::FillPath::Sse2;
    const std::vector<uint8_t> a = draw(track, 320, 200, plain);
    plain.path = scr::Renderer::FillPath::Avx2;
    const std::vector<uint8_t> b = draw(track, 320, 200, plain);
    if (!b.empty()) CHECK(a == b);

    // today's look, without and with the smaller texture versions
    for (bool mipmaps : {false, true}) {
        Options n;
        n.old_look = false;
        n.mipmaps = mipmaps;
        n.path = scr::Renderer::FillPath::Sse2;
        n.threads = 1;
        const std::vector<uint8_t> ref = draw(track, 333, 211, n);
        CHECK(std::set<uint8_t>(ref.begin(), ref.end()).size() > 100);
        CHECK(ref != draw(track, 333, 211, Options{scr::Renderer::FillPath::Sse2, 1}));
        n.threads = 4;
        CHECK(draw(track, 333, 211, n) == ref);
        n.path = scr::Renderer::FillPath::Avx2;
        const std::vector<uint8_t> other = draw(track, 333, 211, n);
        if (!other.empty()) CHECK(other == ref);
    }

#if defined(_MSC_VER) && defined(_M_X64)
    // The pictures of the renderer before its fill loop was rewritten for speed (the same
    // scene built against that code). Only where the maths library is the one they came
    // from: the scene turns its models with sin and cos.
    Options o;
    o.path = scr::Renderer::FillPath::Sse2;
    o.threads = 2;
    CHECK(fnv(draw(track, 320, 200, o)) == 0xcf58f962385b6c11ull);
    CHECK(fnv(draw(track, 333, 211, o)) == 0x38021cc07407a6faull);
    CHECK(fnv(a) == 0xe1cf4952b8026133ull);
#endif
}

TEST_CASE("model3d: open edges and strips of a part's faces") {
    // two quads side by side (sharing an edge) and a long narrow one apart
    scr::Model m;
    scr::Part part;
    part.vertices = {{0, 0, 0}, {500, 0, 0}, {500, 0, 500}, {0, 0, 500}, {1000, 0, 0}, {1000, 0, 500},
                     {2000, 0, 0}, {2100, 0, 0}, {2100, 0, 1000}, {2000, 0, 1000}};
    scr::Face a, b, c;
    a.count = b.count = c.count = 4;
    a.vtx = {0, 1, 2, 3, 0, 0};
    b.vtx = {1, 4, 5, 2, 0, 0};
    c.vtx = {6, 7, 8, 9, 0, 0};
    part.faces = {a, b, c};
    m.parts.push_back(part);
    scr::compute_bounds(m);
    const auto& f = m.parts[0].faces;
    CHECK(f[0].open_edges == 0b1101);  // all but the edge 1-2
    CHECK(f[1].open_edges == 0b0111);  // all but the edge 2-1
    CHECK(f[2].open_edges == 0b1111);
    CHECK(!f[0].strip);
    CHECK(!f[1].strip);
    CHECK(f[2].strip);
}

TEST_CASE("track: corners of neighbouring road pieces are welded half way") {
    // two square pieces end to end, the second 20 units to the side and turned a little
    scr::Track t;
    scr::Model m;
    scr::Part part;
    part.vertices = {{0, 0, 0}, {1000, 0, 0}, {1000, 0, 1000}, {0, 0, 1000}};
    scr::Face f;
    f.count = 4;
    f.vtx = {0, 1, 2, 3, 0, 0};
    part.faces = {f};
    m.parts.push_back(part);
    scr::compute_bounds(m);
    t.objects.push_back(m);
    scr::Placement a, b;
    a.slot = b.slot = 0;
    b.x = 20;
    b.z = 1000;
    b.ay = 3;
    t.placements = {a, b};
    scr::weld_road_joints(t);
    REQUIRE(t.welds.size() == 2);
    // the far corners of the first meet the near corners of the second; the others stay
    REQUIRE(t.welds[0].size() == 2);
    REQUIRE(t.welds[1].size() == 2);
    for (const scr::VertexShift& s : t.welds[0]) {
        CHECK((s.vertex == 2 || s.vertex == 3));
        CHECK(std::fabs(s.dx - 10.0f) < 6.0f);  // half of the 20 units, give or take the turn
        CHECK(std::fabs(s.dy) < 0.01f);
    }
    for (const scr::VertexShift& s : t.welds[1]) CHECK((s.vertex == 0 || s.vertex == 1));
    // a piece far from any other keeps its corners
    scr::Track lone;
    lone.objects.push_back(m);
    lone.placements = {a};
    scr::weld_road_joints(lone);
    CHECK(lone.welds[0].empty());
}

TEST_CASE("render: the sky turns with the camera's roll") {
    // a track of nothing but its panorama: a bright band on a dark sky
    scr::Track t;
    scr::PcxImage bck;
    bck.width = 512;
    bck.height = 256;
    bck.pixels.assign(512 * 256, 1);
    for (int y = 224; y < 234; ++y)  // (panorama row 234 lies on the horizon)
        for (int x = 0; x < 512; ++x) bck.pixels[size_t(y) * 512 + size_t(x)] = 9;
    bck.vga_pal.resize(768, 0);
    t.background = bck;
    auto draw_sky = [&](float roll, bool tilt, int threads) {
        scr::Renderer r;
        r.set_threads(threads);
        r.set_sky_tilt(tilt);
        scr::Camera cam;
        cam.roll = roll;
        scr::Framebuffer fb(320, 200);
        r.begin(t, cam, fb, 0, true);
        r.finish();
        return std::vector<uint8_t>(fb.pixels(), fb.pixels() + 320 * 200);
    };
    const auto level = draw_sky(0, true, 1);
    // level: the band is a level band, the same with or without the option
    CHECK(level == draw_sky(0, false, 1));
    int rows_with_band = 0;
    for (int y = 0; y < 200; ++y) rows_with_band += level[size_t(y) * 320] == 9;
    CHECK(rows_with_band == 10);
    for (int y = 0; y < 200; ++y) CHECK(level[size_t(y) * 320] == level[size_t(y) * 320 + 319]);
    // rolled: off, still level; on, the band is higher at one side of the picture than the other
    CHECK(draw_sky(100, false, 1) == level);
    const auto rolled = draw_sky(100, true, 1);
    CHECK(rolled != level);
    auto band_row = [&](const std::vector<uint8_t>& p, int x) {
        for (int y = 0; y < 200; ++y)
            if (p[size_t(y) * 320 + size_t(x)] == 9) return y;
        return -1;
    };
    CHECK(band_row(rolled, 20) >= 0);
    CHECK(band_row(rolled, 300) >= 0);
    CHECK(std::abs(band_row(rolled, 20) - band_row(rolled, 300)) > 40);
    // the other way round for the opposite roll, and the same picture on any number of threads
    const auto other = draw_sky(2048 - 100, true, 1);
    CHECK((band_row(rolled, 20) < band_row(rolled, 300)) != (band_row(other, 20) < band_row(other, 300)));
    CHECK(draw_sky(100, true, 5) == rolled);
}

TEST_CASE("render: a wide picture is the 4:3 one with more to its sides") {
    const scr::Track track = make_track();
    Options o;
    o.old_look = false;
    o.threads = 1;
    o.wide = true;
    // square pixels: 240 lines of 320 are the original's view, 428 the same and 54 more each side
    const auto narrow = draw(track, 320, 240, o), broad = draw(track, 428, 240, o);
    REQUIRE(narrow.size() == size_t(320 * 240));
    REQUIRE(broad.size() == size_t(428 * 240));
    int differ = 0;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x) differ += narrow[size_t(y) * 320 + size_t(x)] != broad[size_t(y) * 428 + size_t(x + 54)];
    CHECK(differ < 320 * 240 / 200);  // (edges land a hair differently 54 pixels further along)
    CHECK(std::set<uint8_t>(narrow.begin(), narrow.end()).size() > 100);
    // and not the stretched picture the same size gives without it
    o.wide = false;
    CHECK(draw(track, 320, 240, o) != narrow);

    // the sky: one panorama column a pixel either way from the middle, level or rolled
    scr::Track t;
    scr::PcxImage bck;
    bck.width = 512;
    bck.height = 256;
    bck.pixels.resize(512 * 256);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 512; ++x) bck.pixels[size_t(y) * 512 + size_t(x)] = uint8_t(1 + (x / 4 + y / 4) % 250);
    bck.vga_pal.resize(768, 0);
    t.background = bck;
    for (float roll : {0.0f, 60.0f}) {
        auto sky = [&](int w) {
            scr::Renderer r;
            r.set_threads(1);
            scr::Camera cam;
            cam.roll = roll;
            scr::Framebuffer fb(w, 240);
            fb.wide = true;
            r.begin(t, cam, fb, 0, true);
            r.finish();
            return std::vector<uint8_t>(fb.pixels(), fb.pixels() + size_t(w) * 240);
        };
        const auto a = sky(320), b = sky(428);
        int off = 0;
        for (int y = 0; y < 240; ++y)
            for (int x = 0; x < 320; ++x) off += a[size_t(y) * 320 + size_t(x)] != b[size_t(y) * 428 + size_t(x + 54)];
        CHECK(off < 320 * 240 / 100);
        // (the panorama, not its fill colour, out to both edges)
        CHECK(b[100 * 428] != b[100 * 428 + 427]);
    }
}

TEST_CASE("render: the sky follows the yaw by less than a panorama column when asked to") {
    scr::Track t;
    scr::PcxImage bck;
    bck.width = 512;
    bck.height = 256;
    bck.pixels.resize(512 * 256);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 512; ++x) bck.pixels[size_t(y) * 512 + size_t(x)] = uint8_t(1 + x % 250);
    bck.vga_pal.resize(768, 0);
    t.background = bck;
    // 1280 pixels for the original's 320 columns: a panorama column is four pixels
    auto row = [&](float yaw, bool fine, float roll = 0) {
        scr::Renderer r;
        r.set_threads(1);
        scr::Camera cam;
        cam.yaw = yaw;
        cam.roll = roll;
        cam.fine_sky = fine;
        scr::Framebuffer fb(1280, 800);
        r.begin(t, cam, fb, 0, true);
        r.finish();
        return std::vector<uint8_t>(&fb.at(0, 100), &fb.at(0, 100) + 1280);
    };
    // whole columns: half a step of the yaw moves nothing, two steps move four pixels at once
    CHECK(row(100.0f, false) == row(100.4f, false));
    // exactly: half a step is a quarter of a column, one pixel
    const auto a = row(100.0f, true), b = row(100.5f, true);
    CHECK(a != b);
    int same = 0;
    for (int x = 0; x + 1 < 1280; ++x) same += a[size_t(x)] == b[size_t(x + 1)];
    CHECK(same == 1279);
    // and the same when the sky is tilted
    CHECK(row(100.0f, true, 40) != row(100.5f, true, 40));
    CHECK(row(100.0f, false, 40) == row(100.4f, false, 40));
}

TEST_CASE("render: objects the original draws early are covered by what lies behind them") {
    // one object in front of a larger one
    auto picture = [&](int draw_class, int behind_z) {
        scr::Track t;
        t.materials = make_library();
        t.objects.push_back(make_model(0, 255, 300));
        t.objects.push_back(make_model(0, 255, 900));
        t.objects[0].draw_class = draw_class;
        scr::Placement a, b;
        a.object = 0;
        a.z = -1500;
        a.slot = 1;
        b.object = 1;
        b.z = behind_z;
        b.slot = 1;
        t.placements = {a, b};
        for (size_t i = 0; i < 256; ++i) t.palette[i] = {uint8_t(i), uint8_t(i), uint8_t(i)};
        scr::Renderer r;
        r.set_threads(1);
        r.set_mipmaps(false);
        scr::Camera cam;
        cam.z = 600;
        scr::Framebuffer fb(320, 200);
        r.draw_track(t, cam, fb, 3);
        return std::vector<uint8_t>(fb.pixels(), fb.pixels() + 320 * 200);
    };
    // class 3 (cliff faces, ground sheets): as if 24000 units further away
    const auto plain = picture(-1, -4000), early = picture(3, -4000);
    CHECK(plain != early);
    // ... so the object behind shows whole, as if the first were not there
    const auto far_plain = picture(-1, -40000), far_early = picture(3, -40000);
    CHECK(far_plain == far_early);  // (more than 24000 behind: still hidden by it)
    CHECK(picture(0, -4000) == plain);
}

TEST_CASE("track: MAP2's cliff faces are in the original's early drawing class") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto path = scr::resolve_game_file(*dir, "GFX/MAP2H.DAT");
    if (!path) SKIP("MAP2H.DAT not present");
    auto t = scr::load_track(*path, "MAP2");
    REQUIRE(t.has_value());
    int cliffs = 0, roads_in_class = 0;
    for (const scr::Placement& p : t->placements) {
        const scr::Model& m = t->objects[size_t(p.object)];
        if (!m.parts.empty() && m.parts[0].name.rfind("akk", 0) == 0) {
            ++cliffs;
            CHECK(m.draw_class == 3);
        }
        if (p.slot == 0 && m.draw_class > 0) ++roads_in_class;
    }
    CHECK(cliffs > 40);
    CHECK(roads_in_class == 0);
}
