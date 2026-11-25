#include "testing.h"

#include "core/render3d.h"
#include "core/render3d_fill.h"
#include "core/track.h"

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
    return t;
}

struct Options {
    scr::Renderer::FillPath path = scr::Renderer::FillPath::Auto;
    int threads = 0;
    bool clear_at_finish = true;
    bool fade = true;
};

std::vector<uint8_t> draw(const scr::Track& track, int width, int height, const Options& o) {
    scr::Renderer r;
    if (!r.set_fill_path(o.path)) return {};
    r.set_threads(o.threads);
    scr::Camera cam;
    cam.z = 600;
    if (o.fade) {
        cam.far_depth = 8000.0f;
        cam.fade_depth = 5000.0f;
    }
    scr::Framebuffer fb(width, height);
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
