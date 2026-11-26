#pragma once
// Smaller versions of a track's textures (ours; the original draws one texel per pixel
// whatever the distance, which breaks thin details such as the road's painted lines into
// dashes where the road is seen at a shallow angle).
//
// Every texture is halved along u and along v separately, down to eight texels: level
// (lu, lv) averages 2^lu x 2^lv texels of the full texture. A road in the distance is
// squeezed lengthwise much more than across, and takes a level that is shortened but as
// wide as ever, so the lines stay sharp. The averages are taken in linear light and matched
// to the nearest colour of the track's palette. Texel 0 of the materials with holes
// (types 3-7) stays a hole where at least half of what a texel covers is one.
// Materials whose texels are not colours (translucent, types 14/15) have no smaller versions,
// nor have textures less than eight texels wide or high (the painted lines).

#include "core/framebuffer.h"
#include "core/render3d_fill.h"

#include <cstdint>
#include <vector>

namespace scr {

class MaterialLibrary;

struct MipSet {
    std::vector<uint8_t> data;
    std::vector<detail::MipTexture> textures;
    // per material of the library: its first texture (one per frame) or -1, and how many
    std::vector<int32_t> first, frames;

    const detail::MipTexture* find(size_t material, int frame) const {
        if (material >= first.size() || first[material] < 0) return nullptr;
        return &textures[size_t(first[material]) + size_t(frame) % size_t(frames[material])];
    }
};

MipSet build_mipmaps(const MaterialLibrary& lib, const Palette& palette);

} // namespace scr
