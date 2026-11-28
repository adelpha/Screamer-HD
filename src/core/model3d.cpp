#include "core/model3d.h"

#include <algorithm>
#include <map>
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
    auto parse_part = [&](int32_t po) -> std::optional<Part> {
        if (po <= 0 || size_t(po) >= size) return std::nullopt;
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
        int32_t list0_count = 0;
        rd(d, size, p + 0x14, list0_count);
        rd(d, size, p, part.bound_r);
        rd(d, size, p + 4, part.bound_x);
        rd(d, size, p + 6, part.bound_y);
        rd(d, size, p + 8, part.bound_z);
        std::vector<int> list0;
        if (list0_count > 0 && list0_count < 20000)
            for (int32_t k = 0; k < list0_count; ++k) {
                uint16_t fo = 0;
                if (!rd(d, size, p + size_t(order0) + 2 * size_t(k), fo)) break;
                list0.push_back(fo);
            }
        std::vector<std::pair<int, int>> offset_to_face;
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
            if (ok) {
                offset_to_face.push_back({fo, int(part.faces.size())});
                part.faces.push_back(face);
            }
        }
        for (size_t list = 0; list < 16; ++list) {
            int32_t order = 0;
            if (!rd(d, size, p + 0x28 + 4 * list, order)) break;
            for (size_t q = p + size_t(order);; q += 2) {
                int16_t fo = 0;
                if (!rd(d, size, q, fo) || fo <= 0) break;
                for (const auto& [o, i] : offset_to_face)
                    if (o == fo) {
                        part.draw_order[list].push_back(uint16_t(i));
                        break;
                    }
            }
        }
        for (Face& f : part.faces) f.lists = 0;
        for (size_t list = 0; list < 16; ++list)
            for (const uint16_t i : part.draw_order[list]) part.faces[i].lists |= uint16_t(1u << list);
        for (const int fo : list0)
            for (const auto& [o, i] : offset_to_face)
                if (o == fo) {
                    part.collision_faces.push_back(i);
                    break;
                }
        return part;
    };
    for (int32_t k = 0; k < nparts; ++k) {
        int32_t po = 0;
        if (!rd(d, size, 0x18 + 4 * size_t(k), po)) return std::nullopt;
        auto part = parse_part(po);
        if (!part) return std::nullopt;
        m.parts.push_back(std::move(*part));
    }
    rd(d, size, 0x32c, m.draw_class);
    // object_compute_depth: two simpler versions of each part for distant objects, the part
    // offset tables at +0x11c and +0x220 (65 entries further each); 0 = not drawn there.
    for (int lod = 1; lod <= 2; ++lod) {
        std::vector<LodPart> parts;
        bool any = false;
        for (int32_t k = 0; k < nparts; ++k) {
            int32_t po = 0, po0 = 0;
            rd(d, size, 0x18 + 0x104 * size_t(lod) + 4 * size_t(k), po);
            rd(d, size, 0x18 + 4 * size_t(k), po0);
            LodPart lp;
            if (po == po0) {
                lp.same = true;
            } else if (po > 0) {
                if (auto part = parse_part(po)) {
                    lp.part = std::move(*part);
                    lp.present = true;
                    any = true;
                }
            } else {
                any = true;  // left out at this distance
            }
            parts.push_back(std::move(lp));
        }
        if (any) m.lods[size_t(lod - 1)] = std::move(parts);
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

// Faces narrower than this (model units) and at least half as long again are strips (the
// road's painted lines are about 127 wide and up to 1100 long, shorter in bends; its lanes
// are 665 and more wide).
constexpr double kStripWidth = 200.0;

void compute_bounds(Model& model) {
    model.radius = 0;
    model.has_strips = false;
    model.flat = false;
    bool any = false, flat = true;
    int flat_y = 0;
    for (const Part& part : model.parts)
        for (const Vec3i16& v : part.vertices) {
            const int y = v.y + part.offset[1];
            model.min_y = any ? std::min(model.min_y, y) : y;
            model.max_y = any ? std::max(model.max_y, y) : y;
            if (!any) flat_y = y;
            else if (y != flat_y || part.angles != std::array<int32_t, 3>{}) flat = false;
            any = true;
        }
    model.flat = any && flat;
    model.flat_y = flat_y;
    std::vector<Part*> all;
    for (Part& part : model.parts) all.push_back(&part);
    for (auto& lod : model.lods)
        for (LodPart& lp : lod)
            if (lp.present) all.push_back(&lp.part);
    for (Part* pp : all) {
        Part& part = *pp;
        // the edges that only one face has (by where their ends are: vertices can be doubled)
        {
            auto key = [&](uint16_t a, uint16_t b) {
                auto at = [&](uint16_t i) {
                    const Vec3i16 v = i < part.vertices.size() ? part.vertices[i] : Vec3i16{};
                    return uint64_t(uint16_t(v.x)) | uint64_t(uint16_t(v.y)) << 16 | uint64_t(uint16_t(v.z)) << 32;
                };
                const uint64_t p = at(a), q = at(b);
                return std::pair<uint64_t, uint64_t>(std::min(p, q), std::max(p, q));
            };
            std::map<std::pair<uint64_t, uint64_t>, int> uses;
            for (const Face& f : part.faces)
                for (int i = 0; i < f.count && i < 6; ++i) ++uses[key(f.vtx[size_t(i)], f.vtx[size_t((i + 1) % f.count)])];
            for (Face& f : part.faces) {
                // narrow: its area over its longest edge
                {
                    double ax = 0, ay = 0, az = 0, longest = 0;
                    const int n = std::min<int>(f.count, 6);
                    auto vert = [&](int i) { const uint16_t k = f.vtx[size_t(i % n)]; return k < part.vertices.size() ? part.vertices[k] : Vec3i16{}; };
                    for (int i = 0; i < n; ++i) {
                        const Vec3i16 p = vert(i), q = vert(i + 1), o = vert(0);
                        const double px = p.x - o.x, py = p.y - o.y, pz = p.z - o.z, qx = q.x - o.x, qy = q.y - o.y, qz = q.z - o.z;
                        ax += py * qz - pz * qy;
                        ay += pz * qx - px * qz;
                        az += px * qy - py * qx;
                        longest = std::max(longest, std::sqrt(double(q.x - p.x) * (q.x - p.x) + double(q.y - p.y) * (q.y - p.y) + double(q.z - p.z) * (q.z - p.z)));
                    }
                    const double width = longest > 0 ? 0.5 * std::sqrt(ax * ax + ay * ay + az * az) / longest : 0;
                    f.strip = n >= 3 && width < kStripWidth && longest > 1.5 * width;
                    if (f.strip && pp < model.parts.data() + model.parts.size() && pp >= model.parts.data()) model.has_strips = true;
                }
                f.open_edges = 0;
                for (int i = 0; i < f.count && i < 6; ++i)
                    if (uses[key(f.vtx[size_t(i)], f.vtx[size_t((i + 1) % f.count)])] < 2) f.open_edges |= uint8_t(1 << i);
            }
        }        static const char* const kWheels[] = {"flx", "fli", "blx", "rli", "frx", "fri", "brx", "rri"};
        for (Face& f : part.faces) {
            if (f.count < 4 || f.vtx[0] >= part.vertices.size() || f.vtx[1] >= part.vertices.size() || f.vtx[2] >= part.vertices.size()) continue;
            const Vec3i16& a = part.vertices[f.vtx[0]];
            const Vec3i16& b = part.vertices[f.vtx[1]];
            const Vec3i16& c = part.vertices[f.vtx[2]];
            const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
            const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len <= 0) continue;
            for (int i = 3; i < f.count; ++i) {
                if (f.vtx[size_t(i)] >= part.vertices.size()) continue;
                const Vec3i16& d = part.vertices[f.vtx[size_t(i)]];
                if (std::fabs(((d.x - a.x) * nx + (d.y - a.y) * ny + (d.z - a.z) * nz) / len) > 32.0) f.folded = true;
            }
        }
        part.wheel = std::find(std::begin(kWheels), std::end(kWheels), part.name) != std::end(kWheels);
        // cards: two faces with the same corners (by position), normals opposite
        {
            std::map<std::vector<std::array<int, 3>>, std::vector<size_t>> by_corners;
            for (size_t fi = 0; fi < part.faces.size(); ++fi) {
                const Face& f = part.faces[fi];
                std::vector<std::array<int, 3>> c;
                for (int i = 0; i < f.count && i < 6; ++i) {
                    if (f.vtx[size_t(i)] >= part.vertices.size()) break;
                    const Vec3i16& v = part.vertices[f.vtx[size_t(i)]];
                    c.push_back({v.x, v.y, v.z});
                }
                if (c.size() < 3 || c.size() != size_t(f.count)) continue;
                std::sort(c.begin(), c.end());
                by_corners[c].push_back(fi);
            }
            for (const auto& [corners, faces] : by_corners)
                for (size_t a : faces)
                    for (size_t b : faces) {
                        const Face& fa = part.faces[a];
                        const Face& fb = part.faces[b];
                        if (int64_t(fa.normal[0]) * fb.normal[0] + int64_t(fa.normal[1]) * fb.normal[1] + int64_t(fa.normal[2]) * fb.normal[2] < 0)
                            part.faces[a].card = true;
                    }
            // ... standing free of the rest of the part: every edge shared with cards only (a
            // wing, its fins and supports; not the vents and panels set into the bodywork)
            using Pos = std::array<int, 3>;
            std::map<std::pair<Pos, Pos>, std::vector<size_t>> edge_faces;
            auto pos = [&](uint16_t k) { const Vec3i16& v = part.vertices[k]; return Pos{v.x, v.y, v.z}; };
            for (size_t fi = 0; fi < part.faces.size(); ++fi) {
                const Face& f = part.faces[fi];
                for (int i = 0; i < f.count && i < 6; ++i) {
                    const uint16_t a = f.vtx[size_t(i)], b = f.vtx[size_t((i + 1) % f.count)];
                    if (a >= part.vertices.size() || b >= part.vertices.size()) continue;
                    Pos pa = pos(a), pb = pos(b);
                    if (pb < pa) std::swap(pa, pb);
                    edge_faces[{pa, pb}].push_back(fi);
                }
            }
            for (bool changed = true; changed;) {
                changed = false;
                for (const auto& [edge, faces] : edge_faces) {
                    bool all_cards = true;
                    for (size_t fi : faces) all_cards = all_cards && part.faces[fi].card;
                    if (all_cards) continue;
                    for (size_t fi : faces)
                        if (part.faces[fi].card) {
                            part.faces[fi].card = false;
                            changed = true;
                        }
                }
            }
        }
        if (part.wheel) model.car = true;
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
