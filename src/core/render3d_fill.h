#pragma once
// The renderer's pixel filler: what Renderer::finish() hands to the fill threads. The filler
// itself (render3d_fill.inl) is built twice, for SSE2 (any x64 CPU) and for AVX2, and both
// give exactly the same picture: every pixel goes through the same single-precision
// operations in the same order, only more of them at a time.
//
// This header and the .inl use nothing from the standard library that would be compiled
// into the AVX2 object file (an inline function emitted there could be the copy the linker
// keeps for the whole program).

#include <cstddef>
#include <cstdint>

namespace scr {

struct Material;
class MaterialLibrary;

namespace detail {

enum class FillMode : int { Textured, Transparent, Flat, Darken, Translucent };

// Smaller versions of a texture for where it is drawn squeezed (core/mipmaps.h), halved
// along u and along v separately: level (lu, lv) is size >> lu wide and size >> lv high
// (rounded, at least 1), its rows one after the other.
constexpr int kMipLevels = 8;
struct MipAxis {
    int32_t size;         // texels at this level
    int32_t mask;         // size - 1 for a power of two, else -1
    int32_t bias, magic;  // see wrap_mod
    float scale;          // level texels per texel of the full texture
};
struct MipTexture {
    int32_t levels_u, levels_v;  // at least 1 (the full texture)
    MipAxis u[kMipLevels], v[kMipLevels];
    const uint8_t* texels[kMipLevels][kMipLevels];  // [lv][lu]; four bytes can be read at every texel
};

struct Scr {
    float x, y, iz, uz, vz;
};

// What a polygon's pixels are filled with.
struct Fill {
    const Material* mat;
    const uint8_t* texels;  // the frame's pixels when every texel lies inside the library, else null
    int mode;               // FillMode
    int frame;
    int wmask, hmask;       // size - 1 for power-of-two sizes, else -1
    int width, height;      // the material's size in texels (when texels is set)
    // for sizes that are not powers of two (see wrap_mod): a multiple of the size from 16384 up,
    // and 65536 / size
    int ubias, vbias, umagic, vmagic;
    uint8_t flat;
    bool use_texels, writes_depth;
    bool wide_reads;        // four bytes can be read at every texel (AVX2 gathers)
    // The polygon's corners all lie inside the texture (it shows a piece of it, not repeats):
    // texel coordinates just outside, at its edges, stay on the edge texel and do not
    // come round from the far side of the texture.
    bool clamp_uv;
    // The pixels past the polygon's own edges (edge_slack) are tested and written as a little
    // further away: they fill cracks, but any surface that really is there shows instead.
    bool seal_behind;
    const MipTexture* mip;  // the smaller versions to draw from where the texture is squeezed, or null
    float depth_bias;       // in units of d = 128 - z
    float edge_slack;       // pixels the polygon reaches past its edges
};

struct Tri {
    Scr a, b, c;
    int y0, y1, x0, x1;
    uint32_t fill;
    uint8_t sealed;  // bit 0: the edge b-c is moved out by the fill's edge_slack, 1: c-a, 2: a-b
};

// A triangle to fill and what with.
struct TriRef {
    const Tri* tri;
    const Fill* fill;
};

struct FillContext {
    uint8_t* pixels;        // width * height palette indices
    float* depth;           // 1/z per pixel
    int width, height;
    const MaterialLibrary* lib;
    const uint8_t* shade;   // 64 x 256 translucency table
    const uint8_t* darken;  // 256
    bool fade;              // the view-distance fade is on
    float fade_iz;          // 1 / (128 + fade_depth): pixels with a smaller 1/z are in the fade
    float fade_start, fade_range;
    float mip_sharpness;    // a level is used from 1 / sharpness of its texels per pixel on
};

// Fills rows band0 .. band1 - 1 with the listed triangles, in order.
using FillRowsFn = void (*)(const FillContext& ctx, int band0, int band1, const TriRef* tris, size_t count);
void fill_rows_sse2(const FillContext& ctx, int band0, int band1, const TriRef* tris, size_t count);
void fill_rows_avx2(const FillContext& ctx, int band0, int band1, const TriRef* tris, size_t count);

// u and v wrapped to the texture as the fillers do it, for the tests: out = in mod size
// (0 .. size - 1), for size 1 .. 32767 and any in (the vector way for in of -16384 .. 16383).
void wrap_coords_sse2(const int32_t* in, int32_t* out, size_t count, int size);
void wrap_coords_avx2(const int32_t* in, int32_t* out, size_t count, int size);

// Helpers the fillers call (defined in render3d.cpp, built for any CPU).
uint8_t library_texel(const MaterialLibrary* lib, const Material* mat, int u, int v, int frame);
// How far (in barycentric weight) the three edges of a triangle are moved out for `slack` pixels.
void tri_edge_slack(const Tri& t, float slack, float area, float out[3]);

} // namespace detail
} // namespace scr
