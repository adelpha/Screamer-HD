#include "core/render3d.h"

#include "core/worker_pool.h"

#include <algorithm>
#include <cmath>

namespace scr {
namespace {

constexpr float kAngle = 6.28318530718f / 2048.0f;
constexpr float kNear = 16.0f;

enum class Mode { Textured, Transparent, Flat, Darken, Translucent };

Mode material_mode(const Material* m) {
    if (!m) return Mode::Flat;
    switch (m->type >> 1) {
    case 4: return Mode::Flat;                       // 8, 9
    case 6: return Mode::Darken;                     // 12, 13
    case 7: return Mode::Translucent;                // 14, 15
    default: return m->type >= 3 && m->type <= 7 ? Mode::Transparent : Mode::Textured;
    }
}

// Rotation for an object (or part) placement: yaw about y, then pitch about x, then roll
// about z (R = Ry * Rx * Rz).
Mat3 rotation(float ax, float ay, float az) {
    const float cy = std::cos(ay * kAngle), sy = std::sin(ay * kAngle);
    const float cx = std::cos(ax * kAngle), sx = std::sin(ax * kAngle);
    const float cz = std::cos(az * kAngle), sz = std::sin(az * kAngle);
    Mat3 r{};
    r.m[0][0] = cy * cz + sy * sx * sz;  r.m[0][1] = -cy * sz + sy * sx * cz; r.m[0][2] = sy * cx;
    r.m[1][0] = cx * sz;                 r.m[1][1] = cx * cz;                 r.m[1][2] = -sx;
    r.m[2][0] = -sy * cz + cy * sx * sz; r.m[2][1] = sy * sz + cy * sx * cz;  r.m[2][2] = cy * cx;
    return r;
}

Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}

void apply(const Mat3& r, float x, float y, float z, float& ox, float& oy, float& oz) {
    ox = r.m[0][0] * x + r.m[0][1] * y + r.m[0][2] * z;
    oy = r.m[1][0] * x + r.m[1][1] * y + r.m[1][2] * z;
    oz = r.m[2][0] * x + r.m[2][1] * y + r.m[2][2] * z;
}

} // namespace

Renderer::Renderer() = default;
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

void Renderer::set_threads(int threads) {
    pool_ = std::make_unique<WorkerPool>(threads);
}

void Renderer::begin(const Track& track, const Camera& cam, Framebuffer& fb, uint8_t sky) {
    track_ = &track;
    cam_ = &cam;
    fb_ = &fb;
    stats_ = {};
    deferred_.clear();
    deferred_verts_.clear();
    fills_.clear();
    tris_.clear();
    if (!pool_) pool_ = std::make_unique<WorkerPool>();
    depth_.resize(size_t(fb.width()) * size_t(fb.height()));
    const int w = fb.width(), h = fb.height();
    const int bands = std::max(1, std::min(h, pool_->threads() * 2));
    pool_->run(bands, [&](int b) {
        const int y0 = h * b / bands, y1 = h * (b + 1) / bands;
        for (int y = y0; y < y1; ++y) std::fill(&fb.at(0, y), &fb.at(0, y) + w, sky);
        std::fill(depth_.begin() + ptrdiff_t(y0) * w, depth_.begin() + ptrdiff_t(y1) * w, 0.0f);
        if (track.background) draw_background_rows(*track.background, cam, fb, y0, y1);
    });

    // World -> camera with the rotation built by matrix_from_angles (camera_build_matrices):
    // a0 = pitch, a1 = yaw, a2 = roll. Matched against the original's screen for the same
    // camera (g_camera) in frozen frames of the original: camera space c = M * (v - eye), the
    // camera looks down -z, camera +x is screen left, and the projection centre sits at
    // (w / 2, center_y).
    const float s0 = std::sin(cam.pitch * kAngle), c0 = std::cos(cam.pitch * kAngle);
    const float s1 = std::sin(cam.yaw * kAngle), c1 = std::cos(cam.yaw * kAngle);
    const float s2 = std::sin(cam.roll * kAngle), c2 = std::cos(cam.roll * kAngle);
    const float t = s1 * s0, u = c1 * s0;
    view_.m[0][0] = c2 * c1 - t * s2;    view_.m[0][1] = s2 * c0; view_.m[0][2] = u * s2 + s1 * c2;
    view_.m[1][0] = -(c2 * t + c1 * s2); view_.m[1][1] = c2 * c0; view_.m[1][2] = u * c2 - s1 * s2;
    view_.m[2][0] = -(c0 * s1);          view_.m[2][1] = -s0;     view_.m[2][2] = c1 * c0;

    // The sides of the view. A point projects to sx = cx - x f / d, sy = cy - y f / d with
    // d = 128 - z, so it is on screen where x f <= cx d, -x f <= (w - cx) d, y f <= cy d and
    // -y f <= (h - cy) d: four planes through (0, 0, 128).
    const float scale = float(h) / 200.0f;
    const float f = cam.focal * scale;
    const float cx = float(w) * 0.5f, cy = cam.center_y * scale;
    plane_count_ = 0;
    auto plane = [&](float nx, float ny, float k) {
        // nx x + ny y - k (128 - z) <= 0 inside
        if (k <= 0) return;
        const float len = std::sqrt(nx * nx + ny * ny + k * k);
        planes_[size_t(plane_count_++)] = {nx / len, ny / len, k / len, -128.0f * k / len};
    };
    plane(f, 0, cx);
    plane(-f, 0, float(w) - cx);
    plane(0, f, cy);
    plane(0, -f, float(h) - cy);
}

bool Renderer::sphere_visible(float x, float y, float z, float r) const {
    if (z - r > -kNear) return false;  // wholly in front of the near plane
    for (int i = 0; i < plane_count_; ++i) {
        const auto& p = planes_[size_t(i)];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] > r) return false;
    }
    return true;
}

RenderStats Renderer::draw_track(const Track& track, const Camera& cam, Framebuffer& fb, uint8_t sky) {
    begin(track, cam, fb, sky);
    draw_placements();
    return finish();
}

void Renderer::draw_placements() {
    for (const Placement& p : track_->placements) {
        const float dx = float(p.x) - cam_->x, dz = float(p.z) - cam_->z;
        if (std::fabs(dx) > cam_->far_distance || std::fabs(dz) > cam_->far_distance) continue;
        draw_model(track_->objects[size_t(p.object)],
                   Pose{float(p.x), float(p.y), float(p.z), float(p.ax), float(p.ay), float(p.az), p.animation, p.reflection});
    }
}

void Renderer::draw_model(const Model& model, const Pose& pose, const std::vector<bool>* part_mask) {
    // The glass scroll: movement since the last frame, in camera space, times the rate;
    // rounded to nearest (FIST) and wrapped to the 256-texel reflection map.
    int env_off_x = 0, env_off_z = 0;
    if (pose.env) {
        EnvScroll& e = *pose.env;
        const int px = int(pose.x), py = int(pose.y), pz = int(pose.z);
        if (!e.started) {
            e.prev[0] = px; e.prev[1] = py; e.prev[2] = pz;
            e.started = true;
        }
        float cx, cy, cz;
        apply(view_, float(px - e.prev[0]), float(py - e.prev[1]), float(pz - e.prev[2]), cx, cy, cz);
        e.acc[0] = cx * pose.env_rate + e.acc[0];
        e.acc[1] = cy * pose.env_rate + e.acc[1];
        e.acc[2] = cz * pose.env_rate + e.acc[2];
        env_off_x = int(std::lrint(e.acc[0])) & 0xff;
        env_off_z = int(std::lrint(e.acc[2])) & 0xff;
        e.prev[0] = px; e.prev[1] = py; e.prev[2] = pz;
    }
    // Nothing of it on screen?
    if (model.radius >= 0) {
        float cx, cy, cz;
        apply(view_, pose.x - cam_->x, pose.y - cam_->y, pose.z - cam_->z, cx, cy, cz);
        if (!sphere_visible(cx, cy, cz, model.radius)) return;
    }
    ++stats_.objects;

    // The game turns objects the other way round from the camera: negate the yaw (checked
    // against the original: walls facing the road, cars seen from behind in the intro).
    const Mat3 rot = rotation(pose.ax, -pose.ay, pose.az);
    const MaterialLibrary* lib = track_->materials ? &*track_->materials : nullptr;
    std::array<ClipVert, 6> poly;
    for (size_t pi = 0; pi < model.parts.size(); ++pi) {
        if (part_mask && (pi >= part_mask->size() || !(*part_mask)[pi])) continue;
        const Part& part = model.parts[pi];
        // Part placement inside the object, then the object's.
        const bool placed = part.angles != std::array<int32_t, 3>{};
        const Mat3 prot = placed ? multiply(rot, rotation(float(part.angles[0]), float(part.angles[1]), float(part.angles[2]))) : rot;
        float ox, oy, oz;
        apply(rot, float(part.offset[0]), float(part.offset[1]), float(part.offset[2]), ox, oy, oz);
        ox += pose.x - cam_->x;
        oy += pose.y - cam_->y;
        oz += pose.z - cam_->z;
        if (part.radius >= 0) {
            float rx, ry, rz, cx, cy, cz;
            apply(prot, part.center[0], part.center[1], part.center[2], rx, ry, rz);
            apply(view_, rx + ox, ry + oy, rz + oz, cx, cy, cz);
            if (!sphere_visible(cx, cy, cz, part.radius)) continue;
        }
        const size_t nv = part.vertices.size();
        wx_.resize(nv); wy_.resize(nv); wz_.resize(nv);
        for (size_t i = 0; i < nv; ++i) {
            float rx, ry, rz;
            apply(prot, part.vertices[i].x, part.vertices[i].y, part.vertices[i].z, rx, ry, rz);
            apply(view_, rx + ox, ry + oy, rz + oz, wx_[i], wy_[i], wz_[i]);
        }
        // Reflection UVs (object_transform_float): the view direction to the vertex reflected
        // off the vertex normal, u from its x and v from its z, 127 * component + 127 - scroll.
        const bool env = (part.flags & 0x2000) && part.vertex_normals.size() >= nv;
        if (env) {
            env_uv_.resize(nv);
            for (size_t i = 0; i < nv; ++i) {
                const auto& n0 = part.vertex_normals[i];
                float rnx, rny, rnz, nx, ny, nz;
                apply(prot, n0[0], n0[1], n0[2], rnx, rny, rnz);
                apply(view_, rnx, rny, rnz, nx, ny, nz);
                const double len = std::sqrt(double(wx_[i]) * wx_[i] + double(wy_[i]) * wy_[i] + double(wz_[i]) * wz_[i]);
                const double vx = len > 0 ? wx_[i] / len : 0, vy = len > 0 ? wy_[i] / len : 0, vz = len > 0 ? wz_[i] / len : 0;
                const double d2 = 2.0 * (vx * nx + vy * ny + vz * nz);
                const double rx = double(nx) * d2 - vx, rz = double(nz) * d2 - vz;
                env_uv_[i] = {int(std::lrint(rx * 127.0)) + 127 - env_off_x, int(std::lrint(rz * 127.0)) + 127 - env_off_z};
            }
        }
        for (const Face& f : part.faces) {
            // Back-face test like object_build_polygons: dot(vertex - eye, normal) >= 0 draws.
            float nx, ny, nz;
            apply(prot, float(f.normal[0]), float(f.normal[1]), float(f.normal[2]), nx, ny, nz);
            float cnx, cny, cnz;
            apply(view_, nx, ny, nz, cnx, cny, cnz);
            const size_t v0 = f.vtx[0];
            if (v0 >= nv || wx_[v0] * cnx + wy_[v0] * cny + wz_[v0] * cnz < 0) continue;
            const Material* mat = lib ? lib->find(f.material) : nullptr;
            size_t n = 0;
            float depth = 0;
            for (int i = 0; i < f.count && i < 6; ++i) {
                const size_t vi = f.vtx[size_t(i)];
                if (vi >= nv) { n = 0; break; }
                ClipVert cv{wx_[vi], wy_[vi], wz_[vi], 0, 0};
                if (mat && env && (mat->type >> 1) == 5) {
                    cv.u = float(env_uv_[vi][0]) * float(mat->width) / 256.0f;
                    cv.v = float(env_uv_[vi][1]) * float(mat->height) / 256.0f;
                } else if (mat && vi < part.uvs.size()) {
                    cv.u = float(part.uvs[vi][0]) * float(mat->width) / 256.0f;  // (u * width) >> 5 with 3 fraction bits
                    cv.v = float(part.uvs[vi][1]) * float(mat->height) / 256.0f;
                }
                depth = std::max(depth, -cv.z);
                poly[n++] = cv;
            }
            if (n < 3) continue;
            ++stats_.faces;
            const Mode mode = material_mode(mat);
            if (mode == Mode::Translucent || mode == Mode::Darken) {
                deferred_.push_back({uint32_t(deferred_verts_.size()), uint32_t(n), mat, f.color, pose.animation,
                                     pose.reflection, depth});
                deferred_verts_.insert(deferred_verts_.end(), poly.begin(), poly.begin() + ptrdiff_t(n));
                continue;
            }
            draw_polygon(poly.data(), n, mat, f.color, pose.animation, pose.reflection);
        }
    }
}

RenderStats Renderer::finish() {
    // Translucent polygons do not write depth; draw them far to near over the opaque scene.
    std::stable_sort(deferred_.begin(), deferred_.end(),
                     [](const Deferred& a, const Deferred& b) { return a.depth > b.depth; });
    for (const Deferred& d : deferred_)
        draw_polygon(deferred_verts_.data() + d.first, d.count, d.mat, d.color, d.animation, d.reflection);
    deferred_.clear();
    deferred_verts_.clear();
    // Fill the pixels: bands of rows, several per thread so that the busy parts of the
    // screen are shared out.
    if (!pool_) pool_ = std::make_unique<WorkerPool>();
    const int h = fb_->height();
    const int bands = std::max(1, std::min(h / 4, pool_->threads() * 6));
    pool_->run(bands, [&](int b) { fill_rows(h * b / bands, h * (b + 1) / bands); });
    tris_.clear();
    fills_.clear();
    return stats_;
}

void Renderer::draw_background(const PcxImage& bck, const Camera& cam, Framebuffer& fb) {
    draw_background_rows(bck, cam, fb, 0, fb.height());
}

void Renderer::draw_background_rows(const PcxImage& bck, const Camera& cam, Framebuffer& fb, int y0, int y1) {
    if (bck.width <= 0 || bck.height <= 0) return;
    const float scale = float(fb.height()) / 200.0f;
    // Horizon: projection centre + focal * sin(pitch) (matched against frozen frames of the original).
    const float horizon = (cam.center_y + cam.focal * std::sin(cam.pitch * kAngle)) * scale;
    const float top = horizon - 234.0f * scale;
    const int yaw = int(std::lround(cam.yaw)) & 0x7ff;
    const int x_offset = ((0x7ff - yaw) >> 1) & 0x1ff;
    const uint8_t fill = bck.pixels[size_t(bck.height - 1) * bck.width];
    std::vector<int> column(size_t(fb.width()));
    for (int x = 0; x < fb.width(); ++x)
        column[size_t(x)] = (x_offset + int(float(x) / scale * 320.0f / (float(fb.width()) / scale))) % bck.width;
    for (int y = y0; y < y1; ++y) {
        const int sy = int(std::floor((float(y) - top) / scale));
        uint8_t* dst = &fb.at(0, y);
        if (sy < 0 || sy >= bck.height) {
            std::fill(dst, dst + fb.width(), fill);
            continue;
        }
        const uint8_t* src = bck.pixels.data() + size_t(sy) * size_t(bck.width);
        for (int x = 0; x < fb.width(); ++x) dst[x] = src[column[size_t(x)]];
    }
}

void Renderer::draw_polygon(const ClipVert* in, size_t count, const Material* mat, uint8_t face_color, int32_t animation,
                            int32_t reflection) {
    const Framebuffer& fb = *fb_;
    const MaterialLibrary* lib = track_->materials ? &*track_->materials : nullptr;
    // Clip against the near plane (a convex polygon gains at most one corner).
    std::array<ClipVert, 8> poly;
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        const ClipVert& a = in[i];
        const ClipVert& b = in[(i + 1) % count];
        const bool ain = -a.z >= kNear, bin = -b.z >= kNear;
        if (ain) poly[n++] = a;
        if (ain != bin) {
            const float t = (-kNear - a.z) / (b.z - a.z);
            poly[n++] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, -kNear, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t};
        }
    }
    if (n < 3) return;

    Mode mode = material_mode(mat);
    const bool has_texture = lib && mat && mat->width > 0 && mat->height > 0;
    if (!has_texture && (mode == Mode::Textured || mode == Mode::Transparent)) mode = Mode::Flat;
    if (mode == Mode::Translucent && (!has_texture || track_->shade_table.size() < 64 * 256)) return;

    const float scale = float(fb.height()) / 200.0f;
    const float f = cam_->focal * scale;
    const float cx = float(fb.width()) * 0.5f, cy = cam_->center_y * scale;
    std::array<Scr, 8> s;
    for (size_t i = 0; i < n; ++i) {
        const ClipVert& v = poly[i];
        const float d = 128.0f - v.z;  // the original projection: 128 / (128 + depth)
        const float iz = 1.0f / d;
        s[i] = {cx - v.x * f * iz, cy - v.y * f * iz, iz, v.u * iz, v.v * iz};
    }

    Fill fill{};
    fill.mat = mat;
    fill.mode = int(mode);
    // --flat: average colour per material instead of texels.
    fill.use_texels = textures_ || mode == Mode::Translucent;
    fill.flat = mode == Mode::Flat && mat && (mat->type >> 1) == 4 ? face_color : (mat ? mat->color : 15);
    fill.writes_depth = mode != Mode::Translucent && mode != Mode::Darken;
    // mat10_draw takes its frame from the instance's +0x3c word, the others from the
    // animation phase (cell_begin).
    fill.frame = !mat ? 0
                 : (mat->type >> 1) == 5 ? int((int64_t(mat->frames) * reflection) >> 16)
                                         : material_frame(mat->frames, animation, anim_counter_);
    fill.wmask = fill.hmask = -1;
    if (has_texture) {
        // MaterialLibrary::texel without its checks, when the whole frame lies inside the data
        const uint32_t base = mat->frame_pixels.empty() ? mat->pixels
                                                        : mat->frame_pixels[size_t(fill.frame) % mat->frame_pixels.size()];
        const size_t end = size_t(base) + size_t(mat->height - 1) * 256 + size_t(mat->width);
        if (end <= lib->data().size()) fill.texels = lib->data().data() + base;
        if ((mat->width & (mat->width - 1)) == 0) fill.wmask = mat->width - 1;
        if ((mat->height & (mat->height - 1)) == 0) fill.hmask = mat->height - 1;
    }
    const uint32_t fill_index = uint32_t(fills_.size());
    bool used = false;

    // Fan triangulation; the triangles are filled in finish().
    for (size_t t = 1; t + 1 < n; ++t) {
        const Scr& a = s[0];
        const Scr& b = s[t];
        const Scr& c = s[t + 1];
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-6f) continue;
        const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(fb.width() - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(fb.height() - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
        if (x0 > x1 || y0 > y1) continue;
        tris_.push_back({a, b, c, y0, y1, x0, x1, fill_index});
        used = true;
    }
    if (used) fills_.push_back(fill);
}

void Renderer::fill_rows(int band0, int band1) {
    Framebuffer& fb = *fb_;
    const MaterialLibrary* lib = track_->materials ? &*track_->materials : nullptr;
    const size_t width = size_t(fb.width());
    for (const Tri& t : tris_) {
        if (t.y1 < band0 || t.y0 >= band1) continue;
        const Fill& fl = fills_[t.fill];
        const Mode mode = Mode(fl.mode);
        const Scr& a = t.a;
        const Scr& b = t.b;
        const Scr& c = t.c;
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        const float inv = 1.0f / area;
        // Along a row w0, w1 and w2 are linear in x (w = k x + m): where all three can be
        // positive bounds the span; the exact test below still decides each pixel.
        const float k0 = (b.y - c.y) * inv, k1 = (c.y - a.y) * inv, k2 = -k0 - k1;
        const int ya = std::max(t.y0, band0), yb = std::min(t.y1, band1 - 1);
        for (int y = ya; y <= yb; ++y) {
            const float py = float(y) + 0.5f;
            int x0 = t.x0, x1 = t.x1;
            {
                const float m0 = (b.x * (c.y - py) - (b.y - py) * c.x) * inv;
                const float m1 = (c.x * (a.y - py) - (c.y - py) * a.x) * inv;
                const float m2 = 1.0f - m0 - m1;
                float lo = -1e30f, hi = 1e30f;
                bool empty = false;
                auto bound = [&](float k, float m) {
                    if (k > 1e-9f) lo = std::max(lo, -m / k);
                    else if (k < -1e-9f) hi = std::min(hi, -m / k);
                    else if (m < -1e-3f) empty = true;
                };
                bound(k0, m0);
                bound(k1, m1);
                bound(k2, m2);
                if (empty) continue;
                // px = x + 0.5, with a pixel to spare either side
                if (lo > -1e29f) x0 = std::max(x0, int(std::floor(std::min(lo, 1e8f))) - 2);
                if (hi < 1e29f) x1 = std::min(x1, int(std::ceil(std::max(hi, -1e8f))) + 1);
            }
            uint8_t* row = &fb.at(0, y);
            float* drow = depth_.data() + size_t(y) * width;
            for (int x = x0; x <= x1; ++x) {
                const float px = float(x) + 0.5f;
                const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv;
                const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float iz = w0 * a.iz + w1 * b.iz + w2 * c.iz;
                float& dz = drow[x];
                // Coplanar surfaces (road decals on the road, shadows) are resolved like the
                // original's painter's order: the later polygon wins near-ties.
                if (iz * 1.0005f < dz) continue;
                uint8_t& dst = row[x];
                if (mode == Mode::Darken) {
                    dst = track_->darken[dst];
                    continue;
                }
                uint8_t col = fl.flat;
                if (mode != Mode::Flat && fl.use_texels) {
                    const float u = (w0 * a.uz + w1 * b.uz + w2 * c.uz) / iz;
                    const float v = (w0 * a.vz + w1 * b.vz + w2 * c.vz) / iz;
                    int iu = int(std::floor(u)), iv = int(std::floor(v));
                    if (fl.texels) {
                        const Material& m = *fl.mat;
                        if (fl.wmask >= 0) iu &= fl.wmask;
                        else if ((iu %= m.width) < 0) iu += m.width;
                        if (fl.hmask >= 0) iv &= fl.hmask;
                        else if ((iv %= m.height) < 0) iv += m.height;
                        col = fl.texels[size_t(iv) * 256 + size_t(iu)];
                    } else {
                        col = lib->texel(*fl.mat, iu, iv, fl.frame);
                    }
                    if (mode == Mode::Transparent && col == 0) continue;
                    if (mode == Mode::Translucent) {
                        dst = track_->shade_table[size_t(col & 63) * 256 + dst];
                        continue;
                    }
                }
                if (fl.writes_depth) dz = iz;
                dst = col;
            }
        }
    }
}

} // namespace scr
