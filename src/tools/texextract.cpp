#include "../debug_report.h"
// texextract: writes every texture of the tracks (the materials of GFX/MAPn?.DAT and their sky
// panoramas) as full-colour PNGs, named so that the hardware renderer takes edited copies as
// replacements (core/texture_pack.h). Each picture is written once, in the first place it is
// found, in this order: the cars, the other objects (smoke, shadows), each track's road and
// scenery, its sky, then textures nothing on the track uses.
//
//   OUT/cars/AUTO00/MAP1_m123_<hash>.png        (animated: one file per frame, _f0 .. _f7)
//   OUT/objects/SMOKE/...
//   OUT/tracks/MAP1/road/..., scenery/..., sky/..., unused/...  (MAPnL.DAT: tracks/MAP1/low/...)
//   OUT/textures.csv                            every texture, where it is used, its type
//
// Usage: texextract [--data DIR] [--out DIR] [--force]
// OUT defaults to textures_original beside the program: the game reads replacements from
// textures (main.cpp, --textures), so only the files copied there and edited count. Files
// already in OUT are kept (so edits are not lost) unless --force is given.

#include "core/archive.h"
#include "core/gamedata.h"
#include "core/image.h"
#include "core/model3d.h"
#include "core/pcx.h"
#include "core/texture_pack.h"
#include "core/track.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Use {
    std::string folder;  // under OUT
    std::string label;   // file name before the hash
    bool by_texels = false;  // named by texture_index_hash (the cars and objects: one file for every track)
};

struct Written {
    std::string file;  // first frame's file, relative to OUT
    std::set<std::string> used_by;
};

bool write_png(const scr::RgbaImage& img, const fs::path& path) {
    scr::SurfacePtr s(SDL_CreateSurface(img.width, img.height, SDL_PIXELFORMAT_RGBA32));
    if (!s) return false;
    for (int y = 0; y < img.height; ++y)
        std::memcpy(static_cast<uint8_t*>(s->pixels) + size_t(y) * size_t(s->pitch), img.pixels.data() + size_t(y) * size_t(img.width) * 4,
                    size_t(img.width) * 4);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return scr::save_image(s.get(), path.string());
}

const char* type_name(const scr::Material& m) {
    if (scr::texture_has_holes(m)) return "transparent";
    return "textured";
}

} // namespace

int main(int argc, char** argv) {
    install_debug_report(true);
    std::string data, out;
    bool force = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) data = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--force") force = true;
        else {
            std::fprintf(stderr, "usage: texextract [--data DIR] [--out DIR] [--force]\n");
            return 2;
        }
    }
    if (out.empty()) {
        const char* base = SDL_GetBasePath();
        out = std::string(base ? base : "") + "textures_original";
    }
    const auto game = scr::find_game_dir(data);
    if (!game) {
        std::fprintf(stderr, "texextract: game data not found (use --data DIR)\n");
        return 1;
    }
    const fs::path root = fs::path(out);
    std::map<uint64_t, Written> done;
    size_t files = 0, kept = 0, failed = 0;
    std::vector<std::string> csv{"file,hash,frames,width,height,type,used_by"};

    for (int n = 1; n <= 6; ++n)
        for (const char detail : {'H', 'L'}) {
            const std::string name = "MAP" + std::to_string(n);
            const std::string file = name + detail + ".DAT";
            const auto path = scr::resolve_game_file(*game, "GFX/" + file);
            if (!path) continue;
            auto track = scr::load_track(*path, name);
            auto archive = scr::Archive::open(*path);
            if (!track || !track->materials || !archive) {
                std::fprintf(stderr, "texextract: cannot read %s\n", file.c_str());
                continue;
            }
            const scr::MaterialLibrary& lib = *track->materials;
            const auto& mats = lib.materials();
            const std::string track_dir = "tracks/" + name + (detail == 'L' ? "/low" : "");
            // where each material is used first: the cars, other objects, road, scenery
            std::vector<Use> use(mats.size());
            std::vector<std::set<std::string>> users(mats.size());
            const auto mark = [&](const scr::Model& m, const std::string& folder, const std::string& who, bool by_texels = false) {
                for (const scr::Part& part : m.parts)
                    for (const scr::Face& f : part.faces) {
                        const scr::Material* mat = lib.find(f.material);
                        if (!mat) continue;
                        const size_t mi = size_t(mat - mats.data());
                        if (use[mi].folder.empty()) use[mi] = {folder, (by_texels ? "" : name + "_") + "m" + std::to_string(mi), by_texels};
                        users[mi].insert(who);
                    }
            };
            std::vector<std::string> objects;
            for (const auto& e : archive->entries()) {
                const std::string& en = e.name;
                if (en.size() > 4 && SDL_strcasecmp(en.c_str() + en.size() - 4, ".3DX") == 0) objects.push_back(en);
            }
            std::sort(objects.begin(), objects.end(), [](const std::string& a, const std::string& b) {
                const bool ca = SDL_strncasecmp(a.c_str(), "AUTO", 4) == 0, cb = SDL_strncasecmp(b.c_str(), "AUTO", 4) == 0;
                return ca != cb ? ca : a < b;
            });
            for (const std::string& o : objects) {
                auto m = scr::load_object(*path, o);
                if (!m) continue;
                const std::string stem = o.substr(0, o.size() - 4);
                const bool car = SDL_strncasecmp(o.c_str(), "AUTO", 4) == 0;
                mark(*m, (car ? "cars/" : "objects/") + stem, stem, true);
            }
            for (int slot : {0, 1})
                for (const scr::Placement& p : track->placements)
                    if (p.slot == slot && p.object >= 0 && size_t(p.object) < track->objects.size())
                        mark(track->objects[size_t(p.object)], track_dir + (slot == 0 ? "/road" : "/scenery"), name + (slot == 0 ? " road" : " scenery"));
            // the track's objects no placement uses (the game places some itself: animations, obstacles)
            for (const scr::Model& m : track->objects) mark(m, track_dir + "/other", name + " other");
            for (size_t mi = 0; mi < mats.size(); ++mi)
                if (use[mi].folder.empty()) use[mi] = {track_dir + "/unused", name + "_m" + std::to_string(mi)};

            for (size_t mi = 0; mi < mats.size(); ++mi) {
                const scr::Material& m = mats[mi];
                if (!scr::texture_replaceable(m)) continue;
                const uint64_t h = use[mi].by_texels ? scr::texture_index_hash(lib, m) : scr::texture_hash(lib, m, track->palette);
                const auto it = done.find(h);
                if (it != done.end()) {
                    it->second.used_by.insert(users[mi].begin(), users[mi].end());
                    continue;
                }
                Written w;
                w.used_by = users[mi];
                const int frames = scr::texture_frames(m);
                for (int f = 0; f < frames; ++f) {
                    const std::string fname = use[mi].label + "_" + scr::hash_name(h) + (frames > 1 ? "_f" + std::to_string(f) : "") + ".png";
                    const fs::path rel = fs::path(use[mi].folder) / fs::path(fname);
                    if (f == 0) w.file = rel.generic_string();
                    const fs::path full = root / rel;
                    std::error_code ec;
                    if (!force && fs::exists(full, ec)) {
                        ++kept;
                        continue;
                    }
                    if (write_png(scr::material_rgba(lib, m, f, track->palette), full)) ++files;
                    else ++failed;
                }
                char row[512];
                std::snprintf(row, sizeof row, "%s,%s,%d,%d,%d,%s,", w.file.c_str(), scr::hash_name(h).c_str(), frames, m.width, m.height, type_name(m));
                csv.push_back(row);
                done.emplace(h, std::move(w));
            }
            if (track->background) {
                const uint64_t h = scr::texture_hash(*track->background, track->palette);
                if (!done.count(h)) {
                    const fs::path rel = fs::path(track_dir + "/sky") / fs::path(name + "_sky_" + scr::hash_name(h) + ".png");
                    const fs::path full = root / rel;
                    std::error_code ec;
                    if (!force && fs::exists(full, ec)) ++kept;
                    else if (write_png(scr::panorama_rgba(*track->background, track->palette), full)) ++files;
                    else ++failed;
                    char row[512];
                    std::snprintf(row, sizeof row, "%s,%s,1,%d,%d,sky,", rel.generic_string().c_str(), scr::hash_name(h).c_str(),
                                  track->background->width, track->background->height);
                    csv.push_back(row);
                    Written w;
                    w.file = rel.generic_string();
                    w.used_by.insert(name + " sky");
                    done.emplace(h, std::move(w));
                }
            }
            std::printf("%s: %zu materials\n", file.c_str(), mats.size());
        }

    // the CSV, with every user of each texture
    {
        std::error_code ec;
        fs::create_directories(root, ec);
        std::ofstream f(root / "textures.csv", std::ios::binary);
        f << csv[0] << "\r\n";
        for (size_t i = 1; i < csv.size(); ++i) {
            std::string row = csv[i];
            const std::string hash = row.substr(row.find(',') + 1, 12);
            uint64_t h = 0;
            for (char c : hash) h = (h << 4) | uint64_t(c <= '9' ? c - '0' : c - 'a' + 10);
            std::string who;
            for (const std::string& u : done[h].used_by) who += (who.empty() ? "" : " ") + u;
            f << row << '"' << who << '"' << "\r\n";
        }
    }
    std::printf("%zu textures: %zu files written, %zu kept, %zu failed, in %s\n", done.size(), files, kept, failed, root.string().c_str());
    return failed ? 1 : 0;
}
