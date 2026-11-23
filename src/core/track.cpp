#include "core/track.h"

#include "core/archive.h"

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

uint8_t MaterialLibrary::texel(const Material& m, int u, int v) const {
    if (m.width <= 0 || m.height <= 0) return m.color;
    u %= m.width;
    if (u < 0) u += m.width;
    v %= m.height;
    if (v < 0) v += m.height;
    const size_t o = size_t(m.pixels) + size_t(v) * 256 + size_t(u);
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
                    p.slot = slot;
                    if (p.object >= 0 && size_t(p.object) < object_count) out.push_back(p);
                }
            }
        }
    }
    return out;
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
    t.materials = MaterialLibrary::parse(std::move(*mlb));
    if (!t.materials) return std::nullopt;
    t.palette = palette_from_vga(col->data());
    if (auto bck = a->read(name + ".BCK")) t.background = decode_pcx(*bck);
    return t;
}

} // namespace scr
