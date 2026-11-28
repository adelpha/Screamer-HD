#pragma once
// Replacement textures (ours): full-colour PNGs that the hardware renderer draws in place of
// the tracks' palette textures and sky panoramas.
//
// A texture is known by a hash of what it looks like: its size, frame count and the colours of
// every texel of every frame in the track's palette (with the holes of the transparent types).
// The same picture on two tracks (the cars, mostly) is one texture; the same texels in another
// palette are another. A replacement is any PNG whose name ends in _<hash>.png (12 hex digits),
// or _<hash>_f<N>.png for frame N of an animated one, anywhere under the texture folder; the
// rest of the name and the folders are free (texextract names and sorts them: cars, tracks,
// scenery, sky). A replacement may be any whole multiple of the original's size (other sizes
// are resampled); its alpha marks the holes of the transparent material types. Translucent and
// darkening materials (types 12-15) go through the palette tables and are not replaced.

#include "core/framebuffer.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace scr {

struct Material;
class MaterialLibrary;
struct PcxImage;

struct RgbaImage {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;  // width * height * 4, RGBA, top row first
};

// The material has texels a replacement can stand in for (not flat, translucent or darkening).
bool texture_replaceable(const Material& m);
// The material's texels have holes (types 3-7: texel 0 is not drawn).
bool texture_has_holes(const Material& m);
int texture_frames(const Material& m);

// Frame f of the material in the palette: alpha 0 for holes, else 255.
RgbaImage material_rgba(const MaterialLibrary& lib, const Material& m, int frame, const Palette& palette);
// The panorama in the palette (opaque).
RgbaImage panorama_rgba(const PcxImage& pano, const Palette& palette);

// What the material (every frame) or panorama looks like, 48 bits.
// texture_index_hash: the material's texels whatever the palette (the cars and objects the
// game loads onto every track, in each track's palette): a replacement under that hash serves
// every track; one under the colour hash only the tracks where it looks just so, and wins.
uint64_t texture_hash(const MaterialLibrary& lib, const Material& m, const Palette& palette);
uint64_t texture_hash(const PcxImage& pano, const Palette& palette);
uint64_t texture_index_hash(const MaterialLibrary& lib, const Material& m);
std::string hash_name(uint64_t hash);  // 12 hex digits

class TexturePack {
public:
    // Every replacement under dir (recursively). Empty if the folder is missing.
    static TexturePack scan(const std::string& dir);
    bool empty() const { return files_.empty(); }
    size_t size() const { return files_.size(); }
    const std::string& dir() const { return dir_; }
    // A replacement for the texture's frame (a file for that frame, else one for them all).
    bool has(uint64_t hash) const { return files_.count(hash) != 0; }
    // The material's replacement: by its colours, else by its texels (0: none).
    uint64_t find(uint64_t colour_hash, uint64_t index_hash) const { return has(colour_hash) ? colour_hash : has(index_hash) ? index_hash : 0; }
    std::optional<RgbaImage> load(uint64_t hash, int frame = 0) const;
    // The file a replacement would come from (empty if none).
    std::string path(uint64_t hash, int frame = 0) const;

private:
    std::string dir_;
    // per hash: the file for every frame (key -1: for all frames)
    std::map<uint64_t, std::map<int, std::string>> files_;
};

// The image resampled to w x h: whole multiples of its size larger by repeating texels, smaller
// by box filtering, else bilinear.
RgbaImage resample(const RgbaImage& img, int w, int h);

// Parses "name_<hash>.png" / "name_<hash>_f<N>.png": the hash and the frame (-1: none).
bool parse_texture_name(const std::string& file_name, uint64_t& hash, int& frame);

} // namespace scr
