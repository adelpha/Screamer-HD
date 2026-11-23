#include "core/model3d.h"

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
    for (int32_t k = 0; k < nparts; ++k) {
        int32_t po = 0;
        if (!rd(d, size, 0x18 + 4 * size_t(k), po) || po <= 0 || size_t(po) >= size) return std::nullopt;
        const size_t p = size_t(po);
        int32_t voff = 0, uvoff = 0, order0 = 0;
        if (!rd(d, size, p + 0x1c, voff) || !rd(d, size, p + 0x24, uvoff) || !rd(d, size, p + 0x28, order0)) return std::nullopt;
        Part part;
        for (size_t i = 0; i < 24 && p + 0x68 + i < size && d[p + 0x68 + i]; ++i) part.name.push_back(char(d[p + 0x68 + i]));

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

        // The first draw order lists every face of the part.
        std::set<int> seen;
        for (size_t q = p + size_t(order0);; q += 2) {
            int16_t fo = 0;
            if (!rd(d, size, q, fo) || fo <= 0) break;
            if (!seen.insert(fo).second) continue;
            const size_t f = p + size_t(fo);
            Face face;
            uint16_t n = 0;
            if (!rd(d, size, f + 4, face.material) || !rd(d, size, f + 6, n)) return std::nullopt;
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
    return m;
}

} // namespace scr
