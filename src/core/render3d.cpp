#include "core/render3d.h"

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

} // namespace

void Renderer::begin(const Track& track, const Camera& cam, Framebuffer& fb, uint8_t sky) {
    track_ = &track;
    cam_ = &cam;
    fb_ = &fb;
    stats_ = {};
    deferred_.clear();
    fb.clear(sky);
    if (track.background) draw_background(*track.background, cam, fb);
    depth_.assign(size_t(fb.width()) * fb.height(), 0.0f);

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
    auto apply = [](const Mat3& r, float x, float y, float z, float& ox, float& oy, float& oz) {
        ox = r.m[0][0] * x + r.m[0][1] * y + r.m[0][2] * z;
        oy = r.m[1][0] * x + r.m[1][1] * y + r.m[1][2] * z;
        oz = r.m[2][0] * x + r.m[2][1] * y + r.m[2][2] * z;
    };
    // The game turns objects the other way round from the camera: negate the yaw (checked
    // against the original: walls facing the road, cars seen from behind in the intro).
    const Mat3 rot = rotation(pose.ax, -pose.ay, pose.az);

    const MaterialLibrary* lib = track_->materials ? &*track_->materials : nullptr;
    std::vector<ClipVert> poly;
    ++stats_.objects;
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
            poly.clear();
            float depth = 0;
            for (int i = 0; i < f.count; ++i) {
                const size_t vi = f.vtx[size_t(i)];
                if (vi >= nv) { poly.clear(); break; }
                ClipVert cv{wx_[vi], wy_[vi], wz_[vi], 0, 0};
                if (mat && env && (mat->type >> 1) == 5) {
                    cv.u = float(env_uv_[vi][0]) * float(mat->width) / 256.0f;
                    cv.v = float(env_uv_[vi][1]) * float(mat->height) / 256.0f;
                } else if (mat && vi < part.uvs.size()) {
                    cv.u = float(part.uvs[vi][0]) * float(mat->width) / 256.0f;  // (u * width) >> 5 with 3 fraction bits
                    cv.v = float(part.uvs[vi][1]) * float(mat->height) / 256.0f;
                }
                depth = std::max(depth, -cv.z);
                poly.push_back(cv);
            }
            if (poly.size() < 3) continue;
            ++stats_.faces;
            const Mode mode = material_mode(mat);
            if (mode == Mode::Translucent || mode == Mode::Darken) {
                deferred_.push_back({poly, mat, f.color, pose.animation, pose.reflection, depth});
                continue;
            }
            draw_polygon(poly, mat, f.color, pose.animation, pose.reflection);
        }
    }
}

RenderStats Renderer::finish() {
    // Translucent polygons do not write depth; draw them far to near over the opaque scene.
    std::stable_sort(deferred_.begin(), deferred_.end(),
                     [](const Deferred& a, const Deferred& b) { return a.depth > b.depth; });
    for (const Deferred& d : deferred_) draw_polygon(d.poly, d.mat, d.color, d.animation, d.reflection);
    deferred_.clear();
    return stats_;
}

void Renderer::draw_background(const PcxImage& bck, const Camera& cam, Framebuffer& fb) {
    if (bck.width <= 0 || bck.height <= 0) return;
    const float scale = float(fb.height()) / 200.0f;
    // Horizon: projection centre + focal * sin(pitch) (matched against frozen frames of the original).
    const float horizon = (cam.center_y + cam.focal * std::sin(cam.pitch * kAngle)) * scale;
    const float top = horizon - 234.0f * scale;
    const int yaw = int(std::lround(cam.yaw)) & 0x7ff;
    const int x_offset = ((0x7ff - yaw) >> 1) & 0x1ff;
    const uint8_t fill = bck.pixels[size_t(bck.height - 1) * bck.width];
    for (int y = 0; y < fb.height(); ++y) {
        const int sy = int(std::floor((float(y) - top) / scale));
        for (int x = 0; x < fb.width(); ++x) {
            if (sy < 0 || sy >= bck.height) { fb.at(x, y) = fill; continue; }
            const int sx = (x_offset + int(float(x) / scale * 320.0f / (float(fb.width()) / scale))) % bck.width;
            fb.at(x, y) = bck.pixels[size_t(sy) * bck.width + sx];
        }
    }
}

void Renderer::draw_polygon(const std::vector<ClipVert>& in, const Material* mat, uint8_t face_color, int32_t animation,
                            int32_t reflection) {
    Framebuffer& fb = *fb_;
    const MaterialLibrary* lib = track_->materials ? &*track_->materials : nullptr;
    // Clip against the near plane.
    std::vector<ClipVert> poly;
    for (size_t i = 0; i < in.size(); ++i) {
        const ClipVert& a = in[i];
        const ClipVert& b = in[(i + 1) % in.size()];
        const bool ain = -a.z >= kNear, bin = -b.z >= kNear;
        if (ain) poly.push_back(a);
        if (ain != bin) {
            const float t = (-kNear - a.z) / (b.z - a.z);
            poly.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, -kNear, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t});
        }
    }
    if (poly.size() < 3) return;

    const float scale = float(fb.height()) / 200.0f;
    const float f = cam_->focal * scale;
    const float cx = float(fb.width()) * 0.5f, cy = cam_->center_y * scale;
    struct Scr {
        float x, y, iz, uz, vz;
    };
    std::vector<Scr> s;
    s.reserve(poly.size());
    for (const auto& v : poly) {
        const float d = 128.0f - v.z;  // the original projection: 128 / (128 + depth)
        const float iz = 1.0f / d;
        s.push_back({cx - v.x * f * iz, cy - v.y * f * iz, iz, v.u * iz, v.v * iz});
    }

    Mode mode = material_mode(mat);
    const bool has_texture = lib && mat && mat->width > 0 && mat->height > 0;
    if (!has_texture && (mode == Mode::Textured || mode == Mode::Transparent)) mode = Mode::Flat;
    if (mode == Mode::Translucent && (!has_texture || track_->shade_table.size() < 64 * 256)) return;
    // --flat: average colour per material instead of texels.
    const bool use_texels = textures_ || mode == Mode::Translucent;
    const uint8_t flat = mode == Mode::Flat && mat && (mat->type >> 1) == 4 ? face_color : (mat ? mat->color : 15);
    const bool writes_depth = mode != Mode::Translucent && mode != Mode::Darken;
    // mat10_draw takes its frame from the instance's +0x3c word, the others from the
    // animation phase (cell_begin).
    const int frame = !mat ? 0
                      : (mat->type >> 1) == 5 ? int((int64_t(mat->frames) * reflection) >> 16)
                                              : material_frame(mat->frames, animation, anim_counter_);

    // Fan triangulation, edge-function rasteriser with perspective-correct interpolation.
    for (size_t t = 1; t + 1 < s.size(); ++t) {
        const Scr& a = s[0];
        const Scr& b = s[t];
        const Scr& c = s[t + 1];
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-6f) continue;
        const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(fb.width() - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(fb.height() - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
        const float inv = 1.0f / area;
        for (int y = y0; y <= y1; ++y) {
            const float py = float(y) + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                const float px = float(x) + 0.5f;
                float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv;
                float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float iz = w0 * a.iz + w1 * b.iz + w2 * c.iz;
                float& dz = depth_[size_t(y) * fb.width() + x];
                // Coplanar surfaces (road decals on the road, shadows) are resolved like the
                // original's painter's order: the later polygon wins near-ties.
                if (iz * 1.0005f < dz) continue;
                uint8_t& dst = fb.at(x, y);
                if (mode == Mode::Darken) {
                    dst = track_->darken[dst];
                    continue;
                }
                uint8_t col = flat;
                if (mode != Mode::Flat && use_texels) {
                    const float u = (w0 * a.uz + w1 * b.uz + w2 * c.uz) / iz;
                    const float v = (w0 * a.vz + w1 * b.vz + w2 * c.vz) / iz;
                    col = lib->texel(*mat, int(std::floor(u)), int(std::floor(v)), frame);
                    if (mode == Mode::Transparent && col == 0) continue;
                    if (mode == Mode::Translucent) {
                        dst = track_->shade_table[size_t(col & 63) * 256 + dst];
                        continue;
                    }
                }
                if (writes_depth) dz = iz;
                dst = col;
            }
        }
    }
}

} // namespace scr
