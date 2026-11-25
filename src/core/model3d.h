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
    // One of a car's wheel parts (the names in the table at 0x101d58: flx fli blx rli frx fri
    // brx rri).
    bool wheel = false;
};

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
