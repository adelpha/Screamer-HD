#include "core/render3d.h"

#include "core/worker_pool.h"

#include <algorithm>
#include <emmintrin.h>
#include <cmath>

namespace scr {
namespace {

constexpr float kEdgeSlack = 0.35f;  // pixels at 320x200, scaled with the height

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
    if (skip_) return;
    track_ = &track;
    cam_ = &cam;
    fb_ = &fb;
    geo_.clear();
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
    if (skip_) return;
    // The placements in chunks on all threads, each into its own lists; the lists are then
    // joined in placement order, so the result is the same as drawing them one by one.
    const auto& all = track_->placements;
    const int chunks = std::max(1, std::min(int(all.size() / 32), pool_->threads() * 4));
    if (chunk_geo_.size() < size_t(chunks)) chunk_geo_.resize(size_t(chunks));
    pool_->run(chunks, [&](int k) {
        Geo& g = chunk_geo_[size_t(k)];
        g.clear();
        const size_t i0 = all.size() * size_t(k) / size_t(chunks), i1 = all.size() * size_t(k + 1) / size_t(chunks);
        for (size_t i = i0; i < i1; ++i) {
            const Placement& p = all[i];
            const float dx = float(p.x) - cam_->x, dz = float(p.z) - cam_->z;
            if (std::fabs(dx) > cam_->far_distance || std::fabs(dz) > cam_->far_distance) continue;
            const Model& m = track_->objects[size_t(p.object)];
            Pose pose{float(p.x), float(p.y), float(p.z), float(p.ax), float(p.ay), float(p.az), p.animation, p.reflection};
            // The original draws whole objects far to near (cell by cell) without a depth
            // buffer, and the track relies on the order: flat ground quads beside the road are
            // drawn before the road and its verges, which cover them where the quad pokes
            // through. (Only ground below the eye: flat ceilings overhead keep their depth.)
            if (p.slot != 0 && m.flat && float(p.y + m.flat_y) < cam_->y) pose.depth_bias = -kFlatGroundBias;
            pose.seal_edges = p.slot == 0;
            draw_model_into(g, m, pose, nullptr);
        }
    });
    for (int k = 0; k < chunks; ++k) geo_.append(chunk_geo_[size_t(k)]);
}

void Renderer::Geo::clear() {
    deferred.clear();
    deferred_verts.clear();
    fills.clear();
    tris.clear();
    stats = {};
    edge_slack = 0;
}

void Renderer::Geo::append(const Geo& o) {
    const uint32_t fill0 = uint32_t(fills.size()), vert0 = uint32_t(deferred_verts.size());
    fills.insert(fills.end(), o.fills.begin(), o.fills.end());
    for (Tri t : o.tris) {
        t.fill += fill0;
        tris.push_back(t);
    }
    for (Deferred d : o.deferred) {
        d.first += vert0;
        deferred.push_back(d);
    }
    deferred_verts.insert(deferred_verts.end(), o.deferred_verts.begin(), o.deferred_verts.end());
    stats.objects += o.stats.objects;
    stats.faces += o.stats.faces;
}

void Renderer::draw_model(const Model& model, const Pose& pose, const std::vector<bool>* part_mask) {
    if (skip_) return;
    draw_model_into(geo_, model, pose, part_mask);
}

void Renderer::draw_model_into(Geo& g, const Model& model, const Pose& pose, const std::vector<bool>* part_mask) {
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
    ++g.stats.objects;
    g.edge_slack = pose.seal_edges ? kEdgeSlack * float(fb_->height()) / 200.0f : 0.0f;

    // The game turns objects the other way round from the camera: negate the yaw (checked
    // against the original: walls facing the road, cars seen from behind in the intro).
    Mat3 rot = rotation(pose.ax, -pose.ay, pose.az);
    if (pose.billboard) {
        // the inverse of the view rotation: the model's x/y plane ends up across the view
        // and turned half round about its y so its front faces the camera
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) rot.m[r][c] = view_.m[c][r] * (c == 1 ? 1.0f : -1.0f);
    }
    const MaterialLibrary* lib = track_->materials ? &*track_->materials : nullptr;
    std::array<ClipVert, 6> poly;
    for (size_t pi = 0; pi < model.parts.size(); ++pi) {
        if (part_mask && (pi >= part_mask->size() || !(*part_mask)[pi])) continue;
        const Part& full = model.parts[pi];
        // Part placement inside the object, then the object's.
        const PartPose* pp = pose.parts && pi < pose.parts->size() ? &(*pose.parts)[pi] : nullptr;
        std::array<int32_t, 3> angles = full.angles;
        std::array<int32_t, 3> offset = full.offset;
        if (pp && pp->angles) angles = {pp->ax, pp->ay, pp->az};
        if (pp && pp->y) offset[1] = pp->oy;
        const bool placed = angles != std::array<int32_t, 3>{};
        // (the yaw negated as for whole objects, so a part turns the way the game turns it)
        Mat3 prot = placed ? multiply(rot, rotation(float(angles[0]), -float(angles[1]), float(angles[2]))) : rot;
        float ox, oy, oz;
        apply(rot, float(offset[0]), float(offset[1]), float(offset[2]), ox, oy, oz);
        if (!full.frames.empty()) {
            // A part of an animated object (object_load_bbox): this frame's matrix (stored by
            // columns; it also swaps y and z, which is why the game flips its back-face test
            // for these objects) and position, following the global animation counter.
            const auto& fr = full.frames[size_t((int64_t(full.frames.size()) * (anim_counter_ & 0xffff)) >> 16) % full.frames.size()];
            Mat3 pm{};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) pm.m[r][c] = float(fr[size_t(c * 3 + r)]) / 16384.0f;
            prot = multiply(rot, pm);
            apply(rot, float(fr[9]), float(fr[10]), float(fr[11]), ox, oy, oz);
        }
        ox += pose.x - cam_->x;
        oy += pose.y - cam_->y;
        oz += pose.z - cam_->z;
        if (full.radius >= 0) {
            float rx, ry, rz, cx, cy, cz;
            apply(prot, full.center[0], full.center[1], full.center[2], rx, ry, rz);
            apply(view_, rx + ox, ry + oy, rz + oz, cx, cy, cz);
            if (!sphere_visible(cx, cy, cz, full.radius)) continue;
        }
        // object_compute_depth's distant versions (--lod; the full part otherwise): from 4000
        // and 7000 units for track objects, from 12000 and 16000 for cars ('Z' parts count 8192
        // nearer)
        const Part* chosen = &full;
        if (lod_) {
            const auto& lods = model.lods;
            if (!lods[0].empty() || !lods[1].empty()) {
                float rx, ry, rz, cx, cy, cz;
                apply(prot, full.center[0], full.center[1], full.center[2], rx, ry, rz);
                apply(view_, rx + ox, ry + oy, rz + oz, cx, cy, cz);
                const float dist = std::sqrt(cx * cx + cy * cy + cz * cz);
                int level = 0;
                if (pose.dynamic) {
                    const float key = -dist * 0.5f + (!full.name.empty() && full.name[0] == 'Z' ? 8192.0f : 0.0f);
                    level = key < -7999.0f ? 2 : key < -5999.0f ? 1 : 0;
                } else {
                    level = dist >= 7000.0f ? 2 : dist >= 4000.0f ? 1 : 0;
                }
                if (level > 0 && !lods[size_t(level - 1)].empty() && pi < lods[size_t(level - 1)].size()) {
                    const LodPart& lp = lods[size_t(level - 1)][pi];
                    chosen = lp.same ? &full : lp.present ? &lp.part : nullptr;
                }
            }
        }
        if (!chosen) continue;
        const Part& part = *chosen;
        // A car's wheels: the original sorts an object's parts by distance and draws the
        // nearer wheels after the body, over the dark wheel-arch faces just outside them, and
        // the hub disc (..x, on the outside) after the tyre (..i).
        const float bias = pose.depth_bias + (part.wheel ? kWheelBias + (part.name.back() == 'x' ? 16.0f : 0.0f) : 0.0f);
        const size_t nv = part.vertices.size();
        g.wx.resize(nv); g.wy.resize(nv); g.wz.resize(nv);
        for (size_t i = 0; i < nv; ++i) {
            float rx, ry, rz;
            apply(prot, part.vertices[i].x, part.vertices[i].y, part.vertices[i].z, rx, ry, rz);
            apply(view_, rx + ox, ry + oy, rz + oz, g.wx[i], g.wy[i], g.wz[i]);
        }
        // Reflection UVs (object_transform_float): the view direction to the vertex reflected
        // off the vertex normal, u from its x and v from its z, 127 * component + 127 - scroll.
        const bool env = (part.flags & 0x2000) && part.vertex_normals.size() >= nv;
        if (env) {
            g.env_uv.resize(nv);
            for (size_t i = 0; i < nv; ++i) {
                const auto& n0 = part.vertex_normals[i];
                float rnx, rny, rnz, nx, ny, nz;
                apply(prot, n0[0], n0[1], n0[2], rnx, rny, rnz);
                apply(view_, rnx, rny, rnz, nx, ny, nz);
                const double len = std::sqrt(double(g.wx[i]) * g.wx[i] + double(g.wy[i]) * g.wy[i] + double(g.wz[i]) * g.wz[i]);
                const double vx = len > 0 ? g.wx[i] / len : 0, vy = len > 0 ? g.wy[i] / len : 0, vz = len > 0 ? g.wz[i] / len : 0;
                const double d2 = 2.0 * (vx * nx + vy * ny + vz * nz);
                const double rx = double(nx) * d2 - vx, rz = double(nz) * d2 - vz;
                g.env_uv[i] = {int(std::lrint(rx * 127.0)) + 127 - env_off_x, int(std::lrint(rz * 127.0)) + 127 - env_off_z};
            }
        }
        for (const Face& f : part.faces) {
            // Back-face test like object_build_polygons: dot(vertex - eye, normal) >= 0 draws.
            float nx, ny, nz;
            apply(prot, float(f.normal[0]), float(f.normal[1]), float(f.normal[2]), nx, ny, nz);
            float cnx, cny, cnz;
            apply(view_, nx, ny, nz, cnx, cny, cnz);
            const size_t v0 = f.vtx[0];
            // (render_patch_cull_jge: the animated objects' parts keep the other side.)
            if (v0 >= nv) continue;
            const float facing = g.wx[v0] * cnx + g.wy[v0] * cny + g.wz[v0] * cnz;
            if (part.frames.empty() ? facing < 0 : facing >= 0) continue;
            const Material* mat = lib ? lib->find(f.material) : nullptr;
            size_t n = 0;
            float depth = 0;
            for (int i = 0; i < f.count && i < 6; ++i) {
                const size_t vi = f.vtx[size_t(i)];
                if (vi >= nv) { n = 0; break; }
                ClipVert cv{g.wx[vi], g.wy[vi], g.wz[vi], 0, 0};
                if (mat && env && (mat->type >> 1) == 5) {
                    cv.u = float(g.env_uv[vi][0]) * float(mat->width) / 256.0f;
                    cv.v = float(g.env_uv[vi][1]) * float(mat->height) / 256.0f;
                } else if (mat && vi < part.uvs.size()) {
                    cv.u = float(part.uvs[vi][0]) * float(mat->width) / 256.0f;  // (u * width) >> 5 with 3 fraction bits
                    cv.v = float(part.uvs[vi][1]) * float(mat->height) / 256.0f;
                }
                depth = std::max(depth, -cv.z);
                poly[n++] = cv;
            }
            if (n < 3) continue;
            ++g.stats.faces;
            const Mode mode = material_mode(mat);
            if (mode == Mode::Translucent || mode == Mode::Darken) {
                g.deferred.push_back({uint32_t(g.deferred_verts.size()), uint32_t(n), mat, f.color, pose.animation,
                                     pose.reflection, depth, bias});
                g.deferred_verts.insert(g.deferred_verts.end(), poly.begin(), poly.begin() + ptrdiff_t(n));
                continue;
            }
            draw_polygon(g, poly.data(), n, mat, f.color, pose.animation, pose.reflection, bias);
        }
    }
}

RenderStats Renderer::finish() {
    Geo& g = geo_;
    if (skip_) return g.stats;
    g.edge_slack = 0;
    // Translucent polygons do not write depth; draw them far to near over the opaque scene.
    std::stable_sort(g.deferred.begin(), g.deferred.end(),
                     [](const Deferred& a, const Deferred& b) { return a.depth > b.depth; });
    for (const Deferred& d : g.deferred)
        draw_polygon(g, g.deferred_verts.data() + d.first, d.count, d.mat, d.color, d.animation, d.reflection, d.depth_bias);
    g.deferred.clear();
    g.deferred_verts.clear();
    // Fill the pixels: bands of rows, several per thread so that the busy parts of the
    // screen are shared out.
    if (!pool_) pool_ = std::make_unique<WorkerPool>();
    const int h = fb_->height();
    const int bands = std::max(1, std::min(h / 4, pool_->threads() * 6));
    // each band gets the list of triangles that reach it, in drawing order
    band_tris_.resize(size_t(bands));
    for (auto& l : band_tris_) l.clear();
    row_band_.resize(size_t(h));
    for (int b = 0; b < bands; ++b)
        for (int y = h * b / bands; y < h * (b + 1) / bands; ++y) row_band_[size_t(y)] = b;
    for (size_t i = 0; i < g.tris.size(); ++i) {
        const Tri& t = g.tris[i];
        const int y0 = std::clamp(t.y0, 0, h - 1), y1 = std::clamp(t.y1, 0, h - 1);
        if (t.y1 < 0 || t.y0 >= h) continue;
        for (int b = row_band_[size_t(y0)]; b <= row_band_[size_t(y1)]; ++b) band_tris_[size_t(b)].push_back(uint32_t(i));
    }
    pool_->run(bands, [&](int b) { fill_rows(h * b / bands, h * (b + 1) / bands, band_tris_[size_t(b)]); });
    g.tris.clear();
    g.fills.clear();
    return g.stats;
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

void Renderer::draw_polygon(Geo& g, const ClipVert* in, size_t count, const Material* mat, uint8_t face_color, int32_t animation,
                            int32_t reflection, float depth_bias) {
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
    fill.depth_bias = depth_bias;
    fill.edge_slack = fill.writes_depth ? g.edge_slack : 0.0f;
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
    const uint32_t fill_index = uint32_t(g.fills.size());
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
        g.tris.push_back({a, b, c, y0, y1, x0, x1, fill_index});
        used = true;
    }
    if (used) g.fills.push_back(fill);
}

// One row of a triangle: the barycentric weights and the interpolated 1/z, u/z, v/z at x0
// and their steps per pixel.
struct Renderer::Span {
    float w0, w1, w2, iz, uz, vz;
    float k0, k1, k2, diz, duz, dvz;
    float e0, e1, e2;
    uint8_t* row;
    float* drow;
    int x0, x1;
};


template <Renderer::Kind K>
void Renderer::span(Span& s, const Fill& fl, const MaterialLibrary* lib, bool bias) const {
    // Four pixels at a time (SSE2): the edge tests, the depth test, the perspective divide and
    // the texel coordinates in vector registers, then each passing pixel on its own (texel
    // fetch, transparency, write).
    // Everything the pixels need, in locals: the frame's bytes alias everything else, so
    // the compiler would otherwise reload these after every pixel written.
    const Mode mode = Mode(fl.mode);
    uint8_t* const row = s.row;
    float* const drow = s.drow;
    const uint8_t* const texels = fl.texels;
    const int wmask = fl.wmask, hmask = fl.hmask;
    const uint8_t flat = fl.flat;
    const bool writes_depth = fl.writes_depth;
    const uint8_t* const shade = track_->shade_table.data();
    const uint8_t* const darken = track_->darken.data();
    const Material* const mat = fl.mat;
    const int frame = fl.frame;
    auto pixel = [=](int x, float tz, int iu, int iv) {
        uint8_t& dst = row[x];
        if constexpr (K == Kind::Darken) {
            (void)tz; (void)iu; (void)iv; (void)mode; (void)texels; (void)wmask; (void)hmask; (void)flat;
            (void)writes_depth; (void)shade; (void)mat; (void)frame; (void)lib; (void)drow;
            dst = darken[dst];
        } else {
            (void)darken;
            uint8_t col = flat;
            if constexpr (K == Kind::Library) {
                col = lib->texel(*mat, iu, iv, frame);
            } else if constexpr (K == Kind::Texels) {
                if (wmask >= 0) iu &= wmask;
                else if ((iu %= mat->width) < 0) iu += mat->width;
                if (hmask >= 0) iv &= hmask;
                else if ((iv %= mat->height) < 0) iv += mat->height;
                col = texels[size_t(iv) * 256 + size_t(iu)];
            } else if constexpr (K != Kind::Flat) {
                col = texels[size_t(iv & hmask) * 256 + size_t(iu & wmask)];
            } else {
                (void)iu; (void)iv;
            }
            if constexpr (K == Kind::Library || K == Kind::Texels) {
                if (mode == Mode::Transparent && col == 0) return;
                if (mode == Mode::Translucent) {
                    dst = shade[size_t(col & 63) * 256 + dst];
                    return;
                }
            } else if constexpr (K == Kind::TransparentPow2) {
                if (col == 0) return;
            }
            if constexpr (K == Kind::TranslucentPow2) {
                (void)tz; (void)writes_depth; (void)drow;
                dst = shade[size_t(col & 63) * 256 + dst];
            } else {
                if (writes_depth) drow[x] = tz;
                dst = col;
            }
        }
    };
    const __m128 lane = _mm_setr_ps(0.0f, 1.0f, 2.0f, 3.0f);
    const __m128 k0 = _mm_set1_ps(s.k0), k1 = _mm_set1_ps(s.k1), k2 = _mm_set1_ps(s.k2);
    const __m128 diz = _mm_set1_ps(s.diz), duz = _mm_set1_ps(s.duz), dvz = _mm_set1_ps(s.dvz);
    const __m128 e0 = _mm_set1_ps(s.e0), e1 = _mm_set1_ps(s.e1), e2 = _mm_set1_ps(s.e2);
    const __m128 tie = _mm_set1_ps(1.0005f), one = _mm_set1_ps(1.0f), small = _mm_set1_ps(1e-3f);
    const __m128 vbias = _mm_set1_ps(fl.depth_bias);
    for (int x = s.x0; x <= s.x1; x += 4) {
        // lane j is pixel x + j: start + step * (x - x0 + j)
        const __m128 n = _mm_add_ps(_mm_set1_ps(float(x - s.x0)), lane);
        const __m128 w0 = _mm_add_ps(_mm_set1_ps(s.w0), _mm_mul_ps(k0, n));
        const __m128 w1 = _mm_add_ps(_mm_set1_ps(s.w1), _mm_mul_ps(k1, n));
        const __m128 w2 = _mm_add_ps(_mm_set1_ps(s.w2), _mm_mul_ps(k2, n));
        int in = _mm_movemask_ps(_mm_and_ps(_mm_and_ps(_mm_cmpge_ps(w0, e0), _mm_cmpge_ps(w1, e1)), _mm_cmpge_ps(w2, e2)));
        const int left = s.x1 - x + 1;
        if (left < 4) in &= (1 << left) - 1;
        if (!in) continue;
        const __m128 iz = _mm_add_ps(_mm_set1_ps(s.iz), _mm_mul_ps(diz, n));
        // Coplanar surfaces (road decals on the road, shadows) are resolved like the
        // original's painter's order: the later polygon wins near-ties.
        const __m128 tz = bias ? _mm_div_ps(iz, _mm_max_ps(small, _mm_sub_ps(one, _mm_mul_ps(vbias, iz)))) : iz;
        __m128 dz;
        if (left >= 4) {
            dz = _mm_loadu_ps(s.drow + x);
        } else {
            alignas(16) float d[4] = {0, 0, 0, 0};
            for (int j = 0; j < left; ++j) d[j] = s.drow[x + j];
            dz = _mm_load_ps(d);
        }
        const int pass = in & _mm_movemask_ps(_mm_cmpnlt_ps(_mm_mul_ps(tz, tie), dz));
        if (!pass) continue;
        alignas(16) float tzs[4];
        alignas(16) int32_t ius[4] = {0, 0, 0, 0}, ivs[4] = {0, 0, 0, 0};
        _mm_store_ps(tzs, tz);
        if constexpr (K != Kind::Flat && K != Kind::Darken) {
            const __m128 rz = _mm_div_ps(one, iz);
            const __m128 fu = _mm_mul_ps(_mm_add_ps(_mm_set1_ps(s.uz), _mm_mul_ps(duz, n)), rz);
            const __m128 fv = _mm_mul_ps(_mm_add_ps(_mm_set1_ps(s.vz), _mm_mul_ps(dvz, n)), rz);
            // floor: truncate, then one down where that went up (below zero)
            __m128i iu = _mm_cvttps_epi32(fu), iv = _mm_cvttps_epi32(fv);
            iu = _mm_add_epi32(iu, _mm_castps_si128(_mm_cmplt_ps(fu, _mm_cvtepi32_ps(iu))));
            iv = _mm_add_epi32(iv, _mm_castps_si128(_mm_cmplt_ps(fv, _mm_cvtepi32_ps(iv))));
            _mm_store_si128(reinterpret_cast<__m128i*>(ius), iu);
            _mm_store_si128(reinterpret_cast<__m128i*>(ivs), iv);
        }
        for (int j = 0; j < 4; ++j)
            if (pass & (1 << j)) pixel(x + j, tzs[j], ius[j], ivs[j]);
    }
}

void Renderer::fill_rows(int band0, int band1, const std::vector<uint32_t>& tris) {
    Framebuffer& fb = *fb_;
    const MaterialLibrary* lib = track_->materials ? &*track_->materials : nullptr;
    const size_t width = size_t(fb.width());
    for (const uint32_t ti : tris) {
        const Tri& t = geo_.tris[ti];
        if (t.y1 < band0 || t.y0 >= band1) continue;
        const Fill& fl = geo_.fills[t.fill];
        const Mode mode = Mode(fl.mode);
        const Scr& a = t.a;
        const Scr& b = t.b;
        const Scr& c = t.c;
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        const float inv = 1.0f / area;
        // Along a row w0, w1 and w2 are linear in x (w = k x + m): where all three can be
        // positive bounds the span; the exact test below still decides each pixel.
        const float k0 = (b.y - c.y) * inv, k1 = (c.y - a.y) * inv, k2 = -k0 - k1;
        // Road polygons reach a little past their edges (Pose::seal_edges).
        float e0 = 0, e1 = 0, e2 = 0;
        if (fl.edge_slack > 0) {
            const float s = -fl.edge_slack / std::fabs(area);
            e0 = s * std::hypot(c.x - b.x, c.y - b.y);
            e1 = s * std::hypot(a.x - c.x, a.y - c.y);
            e2 = s * std::hypot(b.x - a.x, b.y - a.y);
        }
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
                bound(k0, m0 - e0);
                bound(k1, m1 - e1);
                bound(k2, m2 - e2);
                if (empty) continue;
                // px = x + 0.5, with a pixel to spare either side
                if (lo > -1e29f) x0 = std::max(x0, int(std::floor(std::min(lo, 1e8f))) - 2);
                if (hi < 1e29f) x1 = std::min(x1, int(std::ceil(std::max(hi, -1e8f))) + 1);
            }
            uint8_t* row = &fb.at(0, y);
            float* drow = depth_.data() + size_t(y) * width;
            // The barycentric weights and the interpolated 1/z, u/z, v/z are linear along the
            // row: start them at x0 and step them one pixel at a time.
            const float px0 = float(x0) + 0.5f;
            Span sp;
            sp.w0 = ((b.x - px0) * (c.y - py) - (b.y - py) * (c.x - px0)) * inv;
            sp.w1 = ((c.x - px0) * (a.y - py) - (c.y - py) * (a.x - px0)) * inv;
            sp.w2 = 1.0f - sp.w0 - sp.w1;
            sp.iz = sp.w0 * a.iz + sp.w1 * b.iz + sp.w2 * c.iz;
            sp.uz = sp.w0 * a.uz + sp.w1 * b.uz + sp.w2 * c.uz;
            sp.vz = sp.w0 * a.vz + sp.w1 * b.vz + sp.w2 * c.vz;
            sp.k0 = k0; sp.k1 = k1; sp.k2 = k2;
            sp.diz = k0 * a.iz + k1 * b.iz + k2 * c.iz;
            sp.duz = k0 * a.uz + k1 * b.uz + k2 * c.uz;
            sp.dvz = k0 * a.vz + k1 * b.vz + k2 * c.vz;
            sp.e0 = e0; sp.e1 = e1; sp.e2 = e2;
            sp.row = row; sp.drow = drow;
            sp.x0 = x0; sp.x1 = x1;
            // the inner loop specialised for the fill's mode (no per-pixel decisions)
            const bool textured = mode != Mode::Flat && fl.use_texels;
            const bool bias = fl.depth_bias != 0;
            if (mode == Mode::Darken) span<Kind::Darken>(sp, fl, lib, bias);
            else if (!textured) span<Kind::Flat>(sp, fl, lib, bias);
            else if (!fl.texels) span<Kind::Library>(sp, fl, lib, bias);
            else if (fl.wmask >= 0 && fl.hmask >= 0) {
                if (mode == Mode::Translucent) span<Kind::TranslucentPow2>(sp, fl, lib, bias);
                else if (mode == Mode::Transparent) span<Kind::TransparentPow2>(sp, fl, lib, bias);
                else span<Kind::TexturedPow2>(sp, fl, lib, bias);
            } else {
                span<Kind::Texels>(sp, fl, lib, bias);
            }
        }
    }
}

} // namespace scr
