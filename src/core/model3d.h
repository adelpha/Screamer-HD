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
};

struct Model {
    std::vector<Part> parts;
};

// Parses one object starting at `data` (size bytes available).
std::optional<Model> parse_model(const uint8_t* data, size_t size);

} // namespace scr
