#pragma once
// A track (gfx\mapNl.dat / mapNh.dat), decoded from engine_load_unr, engine_load_mmp,
// engine_relocate_map and engine_load_materials in STARTL.EXE:
//
//   MAPn.UNR  uint32 data size, uint32 object count, uint32 object offsets[count], data:
//             the track's 3D objects (see model3d.h).
//   MAPn.MMP  256 x 256 map cells of 8192 x 8192 units (cell = (coord + 0x100000) >> 13):
//             int32 row offsets[256] (-1 = empty) -> int32 cell offsets[256] (-1 = empty) ->
//             cell { int32 slot offsets[5]; int32 slot counts[5] }.
//             Slots 0 and 1 hold 64-byte placements { int32 object, flags, x, y, z,
//             angle x, angle y, angle z, ... } (slot 0: road pieces, slot 1: scenery);
//             slots 2-4 hold other data (collision and so on, not decoded yet).
//   MAPn.MLB  materials: 16 bytes each { int32 handler offset (type * 16), int32 texture
//             descriptor offset, int32 width, int32 height }, up to the first descriptor;
//             descriptor { int32 frame count, int32 average colour, int32 pixel offset, ... };
//             texture pixels are stored with a 256-byte row stride.
//   MAPn.COL  768-byte palette (6-bit VGA values).
//   MAPn.BCK  PCX, 512 x 256 sky/background panorama (render_draw_background).

#include "core/framebuffer.h"
#include "core/model3d.h"
#include "core/pcx.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scr {

struct Material {
    int type = 0;        // index into the renderer's handler table (0..15)
    int width = 0;
    int height = 0;
    uint8_t color = 0;   // average colour, used when not texturing
    int frames = 0;
    uint32_t pixels = 0; // offset of the first frame's pixels in the material data
};

struct Placement {
    int object = 0;
    int flags = 0;
    int x = 0, y = 0, z = 0;
    int ax = 0, ay = 0, az = 0;  // 0..0x7ff per turn
    int slot = 0;
};

class MaterialLibrary {
public:
    static std::optional<MaterialLibrary> parse(std::vector<uint8_t> mlb);
    const Material* find(uint16_t offset) const;  // by face material offset
    const std::vector<Material>& materials() const { return materials_; }
    // Texel of frame 0, coordinates wrapped to the material size.
    uint8_t texel(const Material& m, int u, int v) const;

private:
    std::vector<uint8_t> data_;
    std::vector<Material> materials_;
};

struct Track {
    std::vector<Model> objects;
    std::vector<Placement> placements;
    std::optional<MaterialLibrary> materials;
    std::optional<PcxImage> background;
    Palette palette{};
};

// Loads MAPn.UNR/.MMP/.MLB/.COL from the given graphics archive (e.g. GFX/MAP2L.DAT).
std::optional<Track> load_track(const std::string& dat_path, const std::string& name);

std::optional<std::vector<Model>> parse_unr(const std::vector<uint8_t>& unr);
std::optional<std::vector<Placement>> parse_mmp(const std::vector<uint8_t>& mmp, size_t object_count);

} // namespace scr
