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
//             angle x, angle y, angle z, animation (-1), ... } (slot 0: road pieces, slot 1: scenery).
//             The game finds the ground under a car by testing the faces of the cell's slot 0
//             objects (collision_query_cells); the 4th character of the part name is the
//             surface type. Slot 2: 12-byte track points { float heading (radians), int32 x, z }
//             (track_find_nearest_point). Slot 3: 28-byte road cross-sections { float heading,
//             int32 centre x, z, left x, z, right x, z }. Slot 4: one 36+ byte record per cell
//             (bounds and a copy of the slot table; unused here).
//   MAPn.MLB  materials: 16 bytes each { int32 handler offset (type * 16), int32 texture
//             descriptor offset, int32 width, int32 height }, up to the first descriptor;
//             descriptor { int32 frame count, int32 average colour, int32 pixel offsets[frames] };
//             texture pixels are stored with a 256-byte row stride. Car materials have 8
//             frames picked by the object's animation bits (see Pose::animation).
//   MAPn.COL  768-byte palette (6-bit VGA values).
//   MAPn.BCK  PCX, 512 x 256 sky/background panorama (render_draw_background).
//   MAPn.SMG  16 KB translucency table (engine_load_shade_table): new = smg[texel * 256 + old]
//             for material types 14/15 (shadows, smoke, glass); texel 0..63. Rows 0-31 lighten,
//             32-47 darken, 48-63 tint.

#include "core/framebuffer.h"
#include "core/model3d.h"
#include "core/pcx.h"

#include <array>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <cmath>
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

// Track centre-line point (MMP slot 2) and road cross-section (slot 3).
struct TrackPoint {
    float heading = 0;  // radians
    int x = 0, z = 0;
};
struct TrackSection {
    float heading = 0;
    int x = 0, z = 0;              // centre
    int left_x = 0, left_z = 0;
    int right_x = 0, right_z = 0;
};

struct Placement {
    int object = 0;
    int flags = 0;
    int x = 0, y = 0, z = 0;
    int ax = 0, ay = 0, az = 0;  // 0..0x7ff per turn
    int animation = -1;          // +0x20: texture animation word (-1 = global counter, see Pose)
    int reflection = 0;          // +0x3c: see Pose::reflection
    int slot = 0;
    int cell_row = 0, cell_col = 0;  // the MMP cell it is listed in (8192 units, offset 0x100000)
};

class MaterialLibrary {
public:
    static std::optional<MaterialLibrary> parse(std::vector<uint8_t> mlb);
    const Material* find(uint16_t offset) const;  // by face material offset
    const std::vector<Material>& materials() const { return materials_; }
    // Texel of the given frame (wrapped to the frame count), coordinates wrapped to the material size.
    uint8_t texel(const Material& m, int u, int v, int frame = 0) const;
    const std::vector<uint8_t>& data() const { return data_; }

private:
    std::vector<uint8_t> data_;
    std::vector<Material> materials_;
};

struct MipSet;

struct Track {
    std::vector<Model> objects;
    std::vector<Placement> placements;
    // Slot-0 placements of each MMP cell in file order (collision_query_cell_objects), by
    // row * 256 + col.
    std::unordered_map<int, std::vector<int>> cells;
    std::vector<TrackPoint> points;
    std::vector<TrackSection> sections;
    std::optional<MaterialLibrary> materials;
    // Smaller versions of the materials' textures (core/mipmaps.h), in this palette; may be null.
    std::shared_ptr<const MipSet> mips;
    // Per placement (or empty): the corners of road pieces moved to meet the neighbouring
    // piece's (weld_road_joints). For drawing only; the game's own sums use the file's vertices.
    std::vector<std::vector<VertexShift>> welds;
    std::optional<PcxImage> background;
    Palette palette{};
    std::vector<uint8_t> shade_table;          // MAPn.SMG, 64 x 256 (empty if missing)
    std::array<uint8_t, 256> darken{};        // build_darken_tables' x0.6 table (types 12/13)
    std::array<uint8_t, 256> darken075{};     // build_darken_tables' x0.75 table (HUD radar)
};

// Palette remap to the nearest colour at `factor` brightness, as build_darken_tables does
// with the 6-bit VGA palette (first match wins).
std::array<uint8_t, 256> build_darken_table(const uint8_t* rgb6, float factor);

// The track files place the road as pieces with whole-number positions and angles (2048 to
// the turn), and the ends of neighbouring pieces miss each other by some twenty units: the
// painted lines step sideways at every joint. This gathers the corners on the outlines of the
// road pieces (MMP slot 0) that lie within reach units of a corner of another piece, and notes
// for each the shift to the middle of its group (Track::welds).
void weld_road_joints(Track& track, double reach = 48.0);

// Loads MAPn.UNR/.MMP/.MLB/.COL from the given graphics archive (e.g. GFX/MAP2L.DAT).
std::optional<Track> load_track(const std::string& dat_path, const std::string& name);

// Loads one .3DX object (e.g. "AUTO00.3DX", "SHADOW.3DX") from an archive.
std::optional<Model> load_object(const std::string& dat_path, const std::string& file_name);

std::optional<std::vector<Model>> parse_unr(const std::vector<uint8_t>& unr);
std::optional<std::vector<Placement>> parse_mmp(const std::vector<uint8_t>& mmp, size_t object_count);
// Track points (slot 2) and cross-sections (slot 3) of every cell.
void parse_mmp_track_lines(const std::vector<uint8_t>& mmp, std::vector<TrackPoint>& points,
                           std::vector<TrackSection>& sections);

// Height of the ground (the faces of the road objects, MMP slot 0) at x, z: the highest face
// below `below_y` whose outline contains the point. Empty when there is no road there.
std::optional<float> ground_height(const Track& track, float x, float z, float below_y = 1e9f);

struct GroundHit {
    int y = 0;           // integer height, like collision_test_object_faces
    char surface = 0;    // 4th letter of the road part's name ('e' = bumpy)
    int normal_x = 0, normal_z = 0;  // the face's normal turned into the world (2.14)
};
// collision_query_cells: the ground under x, z, or empty off the road (the game treats that
// as a wall). walls_only = the corner probes' variant (collision_test_object_faces_b), which
// accepts any face, not only those with a vertical normal component.
std::optional<GroundHit> ground_query(const Track& track, int x, int z, bool walls_only = false);

// rotate_xz: out.x = trunc(z sin a + x cos a), out.z = trunc(z cos a - x sin a), with sin and
// cos rounded to float and the rest at FPU precision, as the original.
inline void rotate_xz(int& ox, int& oz, int x, int z, float angle) {
    const double c = double(float(std::cos(double(angle)))), s = double(float(std::sin(double(angle))));
    ox = int(double(z) * s + double(x) * c);
    oz = int(double(z) * c - double(x) * s);
}

// track_find_nearest_point: nearest centre-line point (slot 2) and cross-section (slot 3).
const TrackPoint* nearest_track_point(const Track& track, int x, int z);
const TrackSection* nearest_track_section(const Track& track, int x, int z);

} // namespace scr
