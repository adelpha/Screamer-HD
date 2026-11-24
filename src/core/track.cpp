#include "core/track.h"

#include "core/archive.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace scr {
namespace {

int32_t le32(const std::vector<uint8_t>& d, size_t o) {
    int32_t v = 0;
    if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4);
    return v;
}

} // namespace

std::optional<MaterialLibrary> MaterialLibrary::parse(std::vector<uint8_t> mlb) {
    if (mlb.size() < 16) return std::nullopt;
    MaterialLibrary lib;
    const int32_t first_desc = le32(mlb, 4);
    if (first_desc <= 0 || first_desc % 16 || size_t(first_desc) > mlb.size()) return std::nullopt;
    for (size_t i = 0; i < size_t(first_desc) / 16; ++i) {
        Material m;
        const int32_t handler = le32(mlb, i * 16);
        const int32_t desc = le32(mlb, i * 16 + 4);
        m.type = handler / 16;
        m.width = le32(mlb, i * 16 + 8);
        m.height = le32(mlb, i * 16 + 12);
        if (desc > 0 && size_t(desc) + 12 <= mlb.size()) {
            m.frames = le32(mlb, size_t(desc));
            m.color = uint8_t(le32(mlb, size_t(desc) + 4));
            m.pixels = uint32_t(le32(mlb, size_t(desc) + 8));
            for (int f = 0; f < m.frames && f < 64 && size_t(desc) + 12 + 4 * size_t(f) <= mlb.size(); ++f)
                m.frame_pixels.push_back(uint32_t(le32(mlb, size_t(desc) + 8 + 4 * size_t(f))));
        }
        lib.materials_.push_back(m);
    }
    lib.data_ = std::move(mlb);
    return lib;
}

const Material* MaterialLibrary::find(uint16_t offset) const {
    const size_t i = offset / 16;
    return i < materials_.size() ? &materials_[i] : nullptr;
}

uint8_t MaterialLibrary::texel(const Material& m, int u, int v, int frame) const {
    if (m.width <= 0 || m.height <= 0) return m.color;
    u %= m.width;
    if (u < 0) u += m.width;
    v %= m.height;
    if (v < 0) v += m.height;
    const uint32_t base = m.frame_pixels.empty() ? m.pixels : m.frame_pixels[size_t(frame) % m.frame_pixels.size()];
    const size_t o = size_t(base) + size_t(v) * 256 + size_t(u);
    return o < data_.size() ? data_[o] : m.color;
}

std::optional<std::vector<Model>> parse_unr(const std::vector<uint8_t>& unr) {
    if (unr.size() < 8) return std::nullopt;
    const uint32_t data_size = uint32_t(le32(unr, 0)), count = uint32_t(le32(unr, 4));
    const size_t base = 8 + size_t(count) * 4;
    if (count > 10000 || base + data_size > unr.size()) return std::nullopt;
    std::vector<Model> out;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t off = uint32_t(le32(unr, 8 + 4 * size_t(i)));
        const uint32_t end = i + 1 < count ? uint32_t(le32(unr, 8 + 4 * size_t(i + 1))) : data_size;
        if (off >= data_size || end > data_size || end <= off) return std::nullopt;
        auto m = parse_model(unr.data() + base + off, end - off);
        if (!m) return std::nullopt;
        out.push_back(std::move(*m));
    }
    return out;
}

std::optional<std::vector<Placement>> parse_mmp(const std::vector<uint8_t>& mmp, size_t object_count) {
    if (mmp.size() < 1024) return std::nullopt;
    std::vector<Placement> out;
    for (int r = 0; r < 256; ++r) {
        const int32_t ro = le32(mmp, size_t(r) * 4);
        if (ro == -1) continue;
        if (ro < 0 || size_t(ro) + 1024 > mmp.size()) return std::nullopt;
        for (int c = 0; c < 256; ++c) {
            const int32_t co = le32(mmp, size_t(ro) + size_t(c) * 4);
            if (co == -1) continue;
            if (co < 0 || size_t(co) + 40 > mmp.size()) return std::nullopt;
            for (int slot = 0; slot < 2; ++slot) {
                const int32_t so = le32(mmp, size_t(co) + size_t(slot) * 4);
                const int32_t n = le32(mmp, size_t(co) + 20 + size_t(slot) * 4);
                for (int32_t k = 0; k < n; ++k) {
                    const size_t e = size_t(so) + size_t(k) * 64;
                    if (e + 32 > mmp.size()) return std::nullopt;
                    Placement p;
                    p.object = le32(mmp, e);
                    p.flags = le32(mmp, e + 4);
                    p.x = le32(mmp, e + 8);
                    p.y = le32(mmp, e + 12);
                    p.z = le32(mmp, e + 16);
                    p.ax = le32(mmp, e + 20);
                    p.ay = le32(mmp, e + 24);
                    p.az = le32(mmp, e + 28);
                    p.animation = le32(mmp, e + 32);
                    p.slot = slot;
                    if (p.object >= 0 && size_t(p.object) < object_count) out.push_back(p);
                }
            }
        }
    }
    return out;
}

std::array<uint8_t, 256> build_darken_table(const uint8_t* rgb6, float factor) {
    std::array<uint8_t, 256> out{};
    for (int i = 0; i < 256; ++i) {
        float t[3];
        for (int c = 0; c < 3; ++c) t[c] = std::min(255.0f, float(rgb6[3 * i + c]) * factor);
        float best = 2.21e7f;
        int best_j = 0;
        for (int j = 0; j < 256; ++j) {
            float d = 0;
            for (int c = 0; c < 3; ++c) {
                const float e = float(rgb6[3 * j + c]) - t[c];
                d += e * e;
            }
            if (d < best) { best = d; best_j = j; }
        }
        out[size_t(i)] = uint8_t(best_j);
    }
    return out;
}

std::optional<Model> load_object(const std::string& dat_path, const std::string& file_name) {
    auto a = Archive::open(dat_path);
    if (!a) return std::nullopt;
    auto bytes = a->read(file_name);
    if (!bytes) return std::nullopt;
    return parse_model(bytes->data(), bytes->size());
}

void parse_mmp_track_lines(const std::vector<uint8_t>& mmp, std::vector<TrackPoint>& points,
                           std::vector<TrackSection>& sections) {
    auto f32 = [&](size_t o) {
        float v = 0;
        if (o + 4 <= mmp.size()) std::memcpy(&v, mmp.data() + o, 4);
        return v;
    };
    if (mmp.size() < 1024) return;
    for (int r = 0; r < 256; ++r) {
        const int32_t ro = le32(mmp, size_t(r) * 4);
        if (ro < 0 || size_t(ro) + 1024 > mmp.size()) continue;
        for (int c = 0; c < 256; ++c) {
            const int32_t co = le32(mmp, size_t(ro) + size_t(c) * 4);
            if (co < 0 || size_t(co) + 40 > mmp.size()) continue;
            const size_t p2 = size_t(le32(mmp, size_t(co) + 8)), p3 = size_t(le32(mmp, size_t(co) + 12));
            const int32_t n2 = le32(mmp, size_t(co) + 28), n3 = le32(mmp, size_t(co) + 32);
            for (int32_t k = 0; k < n2 && p2 + 12 * size_t(k + 1) <= mmp.size(); ++k) {
                const size_t e = p2 + 12 * size_t(k);
                points.push_back({f32(e), le32(mmp, e + 4), le32(mmp, e + 8)});
            }
            for (int32_t k = 0; k < n3 && p3 + 28 * size_t(k + 1) <= mmp.size(); ++k) {
                const size_t e = p3 + 28 * size_t(k);
                sections.push_back({f32(e), le32(mmp, e + 4), le32(mmp, e + 8), le32(mmp, e + 12),
                                    le32(mmp, e + 16), le32(mmp, e + 20), le32(mmp, e + 24)});
            }
        }
    }
}

namespace {

template <typename F>
void for_each_ground_face(const Track& track, float x, float z, F&& f_) {
    constexpr float kAngle = 6.28318530718f / 2048.0f;
    for (const Placement& p : track.placements) {
        if (p.slot != 0 || std::fabs(float(p.x) - x) > 12288 || std::fabs(float(p.z) - z) > 12288) continue;
        // Into object space: undo the yaw (objects turn by -ay, see render3d.cpp).
        const float a = float(p.ay) * kAngle, c = std::cos(a), s = std::sin(a);
        const float dx = x - float(p.x), dz = z - float(p.z);
        const float lx = c * dx + s * dz, lz = -s * dx + c * dz;
        for (const Part& part : track.objects[size_t(p.object)].parts) {
            for (const Face& f : part.faces) {
                if (f.normal[1] == 0) continue;  // walls
                bool inside = false;
                for (int i = 0, j = f.count - 1; i < f.count; j = i++) {
                    const Vec3i16& vi = part.vertices[f.vtx[size_t(i)]];
                    const Vec3i16& vj = part.vertices[f.vtx[size_t(j)]];
                    if ((vi.z > lz) != (vj.z > lz) &&
                        lx < float(vj.x - vi.x) * (lz - float(vi.z)) / float(vj.z - vi.z) + float(vi.x))
                        inside = !inside;
                }
                if (!inside) continue;
                // Plane through the first vertex with the face normal.
                const Vec3i16& v0 = part.vertices[f.vtx[0]];
                const float nx = float(f.normal[0]), ny = float(f.normal[1]), nz = float(f.normal[2]);
                const float y = float(v0.y) - (nx * (lx - float(v0.x)) + nz * (lz - float(v0.z))) / ny + float(p.y);
                f_(y, part);
            }
        }
    }
}

} // namespace

std::optional<float> ground_height(const Track& track, float x, float z, float below_y) {
    std::optional<float> best;
    for_each_ground_face(track, x, z, [&](float y, const Part&) {
        if (y <= below_y && (!best || y > *best)) best = y;
    });
    return best;
}

std::optional<GroundHit> ground_query(const Track& track, int x, int z, bool walls_only) {
    // collision_query_cells / collision_query_cell_objects / collision_test_quad|triangle:
    // the probe is turned into the placement's frame (rotate_xz, truncated to integers),
    // then each face of the road object is tested in x/z with integer edge functions
    // (inclusive, one winding). Missed points are tried again 10 and 20 units further along
    // +x/+z.
    for (const Placement& p : track.placements) {
        if (p.slot != 0 || std::abs(p.x - x) > 12288 || std::abs(p.z - z) > 12288) continue;
        const double a = double(float(p.ay) * 0.0009765625f * 3.1415927410125732f);
        const double c = std::cos(a), s = std::sin(a);
        const int dx = x - p.x, dz = z - p.z;
        const int rx = int(double(dz) * s + double(dx) * c) + p.x;
        const int rz = int(double(dz) * c - double(dx) * s) + p.z;
        for (int t = 0; t < 3; ++t) {
            const int px = rx + 10 * t, pz = rz + 10 * t;
            for (const Part& part : track.objects[size_t(p.object)].parts) {
                for (const Face& f : part.faces) {
                    if (!walls_only && f.normal[1] == 0) continue;
                    int vx[6], vz[6];
                    int minx = INT32_MAX, maxx = INT32_MIN, minz = INT32_MAX, maxz = INT32_MIN;
                    for (int i = 0; i < f.count; ++i) {
                        const Vec3i16& v = part.vertices[f.vtx[size_t(i)]];
                        vx[i] = v.x + p.x;
                        vz[i] = v.z + p.z;
                        minx = std::min(minx, vx[i]); maxx = std::max(maxx, vx[i]);
                        minz = std::min(minz, vz[i]); maxz = std::max(maxz, vz[i]);
                    }
                    if (px > maxx || px < minx || pz > maxz || pz < minz) continue;
                    bool inside = true;
                    for (int i = 0; i < f.count && inside; ++i) {
                        const int j = (i + 1) % f.count;
                        const int64_t e = int64_t(vz[j] - vz[i]) * (px - vx[i]) + int64_t(vx[i] - vx[j]) * (pz - vz[i]);
                        if (e < 0) inside = false;
                    }
                    if (!inside) continue;
                    GroundHit hit;
                    hit.surface = part.name.size() > 3 ? part.name[3] : char(0);
                    hit.normal_x = f.normal[0];
                    hit.normal_z = f.normal[2];
                    if (f.normal[1] != 0) {
                        const Vec3i16& v0 = part.vertices[f.vtx[0]];
                        const int64_t num = int64_t(px - (v0.x + p.x)) * f.normal[0] + int64_t(pz - (v0.z + p.z)) * f.normal[2];
                        hit.y = int(-num / f.normal[1]) + v0.y + p.y;
                    } else {
                        hit.y = p.y;
                    }
                    return hit;
                }
            }
        }
    }
    return std::nullopt;
}

const TrackPoint* nearest_track_point(const Track& track, int x, int z) {
    const TrackPoint* best = nullptr;
    int64_t bd = 0;
    for (const TrackPoint& p : track.points) {
        const int64_t dx = p.x - x, dz = p.z - z, d = dx * dx + dz * dz;
        if (!best || d < bd) { best = &p; bd = d; }
    }
    return best;
}

const TrackSection* nearest_track_section(const Track& track, int x, int z) {
    const TrackSection* best = nullptr;
    int64_t bd = 0;
    for (const TrackSection& s : track.sections) {
        const int64_t dx = s.x - x, dz = s.z - z, d = dx * dx + dz * dz;
        if (!best || d < bd) { best = &s; bd = d; }
    }
    return best;
}

std::optional<Track> load_track(const std::string& dat_path, const std::string& name) {
    auto a = Archive::open(dat_path);
    if (!a) return std::nullopt;
    auto unr = a->read(name + ".UNR");
    auto mmp = a->read(name + ".MMP");
    auto mlb = a->read(name + ".MLB");
    auto col = a->read(name + ".COL");
    if (!unr || !mmp || !mlb || !col || col->size() < 768) return std::nullopt;
    Track t;
    auto objects = parse_unr(*unr);
    if (!objects) return std::nullopt;
    t.objects = std::move(*objects);
    auto placements = parse_mmp(*mmp, t.objects.size());
    if (!placements) return std::nullopt;
    t.placements = std::move(*placements);
    parse_mmp_track_lines(*mmp, t.points, t.sections);
    t.materials = MaterialLibrary::parse(std::move(*mlb));
    if (!t.materials) return std::nullopt;
    t.palette = palette_from_vga(col->data());
    t.darken = build_darken_table(col->data(), 0.6f);
    if (auto smg = a->read(name + ".SMG"); smg && smg->size() >= 64 * 256) t.shade_table = std::move(*smg);
    if (auto bck = a->read(name + ".BCK")) t.background = decode_pcx(*bck);
    return t;
}

} // namespace scr
