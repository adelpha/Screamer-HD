#include "core/model3d.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace scr {
namespace {

template <typename T>
bool rd(const uint8_t* d, size_t size, size_t off, T& out) {
    if (off + sizeof(T) > size) return false;
    std::memcpy(&out, d + off, sizeof(T));
    return true;
}

} // namespace

std::optional<Model> parse_model(const uint8_t* d, size_t size) {
    int32_t nparts = 0;
    if (!rd(d, size, 0x14, nparts) || nparts < 0 || nparts > 64) return std::nullopt;
    Model m;
    // +0x10: frame count of an animated object (the MAP1 helmet, the MAP2 waterwheel), else -1
    int32_t frames = -1;
    rd(d, size, 0x10, frames);
    if (frames > 0 && frames < 4096) m.frames = frames;
    for (int32_t k = 0; k < nparts; ++k) {
        int32_t po = 0;
        if (!rd(d, size, 0x18 + 4 * size_t(k), po) || po <= 0 || size_t(po) >= size) return std::nullopt;
        const size_t p = size_t(po);
        int32_t voff = 0, uvoff = 0, order0 = 0;
        if (!rd(d, size, p + 0x1c, voff) || !rd(d, size, p + 0x24, uvoff) || !rd(d, size, p + 0x28, order0)) return std::nullopt;
        Part part;
        for (size_t i = 0; i < 24 && p + 0x68 + i < size && d[p + 0x68 + i]; ++i) part.name.push_back(char(d[p + 0x68 + i]));
        rd(d, size, p + 0x18, part.flags);
        int32_t normals = 0, count = 0;
        if ((part.flags & 0x2000) && rd(d, size, p + 0x20, normals) && normals > 0 && rd(d, size, size_t(normals), count) &&
            count > 0 && count < 20000) {
            for (int32_t i = 0; i < count; ++i) {
                std::array<float, 3> n{};
                for (size_t c = 0; c < 3; ++c) {
                    const size_t o = size_t(normals) + 4 + size_t(i) * 12 + c * 4;
                    if (o + 4 <= size) std::memcpy(&n[c], d + o, 4);
                }
                part.vertex_normals.push_back(n);
            }
        }
        // Part placement (copied into the instance by engine_object_instance_init):
        // +0x0c -> { int32 ?, angle x, angle y, angle z, x, y, z }.
        int32_t xf = 0;
        if (rd(d, size, p + 0x0c, xf) && xf > 0) {
            for (size_t i = 0; i < 3; ++i) {
                rd(d, size, size_t(xf) + 4 + 4 * i, part.angles[i]);
                rd(d, size, size_t(xf) + 16 + 4 * i, part.offset[i]);
            }
            // Animated objects (object_load_bbox): from +0x1c, 0x30 bytes per frame, the part's
            // matrix (9 int16, 2.14) and position (3 int16), then their inverse.
            int32_t head = 0;
            rd(d, size, size_t(xf), head);
            const int32_t nframes = head == m.frames && size_t(xf) + 0x1c + size_t(m.frames) * 0x30 <= size ? m.frames : 0;
            for (int32_t f = 0; f < nframes; ++f) {
                std::array<int16_t, 12> fr{};
                for (size_t i = 0; i < 12; ++i) rd(d, size, size_t(xf) + 0x1c + size_t(f) * 0x30 + i * 2, fr[i]);
                part.frames.push_back(fr);
            }
        }

        if (voff <= 0) return std::nullopt;
        for (size_t v = size_t(voff);; v += 6) {
            Vec3i16 vx;
            if (!rd(d, size, v, vx.x)) return std::nullopt;
            if (uint16_t(vx.x) == 0x7fff) break;
            if (!rd(d, size, v + 2, vx.y) || !rd(d, size, v + 4, vx.z)) return std::nullopt;
            part.vertices.push_back(vx);
            std::array<int16_t, 2> uv{0, 0};
            if (uvoff > 0) {
                const size_t u = size_t(uvoff) + (v - size_t(voff));
                rd(d, size, u, uv[0]);
                rd(d, size, u + 2, uv[1]);
            }
            part.uvs.push_back(uv);
            if (part.vertices.size() > 20000) return std::nullopt;
        }

        // The 16 draw orders (one per view direction) usually all list every face, but a few
        // objects leave a face out of some of them: take the union, in list 0's order first.
        std::set<int> seen;
        std::vector<int> face_offsets;
        for (size_t list = 0; list < 16; ++list) {
            int32_t order = 0;
            if (!rd(d, size, p + 0x28 + 4 * list, order)) break;
            for (size_t q = p + size_t(order);; q += 2) {
                int16_t fo = 0;
                if (!rd(d, size, q, fo) || fo <= 0) break;
                if (seen.insert(fo).second) face_offsets.push_back(fo);
            }
        }
        for (const int fo : face_offsets) {
            const size_t f = p + size_t(fo);
            Face face;
            uint16_t n = 0;
            uint16_t color = 0;
            if (!rd(d, size, f + 4, face.material) || !rd(d, size, f + 6, n) || !rd(d, size, f + 2, color)) return std::nullopt;
            face.color = uint8_t(color);
            if (n < 3 || n > 6) continue;
            face.count = uint8_t(n);
            bool ok = true;
            for (int i = 0; i < n; ++i) {
                uint16_t vo = 0;
                if (!rd(d, size, f + 8 + 2 * size_t(i), vo) || vo % 6 || vo / 6 >= part.vertices.size()) ok = false;
                face.vtx[size_t(i)] = uint16_t(vo / 6);
            }
            for (int i = 0; i < 3; ++i) rd(d, size, f + 0x14 + 4 * size_t(i), face.normal[size_t(i)]);
            rd(d, size, f + 0x20, face.plane);
            if (ok) part.faces.push_back(face);
        }
        m.parts.push_back(std::move(part));
    }
    compute_bounds(m);
    return m;
}

CarParts find_car_parts(const Model& car) {
    CarParts out;
    for (size_t i = 0; i < car.parts.size(); ++i) {
        const std::string& n = car.parts[i].name;
        if (n == "fauto" || n == "rauto" || n == "MV_mauto" || n == "fai") out.body.push_back(int(i));
        if (car.parts[i].wheel) out.wheels.push_back(int(i));
        if (n == "flx" || n == "fli" || n == "frx" || n == "fri") out.steered.push_back(int(i));
    }
    return out;
}

void compute_bounds(Model& model) {
    model.radius = 0;
    model.flat = false;
    bool any = false, flat = true;
    int flat_y = 0;
    for (const Part& part : model.parts)
        for (const Vec3i16& v : part.vertices) {
            const int y = v.y + part.offset[1];
            if (!any) flat_y = y;
            else if (y != flat_y || part.angles != std::array<int32_t, 3>{}) flat = false;
            any = true;
        }
    model.flat = any && flat;
    model.flat_y = flat_y;
    for (Part& part : model.parts) {
        static const char* const kWheels[] = {"flx", "fli", "blx", "rli", "frx", "fri", "brx", "rri"};
        part.wheel = std::find(std::begin(kWheels), std::end(kWheels), part.name) != std::end(kWheels);
        if (part.vertices.empty()) {
            part.center = {};
            part.radius = 0;
            continue;
        }
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (const Vec3i16& v : part.vertices) {
            const float c[3] = {float(v.x), float(v.y), float(v.z)};
            for (int i = 0; i < 3; ++i) {
                lo[i] = std::min(lo[i], c[i]);
                hi[i] = std::max(hi[i], c[i]);
            }
        }
        for (int i = 0; i < 3; ++i) part.center[size_t(i)] = (lo[i] + hi[i]) * 0.5f;
        float r2 = 0;
        for (const Vec3i16& v : part.vertices) {
            const float dx = float(v.x) - part.center[0], dy = float(v.y) - part.center[1], dz = float(v.z) - part.center[2];
            r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
        }
        part.radius = std::sqrt(r2) + 1.0f;
        // However the part is turned, it stays within |offset| + |centre| + radius of the origin.
        const auto len = [](float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); };
        model.radius = std::max(model.radius, len(float(part.offset[0]), float(part.offset[1]), float(part.offset[2])) +
                                                  len(part.center[0], part.center[1], part.center[2]) + part.radius);
    }
}

} // namespace scr
