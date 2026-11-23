#include "core/render3d.h"

#include <algorithm>
#include <cmath>

namespace scr {
namespace {

constexpr float kAngle = 6.28318530718f / 2048.0f;
constexpr float kNear = 16.0f;

struct Mat3 {
    float m[3][3];
};

// Rotation for an object placement: yaw about y, then pitch about x, then roll about z.
Mat3 object_rotation(float ax, float ay, float az) {
    const float cy = std::cos(ay * kAngle), sy = std::sin(ay * kAngle);
    const float cx = std::cos(ax * kAngle), sx = std::sin(ax * kAngle);
    const float cz = std::cos(az * kAngle), sz = std::sin(az * kAngle);
    // R = Ry * Rx * Rz
    Mat3 r{};
    r.m[0][0] = cy * cz + sy * sx * sz;  r.m[0][1] = -cy * sz + sy * sx * cz; r.m[0][2] = sy * cx;
    r.m[1][0] = cx * sz;                 r.m[1][1] = cx * cz;                 r.m[1][2] = -sx;
    r.m[2][0] = -sy * cz + cy * sx * sz; r.m[2][1] = sy * sz + cy * sx * cz;  r.m[2][2] = cy * cx;
    return r;
}

void apply(const Mat3& r, float x, float y, float z, float& ox, float& oy, float& oz) {
    ox = r.m[0][0] * x + r.m[0][1] * y + r.m[0][2] * z;
    oy = r.m[1][0] * x + r.m[1][1] * y + r.m[1][2] * z;
    oz = r.m[2][0] * x + r.m[2][1] * y + r.m[2][2] * z;
}

} // namespace

RenderStats Renderer::draw_track(const Track& track, const Camera& cam, Framebuffer& fb, uint8_t sky) {
    RenderStats stats;
    fb.clear(sky);
    if (track.background) draw_background(*track.background, cam, fb);
    depth_.assign(size_t(fb.width()) * fb.height(), 0.0f);

    // World -> camera with the rotation built by matrix_from_angles (camera_build_matrices):
    // a0 = pitch, a1 = yaw, a2 = roll. Matched against the original's screen for the same
    // camera (g_camera): camera space c = M * (v - eye) with the game's pitch negated, the
    // camera looks down -z and camera +x is screen left.
    const float s0 = std::sin(-cam.pitch * kAngle), c0 = std::cos(-cam.pitch * kAngle);
    const float s1 = std::sin(cam.yaw * kAngle), c1 = std::cos(cam.yaw * kAngle);
    const float s2 = std::sin(cam.roll * kAngle), c2 = std::cos(cam.roll * kAngle);
    const float t = s1 * s0, u = c1 * s0;
    Mat3 view{};
    view.m[0][0] = c2 * c1 - t * s2;    view.m[0][1] = s2 * c0; view.m[0][2] = u * s2 + s1 * c2;
    view.m[1][0] = -(c2 * t + c1 * s2); view.m[1][1] = c2 * c0; view.m[1][2] = u * c2 - s1 * s2;
    view.m[2][0] = -(c0 * s1);          view.m[2][1] = -s0;     view.m[2][2] = c1 * c0;

    const MaterialLibrary* lib = track.materials ? &*track.materials : nullptr;
    std::vector<float> wx, wy, wz;
    std::vector<ClipVert> poly;
    for (const Placement& p : track.placements) {
        const float dx = float(p.x) - cam.x, dz = float(p.z) - cam.z;
        if (std::fabs(dx) > cam.far_distance || std::fabs(dz) > cam.far_distance) continue;
        const Model& model = track.objects[size_t(p.object)];
        const Mat3 rot = object_rotation(float(p.ax), float(p.ay), float(p.az));
        ++stats.objects;
        for (const Part& part : model.parts) {
            const size_t nv = part.vertices.size();
            wx.resize(nv); wy.resize(nv); wz.resize(nv);
            for (size_t i = 0; i < nv; ++i) {
                float rx, ry, rz;
                apply(rot, part.vertices[i].x, part.vertices[i].y, part.vertices[i].z, rx, ry, rz);
                apply(view, rx + float(p.x) - cam.x, ry + float(p.y) - cam.y, rz + float(p.z) - cam.z, wx[i], wy[i], wz[i]);
            }
            for (const Face& f : part.faces) {
                // Back-face test like object_build_polygons: dot(vertex - eye, normal) >= 0 draws.
                float nx, ny, nz;
                apply(rot, float(f.normal[0]), float(f.normal[1]), float(f.normal[2]), nx, ny, nz);
                float cnx, cny, cnz;
                apply(view, nx, ny, nz, cnx, cny, cnz);
                const size_t v0 = f.vtx[0];
                if (wx[v0] * cnx + wy[v0] * cny + wz[v0] * cnz < 0) continue;
                const Material* mat = lib ? lib->find(f.material) : nullptr;
                poly.clear();
                for (int i = 0; i < f.count; ++i) {
                    const size_t vi = f.vtx[size_t(i)];
                    ClipVert cv{wx[vi], wy[vi], wz[vi], 0, 0};
                    if (mat) {
                        cv.u = float(part.uvs[vi][0]) * float(mat->width) / 256.0f;  // (u * width) >> 5 with 3 fraction bits
                        cv.v = float(part.uvs[vi][1]) * float(mat->height) / 256.0f;
                    }
                    poly.push_back(cv);
                }
                draw_polygon(poly, mat, lib, fb, cam);
                ++stats.faces;
            }
        }
    }
    return stats;
}

void Renderer::draw_background(const PcxImage& bck, const Camera& cam, Framebuffer& fb) {
    if (bck.width <= 0 || bck.height <= 0) return;
    const float scale = float(fb.height()) / 200.0f;
    // Horizon: centre + focal * sin(pitch) (the game's pitch sign, see draw_track).
    const float horizon = float(fb.height()) * 0.5f + cam.focal * scale * std::sin(cam.pitch * kAngle);
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

void Renderer::draw_polygon(const std::vector<ClipVert>& in, const Material* mat, const MaterialLibrary* lib,
                            Framebuffer& fb, const Camera& cam) {
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
    const float f = cam.focal * scale;
    const float cx = float(fb.width()) * 0.5f, cy = float(fb.height()) * 0.5f;
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

    const bool textured = textures_ && mat && lib && mat->width > 0 && mat->height > 0 && mat->type != 0;
    const uint8_t flat = mat ? mat->color : 15;
    // Material types 3-7 (mat3_draw) skip texels of colour 0: see-through fences, trees, signs.
    const bool transparent = textured && mat->type >= 3 && mat->type <= 7;
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
                if (iz <= dz) continue;
                uint8_t col = flat;
                if (textured) {
                    const float u = (w0 * a.uz + w1 * b.uz + w2 * c.uz) / iz;
                    const float v = (w0 * a.vz + w1 * b.vz + w2 * c.vz) / iz;
                    col = lib->texel(*mat, int(std::floor(u)), int(std::floor(v)));
                    if (transparent && col == 0) continue;
                }
                dz = iz;
                fb.at(x, y) = col;
            }
        }
    }
}

} // namespace scr
