#include "core/render3d.h"

#include "core/mipmaps.h"
#include "core/worker_pool.h"

#include <algorithm>
#include <cmath>
#include <emmintrin.h>
#include <cstring>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace scr {
namespace {

constexpr float kEdgeSlack = 0.35f;  // pixels at 320x200, scaled with the height

constexpr float kAngle = 6.28318530718f / 2048.0f;
constexpr float kNear = 16.0f;

using Mode = detail::FillMode;

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

namespace {
bool cpu_has_avx2() {
#ifdef _MSC_VER
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    // OSXSAVE and AVX, and the system keeps the YMM registers
    if ((r[2] & 0x18000000) != 0x18000000 || (_xgetbv(0) & 6) != 6) return false;
    __cpuidex(r, 7, 0);
    return (r[1] & (1 << 5)) != 0;
#else
    return __builtin_cpu_supports("avx2");
#endif
}
} // namespace

bool Renderer::set_fill_path(FillPath path) {
    static const bool avx2 = cpu_has_avx2();
    if (path == FillPath::Avx2 && !avx2) return false;
    fill_rows_ = path == FillPath::Sse2 || !avx2 ? detail::fill_rows_sse2 : detail::fill_rows_avx2;
    return true;
}

const char* Renderer::fill_path_name() const {
    return fill_rows_ == detail::fill_rows_sse2 ? "sse2" : fill_rows_ == detail::fill_rows_avx2 ? "avx2" : "auto";
}

namespace detail {
uint8_t library_texel(const MaterialLibrary* lib, const Material* mat, int u, int v, int frame) {
    return lib->texel(*mat, u, v, frame);
}

void tri_edge_slack(const Tri& t, float slack, float area, float out[3]) {
    const float s = -slack / std::fabs(area);
    out[0] = t.sealed & 1 ? s * std::hypot(t.c.x - t.b.x, t.c.y - t.b.y) : 0.0f;
    out[1] = t.sealed & 2 ? s * std::hypot(t.a.x - t.c.x, t.a.y - t.c.y) : 0.0f;
    out[2] = t.sealed & 4 ? s * std::hypot(t.b.x - t.a.x, t.b.y - t.a.y) : 0.0f;
}
} // namespace detail
void Renderer::sky_rows(int y0, int y1) {
    // (after the solid polygons: the sky only where none of them is)
    Framebuffer& fb = *fb_;
    const int w = fb.width();
    if (!bg_columns_.empty()) {
        draw_background_rows(*track_->background, *cam_, fb, y0, y1, bg_columns_.data(), sky_tilt_, depth_.data());
        return;
    }
    for (int y = y0; y < y1; ++y) {
        const float* const drow = depth_.data() + size_t(y) * size_t(w);
        uint8_t* const dst = &fb.at(0, y);
        for (int x = 0; x < w; ++x)
            if (drow[x] == 0.0f) dst[x] = sky_;
    }
}

void Renderer::clear_rows(int y0, int y1) {
    Framebuffer& fb = *fb_;
    const int w = fb.width();
    std::fill(depth_.begin() + ptrdiff_t(y0) * w, depth_.begin() + ptrdiff_t(y1) * w, 0.0f);
    if (!bg_columns_.empty()) draw_background_rows(*track_->background, *cam_, fb, y0, y1, bg_columns_.data(), sky_tilt_, nullptr);
    else std::fill(&fb.at(0, y0), &fb.at(0, y0) + size_t(y1 - y0) * size_t(w), sky_);
}

void Renderer::begin(const Track& track, const Camera& cam, Framebuffer& fb, uint8_t sky, bool clear_at_finish) {
    if (skip_) return;
    track_ = &track;
    cam_ = &cam;
    fb_ = &fb;
    sky_ = sky;
    clear_at_finish_ = clear_at_finish;
    geo_.clear();
    model_jobs_.clear();
    placements_pending_ = false;
    geometry_done_ = false;
    job_count_ = 0;
    if (!pool_) pool_ = std::make_unique<WorkerPool>();
    depth_.resize(size_t(fb.width()) * size_t(fb.height()));
    const int w = fb.width(), h = fb.height();
    // (the background covers every pixel when there is one; the sky colour otherwise)
    bg_columns_.clear();
    if (track.background) background_columns(*track.background, cam, fb, bg_columns_);
    if (!clear_at_finish) {
        const int bands = std::max(1, std::min(h, pool_->threads() * 2));
        pool_->run(bands, [&](int b) { clear_rows(h * b / bands, h * (b + 1) / bands); });
    }

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
    // the view-distance fade (d = 128 - z = 128 + depth)
    fade_ = cam.far_depth < 1e29f;
    if (fade_) {
        const float start = std::min(cam.fade_depth, cam.far_depth);
        fade_iz_ = 1.0f / (128.0f + start);
        fade_start_ = start;
        fade_range_ = std::max(1.0f, cam.far_depth - start);
    }
}

bool Renderer::sphere_visible(float x, float y, float z, float r) const {
    if (z - r > -kNear) return false;  // wholly in front of the near plane
    if (-z - r > cam_->far_depth) return false;  // wholly beyond the far clip (view distance)
    for (int i = 0; i < plane_count_; ++i) {
        const auto& p = planes_[size_t(i)];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] > r) return false;
    }
    return true;
}

RenderStats Renderer::draw_track(const Track& track, const Camera& cam, Framebuffer& fb, uint8_t sky) {
    begin(track, cam, fb, sky, true);
    draw_placements();
    return finish();
}

void Renderer::draw_placements() {
    if (skip_) return;
    placements_pending_ = true;
    placements_at_ = model_jobs_.size();
}

void Renderer::draw_model(const Model& model, const Pose& pose, const std::vector<bool>* part_mask) {
    if (skip_) return;
    model_jobs_.push_back({&model, pose, part_mask});
}

void Renderer::draw_placement_range(Geo& g, size_t i0, size_t i1) {
    const auto& all = track_->placements;
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
        // (road pieces; some stretches of road are listed with the scenery)
        pose.seal_edges = p.slot == 0 || (m.has_strips && !seal_all_edges_);
        if (weld_joints_ && i < track_->welds.size() && !track_->welds[i].empty()) pose.shifts = &track_->welds[i];
        draw_model_into(g, m, pose, nullptr);
    }
}

void Renderer::flush_geometry() {
    if (skip_ || geometry_done_) return;
    geometry_done_ = true;
    // Everything drawn since begin(), on all threads: the track's placements a few at a time
    // and each model, every job into lists of its own. The lists are then used in the order
    // of the draw calls, so the result is the same as drawing one thing after another.
    constexpr size_t kPlacementsPerJob = 8;
    const size_t placements = placements_pending_ ? track_->placements.size() : 0;
    const size_t chunks = (placements + kPlacementsPerJob - 1) / kPlacementsPerJob;
    const size_t models = model_jobs_.size();
    job_count_ = chunks + models;
    if (job_geo_.size() < job_count_) job_geo_.resize(job_count_);
    // job order: the models drawn before draw_placements(), the placements, the other models
    const size_t before = placements_pending_ ? placements_at_ : models;
    pool_->run(int(job_count_), [&](int job) {
        Geo& g = job_geo_[size_t(job)];
        g.clear();
        const size_t k = size_t(job);
        if (k >= before && k < before + chunks) {
            const size_t c = k - before;
            draw_placement_range(g, c * kPlacementsPerJob, std::min(placements, (c + 1) * kPlacementsPerJob));
        } else {
            const ModelJob& m = model_jobs_[k < before ? k : k - chunks];
            draw_model_into(g, *m.model, m.pose, m.part_mask);
        }
    });
    // the translucent polygons and the counts of all jobs together
    Geo& all = geo_;
    for (size_t k = 0; k < job_count_; ++k) {
        const Geo& o = job_geo_[k];
        const uint32_t vert0 = uint32_t(all.deferred_verts.size());
        for (Deferred d : o.deferred) {
            d.first += vert0;
            all.deferred.push_back(d);
        }
        all.deferred_verts.insert(all.deferred_verts.end(), o.deferred_verts.begin(), o.deferred_verts.end());
        all.stats.objects += o.stats.objects;
        all.stats.faces += o.stats.faces;
    }
    model_jobs_.clear();
    placements_pending_ = false;
}

void Renderer::Geo::clear() {
    deferred.clear();
    deferred_verts.clear();
    fills.clear();
    tris.clear();
    tri_rows.clear();
    stats = {};
    edge_slack = 0;
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
        if (pose.shifts && chosen == &full)
            for (const VertexShift& sh : *pose.shifts) {
                if (sh.part != pi || sh.vertex >= nv) continue;
                const Vec3i16& v = part.vertices[sh.vertex];
                float rx, ry, rz;
                apply(prot, float(v.x) + sh.dx, float(v.y) + sh.dy, float(v.z) + sh.dz, rx, ry, rz);
                apply(view_, rx + ox, ry + oy, rz + oz, g.wx[sh.vertex], g.wy[sh.vertex], g.wz[sh.vertex]);
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
            draw_polygon(g, poly.data(), n, mat, f.color, pose.animation, pose.reflection, bias, seal_all_edges_ ? uint8_t(0xff) : uint8_t(f.open_edges | (f.strip ? 0x40 : 0)));
        }
    }
}

RenderStats Renderer::finish() {
    Geo& g = geo_;
    if (skip_) return g.stats;
    flush_geometry();
    g.edge_slack = 0;
    // Translucent polygons do not write depth; draw them far to near over the opaque scene.
    std::stable_sort(g.deferred.begin(), g.deferred.end(),
                     [](const Deferred& a, const Deferred& b) { return a.depth > b.depth; });
    for (const Deferred& d : g.deferred)
        draw_polygon(g, g.deferred_verts.data() + d.first, d.count, d.mat, d.color, d.animation, d.reflection, d.depth_bias, 0);
    g.deferred.clear();
    g.deferred_verts.clear();
    // Fill the pixels: bands of rows, several per thread so that the busy parts of the
    // screen are shared out.
    if (!pool_) pool_ = std::make_unique<WorkerPool>();
    const int h = fb_->height();
    const int bands = std::max(1, std::min(h / 4, pool_->threads() * 6));
    if (!fill_rows_) set_fill_path(FillPath::Auto);
    detail::FillContext ctx{};
    ctx.pixels = fb_->pixels();
    ctx.depth = depth_.data();
    ctx.width = fb_->width();
    ctx.height = h;
    ctx.lib = track_->materials ? &*track_->materials : nullptr;
    ctx.shade = track_->shade_table.data();
    ctx.darken = track_->darken.data();
    ctx.fade = fade_;
    ctx.fade_iz = fade_iz_;
    ctx.fade_start = fade_start_;
    ctx.fade_range = fade_range_;
    ctx.mip_sharpness = mip_sharpness_;
    pool_->run(bands, [&](int b) {
        const int y0 = h * b / bands, y1 = h * (b + 1) / bands;
        // (cleared here, the band's rows are still in the cache when its polygons are filled:
        // only the depths; the sky comes after the solid polygons, where none of them is)
        if (clear_at_finish_) std::fill(depth_.begin() + ptrdiff_t(y0) * fb_->width(), depth_.begin() + ptrdiff_t(y1) * fb_->width(), 0.0f);
        // the triangles that reach the band, in drawing order: every job's, then the
        // translucent polygons (which mix with what is under them, the sky too)
        static thread_local std::vector<detail::TriRef> list;
        list.clear();
        auto collect = [&](const Geo& o) {
            const Tri* const tris = o.tris.data();
            const Fill* const fills = o.fills.data();
            const std::array<int, 2>* const rows = o.tri_rows.data();
            for (size_t i = 0, n = o.tri_rows.size(); i < n; ++i)
                if (rows[i][1] >= y0 && rows[i][0] < y1) list.push_back({tris + i, fills + tris[i].fill});
        };
        for (size_t k = 0; k < job_count_; ++k) collect(job_geo_[k]);
        fill_rows_(ctx, y0, y1, list.data(), list.size());
        if (clear_at_finish_) sky_rows(y0, y1);
        list.clear();
        collect(g);
        fill_rows_(ctx, y0, y1, list.data(), list.size());
    });
    g.tris.clear();
    g.tri_rows.clear();
    g.fills.clear();
    job_count_ = 0;
    return g.stats;
}

void Renderer::draw_background(const PcxImage& bck, const Camera& cam, Framebuffer& fb) {
    draw_background_rows(bck, cam, fb, 0, fb.height());
}

void Renderer::background_columns(const PcxImage& bck, const Camera& cam, const Framebuffer& fb, std::vector<int>& column) {
    column.clear();
    if (bck.width <= 0 || bck.height <= 0) return;
    const float scale = float(fb.height()) / 200.0f;
    const int yaw = int(std::lround(cam.yaw)) & 0x7ff;
    const int x_offset = ((0x7ff - yaw) >> 1) & 0x1ff;
    column.resize(size_t(fb.width()));
    for (int x = 0; x < fb.width(); ++x)
        column[size_t(x)] = (x_offset + int(float(x) / scale * 320.0f / (float(fb.width()) / scale))) % bck.width;
}

void Renderer::draw_background_rows(const PcxImage& bck, const Camera& cam, Framebuffer& fb, int y0, int y1) {
    std::vector<int> column;
    background_columns(bck, cam, fb, column);
    if (!column.empty()) draw_background_rows(bck, cam, fb, y0, y1, column.data(), false, nullptr);
}

void Renderer::draw_background_rows(const PcxImage& bck, const Camera& cam, Framebuffer& fb, int y0, int y1, const int* column, bool tilt,
                                    const float* depth) {
    const float scale = float(fb.height()) / 200.0f;
    // Horizon: projection centre + focal * sin(pitch) (matched against frozen frames of the original).
    const float horizon = (cam.center_y + cam.focal * std::sin(cam.pitch * kAngle)) * scale;
    const float top = horizon - 234.0f * scale;
    const uint8_t fill = bck.pixels[size_t(bck.height - 1) * bck.width];
    const int w = fb.width();
    // The camera rolled (a banked car): the panorama turns about the projection centre with
    // the rest of the scene (ours). The scene's roll turns a point at (ux, uy) from the centre
    // to (c ux + s uy, -s ux + c uy); each pixel looks up the panorama where it came from.
    const float s2 = std::sin(cam.roll * kAngle), c2 = std::cos(cam.roll * kAngle);
    if (tilt && s2 != 0.0f) {
        const float cx = float(w) * 0.5f, cy = cam.center_y * scale;
        const int yaw = int(std::lround(cam.yaw)) & 0x7ff;
        const int x_offset = ((0x7ff - yaw) >> 1) & 0x1ff;
        const float across = 320.0f / (float(w) / scale) / scale;  // panorama columns per pixel
        const int bw = bck.width, bh = bck.height;
        const uint8_t* const pixels = bck.pixels.data();
        // Along a row the panorama column and row change evenly: u = u0 + du x, v = v0 + dv x.
        const float du = c2 * across, dv = s2 / scale;
        const bool pow2 = (bw & (bw - 1)) == 0;
        for (int y = y0; y < y1; ++y) {
            uint8_t* dst = &fb.at(0, y);
            const float* const drow = depth ? depth + size_t(y) * size_t(w) : nullptr;
            const float dy = float(y) - cy;
            const float u0 = float(x_offset) + (cx - c2 * cx - s2 * dy) * across;
            const float v0 = (cy - s2 * cx + c2 * dy - top) / scale;
            int x = 0;
            if (pow2) {
                // four pixels at a time; rounded down by way of a positive number (the picture
                // is some thousands of pixels across at most, a panorama column a few of them)
                const __m128 lane = _mm_setr_ps(0.0f, 1.0f, 2.0f, 3.0f);
                const __m128 vdu = _mm_set1_ps(du), vdv = _mm_set1_ps(dv);
                const __m128 ub = _mm_set1_ps(u0 + 65536.0f), vb = _mm_set1_ps(v0 + 65536.0f);
                const __m128i wrap = _mm_set1_epi32(bw - 1), up = _mm_set1_epi32(65536), rows = _mm_set1_epi32(bh);
                const int shift = int(std::lround(std::log2(double(bw))));
                for (; x + 4 <= w; x += 4) {
                    // (with a depth buffer: only the pixels nothing was drawn on)
                    const int open = drow ? _mm_movemask_ps(_mm_cmpeq_ps(_mm_loadu_ps(drow + x), _mm_setzero_ps())) : 15;
                    if (!open) continue;
                    const __m128 n = _mm_add_ps(_mm_set1_ps(float(x)), lane);
                    const __m128i iu = _mm_and_si128(_mm_cvttps_epi32(_mm_add_ps(ub, _mm_mul_ps(vdu, n))), wrap);
                    const __m128i iv = _mm_sub_epi32(_mm_cvttps_epi32(_mm_add_ps(vb, _mm_mul_ps(vdv, n))), up);
                    // inside the panorama: 0 <= row < its height
                    const __m128i inside = _mm_andnot_si128(_mm_srai_epi32(iv, 31), _mm_cmplt_epi32(iv, rows));
                    const __m128i at = _mm_and_si128(_mm_add_epi32(_mm_slli_epi32(iv, shift), iu), inside);
                    alignas(16) int32_t ats[4], in[4];
                    _mm_store_si128(reinterpret_cast<__m128i*>(ats), at);
                    _mm_store_si128(reinterpret_cast<__m128i*>(in), inside);
                    for (int j = 0; j < 4; ++j)
                        if (open & (1 << j)) dst[x + j] = in[j] ? pixels[size_t(ats[j])] : fill;
                }
            }
            for (; x < w; ++x) {
                if (drow && drow[x] != 0.0f) continue;
                const int sy = int(std::floor(v0 + dv * float(x)));
                if (sy < 0 || sy >= bh) {
                    dst[x] = fill;
                    continue;
                }
                int col = int(std::floor(u0 + du * float(x))) % bw;
                if (col < 0) col += bw;
                dst[x] = pixels[size_t(sy) * size_t(bw) + size_t(col)];
            }
        }
        return;
    }
    int drawn_sy = -1;                 // the panorama row of the last row drawn from it,
    const uint8_t* drawn = nullptr;    // and that row: the next rows are often the same
    for (int y = y0; y < y1; ++y) {
        const int sy = int(std::floor((float(y) - top) / scale));
        uint8_t* dst = &fb.at(0, y);
        if (depth) {
            // only the pixels nothing was drawn on (their depth is still nought)
            const float* const drow = depth + size_t(y) * size_t(w);
            const uint8_t* const src = sy < 0 || sy >= bck.height ? nullptr : bck.pixels.data() + size_t(sy) * size_t(bck.width);
            int x = 0;
            for (; x + 8 <= w; x += 8) {
                const int open = _mm_movemask_ps(_mm_cmpeq_ps(_mm_loadu_ps(drow + x), _mm_setzero_ps())) |
                                 _mm_movemask_ps(_mm_cmpeq_ps(_mm_loadu_ps(drow + x + 4), _mm_setzero_ps())) << 4;
                if (!open) continue;
                for (int j = 0; j < 8; ++j)
                    if (open & (1 << j)) dst[x + j] = src ? src[column[x + j]] : fill;
            }
            for (; x < w; ++x)
                if (drow[x] == 0.0f) dst[x] = src ? src[column[x]] : fill;
            continue;
        }
        if (sy < 0 || sy >= bck.height) {
            std::fill(dst, dst + w, fill);
            continue;
        }
        if (sy == drawn_sy) {
            std::memcpy(dst, drawn, size_t(w));
            continue;
        }
        const uint8_t* src = bck.pixels.data() + size_t(sy) * size_t(bck.width);
        for (int x = 0; x < w; ++x) dst[x] = src[column[x]];
        drawn_sy = sy;
        drawn = dst;
    }
}

void Renderer::draw_polygon(Geo& g, const ClipVert* in, size_t count, const Material* mat, uint8_t face_color, int32_t animation,
                            int32_t reflection, float depth_bias, uint8_t open_edges) {
    const Framebuffer& fb = *fb_;
    const MaterialLibrary* lib = track_->materials ? &*track_->materials : nullptr;
    // Clip against the near plane (a convex polygon gains at most one corner).
    std::array<ClipVert, 8> poly;
    std::array<bool, 8> sealed{};
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        const ClipVert& a = in[i];
        const ClipVert& b = in[(i + 1) % count];
        const bool ain = -a.z >= kNear, bin = -b.z >= kNear;
        // (the edge that leaves each corner keeps its seal; the edge along the near plane has none)
        const bool open = (open_edges >> i) & 1;
        if (ain) {
            sealed[n] = open;
            poly[n++] = a;
        }
        if (ain != bin) {
            const float t = (-kNear - a.z) / (b.z - a.z);
            sealed[n] = !ain && open;
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
    fill.seal_behind = open_edges != 0xff;
    // A painted line of the road, far enough to be thinner than a pixel, would be drawn in
    // dots (a pixel is filled when its centre is inside): it reaches past all its edges by what
    // it lacks to a pixel's width, over the road faces beside it. Only to three times its
    // real width, though: further still the lines thin out again, or a road seen from far
    // away would be more line than road.
    if (fill.edge_slack > 0 && open_edges != 0xff && (open_edges & 0x40)) {
        float area2 = 0, longest = 0;
        for (size_t i = 0; i < n; ++i) {
            const Scr& p = s[i];
            const Scr& q = s[(i + 1) % n];
            area2 += p.x * q.y - q.x * p.y;
            longest = std::max(longest, std::hypot(q.x - p.x, q.y - p.y));
        }
        const float width = longest > 0 ? std::fabs(area2) * 0.5f / longest : 0.0f;
        if (width < 1.0f) {
            fill.edge_slack = (std::min(1.0f, width * kStripWiden) - width) * 0.5f;
            fill.depth_bias += kStripBias;
            fill.seal_behind = false;
            sealed.fill(true);
        }
    }    // mat10_draw takes its frame from the instance's +0x3c word, the others from the
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
        // (the fillers wrap u and v with 16-bit arithmetic; anything larger goes texel by texel)
        if (end <= lib->data().size() && mat->width <= 32767 && mat->height <= 32767) fill.texels = lib->data().data() + base;
        fill.wide_reads = end + 3 <= lib->data().size();
        fill.width = mat->width;
        fill.height = mat->height;
        fill.ubias = (16384 + mat->width - 1) / mat->width * mat->width;
        fill.vbias = (16384 + mat->height - 1) / mat->height * mat->height;
        if (clamp_edges_) {
            fill.clamp_uv = true;
            for (size_t i = 0; i < count; ++i)
                if (!(in[i].u >= 0 && in[i].u <= float(mat->width) && in[i].v >= 0 && in[i].v <= float(mat->height))) fill.clamp_uv = false;
        }
        fill.umagic = 65536 / mat->width;
        fill.vmagic = 65536 / mat->height;
        if (mipmaps_ && track_->mips && fill.texels && fill.use_texels && mode != Mode::Translucent)
            fill.mip = track_->mips->find(size_t(mat - lib->materials().data()), fill.frame);
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
        // (edge b-c is the polygon's edge t; c-a and a-b are its last and first edge in the
        // last and first triangle of the fan, and cuts through the polygon otherwise)
        const uint8_t seal = open_edges == 0xff ? uint8_t(7) : uint8_t((sealed[t] ? 1 : 0) | (t + 2 == n && sealed[n - 1] ? 2 : 0) | (t == 1 && sealed[0] ? 4 : 0));
        g.tris.push_back({a, b, c, y0, y1, x0, x1, fill_index, seal});
        g.tri_rows.push_back({y0, y1});
        used = true;
    }
    if (used) g.fills.push_back(fill);
}

} // namespace scr
