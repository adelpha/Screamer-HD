#pragma once
// Screamer's 3D objects: .3DX files and the objects inside a track's .UNR file share
// one layout (decoded from obj_load_3dx, object_begin, object_transform_vertices and
// object_build_polygons in STARTL.EXE).
//
// Object (all offsets from the object start):
//   +0x14 int32 part count, +0x18 int32 part offsets[...]
// Part:
//   +0x04 int16 centre x, y, z (bounding sphere), +0x0c int32 placement offset (object-relative):
//         { int32 ?, angle x, angle y, angle z, x, y, z } - where the part sits in the object
//         (car wheels; zero for most parts)
//   +0x10 int32 vertex count (informational), +0x14 int32 face count
//   +0x18 int32 flags (0x2000: environment-mapped, see Part::vertex_normals)
//   +0x20 int32 vertex normal list offset (object-relative, 0x2000 parts only)
//   +0x1c int32 vertex list offset (object-relative): int16 x, y, z per vertex, ends with x == 0x7fff
//   +0x24 int32 uv list offset (object-relative): int16 u, v at the same byte offset as each vertex
//   +0x28 int32 draw-order offsets[16] (part-relative, one per view direction): int16 face
//         offsets, ends with a value <= 0
//   +0x68 char name[...]
// Face (0x24 bytes, part-relative offset):
//   +0x02 int16 flat colour (material types 8 and 9)
//   +0x04 uint16 material offset (into the track's material table, 16 bytes per material)
//   +0x06 uint16 vertex count (up to 6)
//   +0x08 uint16 vertex byte offsets[6] (into the vertex list)
//   +0x14 int32 normal x, y, z (2.14 fixed point), +0x20 int32 plane distance
// World axes: y is up; angles 0..0x7ff per turn. Face normals point into the object.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scr {

struct Vec3i16 {
    int16_t x = 0, y = 0, z = 0;
};

struct Face {
    uint8_t color = 0;                // +0x02: fill colour of flat material types (8, 9)
    uint16_t material = 0;            // byte offset into the material table
    uint8_t count = 0;                // number of vertices (3..6)
    std::array<uint16_t, 6> vtx{};    // vertex indices
    std::array<int32_t, 3> normal{};  // 2.14 fixed point
    int32_t plane = 0;
    // Bit i: the edge from vertex i to the next is not shared with another face of the part
    // (compute_bounds works it out; all set until then). The renderer seals only these edges
    // of road pieces against their neighbours (Pose::seal_edges).
    uint8_t open_edges = 0x3f;
    // A long narrow face (compute_bounds): on road pieces, a painted line.
    bool strip = false;
    // Bit i: the face is in the part's draw order i (Part::draw_order). The original draws
    // only the faces of the list for where the eye is; a few faces are left out of some.
    uint16_t lists = 0xffff;
    // The face is not flat: a corner lies well off the plane of its first three (a few faces
    // of the tracks' cliffs are folded along a diagonal). The original tests the whole face
    // for facing the eye by its one normal; the renderer tests each half of such a face.
    bool folded = false;
    // One side of a card: another face of the part has the same corners, facing the other
    // way (thin fins and supports, a face per side). compute_bounds works it out.
    bool card = false;
};

struct Part {
    std::string name;
    std::array<int32_t, 3> angles{};  // part placement in the object: rotation (x, y, z) ...
    std::array<int32_t, 3> offset{};  // ... then translation
    std::vector<Vec3i16> vertices;
    std::vector<std::array<int16_t, 2>> uvs;  // per vertex, 0..255 across the texture (texel = uv * size / 256)
    std::vector<Face> faces;
    int32_t flags = 0;  // +0x18; 0x2000: environment-mapped (object_transform_float)
    // +0x20 (object-relative) for 0x2000 parts: int32 count, float x, y, z per vertex - the
    // vertex normals used for the car glass reflections.
    std::vector<std::array<float, 3>> vertex_normals;
    // Animated objects: per frame, the part's matrix (row-major, 2.14) and position. They are
    // stored with y and z swapped on the output side (and the game flips its back-face test
    // for them): the part's transform is v' = P (M v + t) with P swapping y and z.
    std::vector<std::array<int16_t, 12>> frames;
    // Bounding sphere of the vertices (part space), for skipping parts outside the view;
    // radius < 0: unknown (never skipped).
    std::array<float, 3> center{};
    float radius = -1;
    // collision_query_cell_objects / collision_test_object_faces (the first part of a track
    // object is its collision mesh): the bounding circle (+0 radius, +4 / +8 int16 centre
    // x / z) and the faces of draw order 0 (+0x28, +0x14 of them) as indices into `faces`.
    int32_t bound_r = 0;
    int16_t bound_x = 0, bound_z = 0;
    std::vector<int> collision_faces;
    // One of a car's wheel parts (the names in the table at 0x101d58: flx fli blx rli frx fri
    // brx rri).
    bool wheel = false;
    // +0x28: the faces in the order the original draws them (it has no depth buffer), one
    // list for each of 16 directions the part can be seen from (draw_order_index); a face
    // missing from a list is not drawn from there. Indices into faces.
    std::array<std::vector<uint16_t>, 16> draw_order;
    int16_t bound_y = 0;  // +0x06 (with bound_x, bound_z: the centre the original measures distance to)
};

// Which of a part's draw orders is used with the eye at (x, y, z) in the part's own space
// (clip_outcode: below or above, then eight sectors round the y axis).
inline int draw_order_index(float x, float y, float z) {
    int i = y < 0 ? 8 : 0;
    if (x >= 0) {
        if (z >= 0) return i + (z > x ? 0 : 1);
        return i + 2 + (-z > x ? 1 : 0);
    }
    x = -x;
    if (z < 0) return i + 4 + (-z > x ? 0 : 1);
    return i + 6 + (z > x ? 1 : 0);
}

// A part's simpler version for distant objects (Model::lods).
struct LodPart {
    bool same = false;     // the full part again
    bool present = false;  // `part` is its own simpler version; neither: not drawn
    Part part;
};

struct Model {
    std::vector<Part> parts;
    // object_compute_depth's level-of-detail tables (beyond 4000 and 7000 units for track
    // objects); empty when every part stays the same.
    std::array<std::vector<LodPart>, 2> lods;
    // Animated objects (object header +0x10 >= 0): the number of frames; each part then has
    // a matrix and position per frame (Part::frames), the frame following the global
    // animation counter (frames * counter >> 16).
    int frames = 0;
    // Bounding sphere around the object's origin that holds every part however it is placed;
    // radius < 0: unknown.
    float radius = -1;
    // Every vertex at the same height: a flat piece of ground (the grass and earth filler
    // quads around the road).
    bool flat = false;
    int flat_y = 0;
    // The lowest and highest vertex (with the parts' offsets; their turns left out).
    int min_y = 0, max_y = 0;
    // Some face is a strip (Face::strip): a piece of road with painted lines, wherever the
    // track file lists it (stretches of road are also among the scenery, MMP slot 1).
    bool has_strips = false;
    // A car (it has wheel parts). Drawn as the original draws an object, with no depth test
    // between its own faces: its parts far to near, tyres and hub discs first, each part's
    // faces in the order of its draw_order list for the view. The bodywork then covers the
    // wheels and the dark faces behind them wherever the original's does.
    bool car = false;
    // +0x32c: where the object goes in the original's drawing order (vis_sort_cells). It
    // draws objects far to near with no depth buffer; an object of class 1, 2, 3 or 4 is
    // drawn as if it were 6000, 3200, 24000 or 800 units further away, so everything up to
    // that much behind it is painted over it (cliff faces under the road are class 3: the
    // road, its verges and the hills beyond cover the top of them, which stands above the road).
    int32_t draw_class = 0;
};

// A vertex of a placed object drawn a little away from where the file has it (Track::welds):
// the shift is in the object's own axes.
struct VertexShift {
    uint16_t part = 0, vertex = 0;
    float dx = 0, dy = 0, dz = 0;
};

// A part's placement set by the game for one frame (the instance's part table, f_10 in
// g_objects, 0x20 bytes per part): angles in 2048 units replacing the file's, and the height.
struct PartPose {
    bool angles = false;
    int32_t ax = 0, ay = 0, az = 0;
    bool y = false;
    int32_t oy = 0;
};

// The parts of a car that car_sync_render_object / opponent_sync_render_object move: the
// body sections (fauto, rauto, MV_mauto, fai: lean and suspension) and the wheels (table at
// 0x101d58: flx fli blx rli frx fri brx rri; the first two pairs steer).
struct CarParts {
    std::vector<int> body;    // part indices
    std::vector<int> wheels;
    std::vector<int> steered;
};
CarParts find_car_parts(const Model& car);

// Fills in Part::center/radius and Model::radius (parse_model does this).
void compute_bounds(Model& model);

// Parses one object starting at `data` (size bytes available).
std::optional<Model> parse_model(const uint8_t* data, size_t size);

} // namespace scr
