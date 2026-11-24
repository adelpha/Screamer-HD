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
//             descriptor { int32 frame count, int32 average colour, int32 pixel offsets[frames] };
//             texture pixels are stored with a 256-byte row stride. Car materials have 8
//             frames picked by the object's animation bits (see Pose::frame).
//   MAPn.COL  768-byte palette (6-bit VGA values).
//   MAPn.BCK  PCX, 512 x 256 sky/background panorama (render_draw_background).
//   MAPn.SMG  16 KB translucency table (engine_load_shade_table): new = smg[texel * 256 + old]
//             for material types 14/15 (shadows, smoke, glass); texel 0..63. Rows 0-31 lighten,
//             32-47 darken, 48-63 tint.

#include "core/framebuffer.h"
#include "core/model3d.h"
#include "core/pcx.h"

#include <array>
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
    std::vector<uint32_t> frame_pixels;  // every frame's pixel offset
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
    // Texel of the given frame (wrapped to the frame count), coordinates wrapped to the material size.
    uint8_t texel(const Material& m, int u, int v, int frame = 0) const;

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
    std::vector<uint8_t> shade_table;          // MAPn.SMG, 64 x 256 (empty if missing)
    std::array<uint8_t, 256> darken{};        // build_darken_tables' x0.6 table (types 12/13)
};

// Palette remap to the nearest colour at `factor` brightness, as build_darken_tables does
// with the 6-bit VGA palette (first match wins).
std::array<uint8_t, 256> build_darken_table(const uint8_t* rgb6, float factor);

// Loads MAPn.UNR/.MMP/.MLB/.COL from the given graphics archive (e.g. GFX/MAP2L.DAT).
std::optional<Track> load_track(const std::string& dat_path, const std::string& name);

// Loads one .3DX object (e.g. "AUTO00.3DX", "SHADOW.3DX") from an archive.
std::optional<Model> load_object(const std::string& dat_path, const std::string& file_name);

std::optional<std::vector<Model>> parse_unr(const std::vector<uint8_t>& unr);
std::optional<std::vector<Placement>> parse_mmp(const std::vector<uint8_t>& mmp, size_t object_count);

} // namespace scr
