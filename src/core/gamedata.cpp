#include "core/gamedata.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace scr {
namespace {

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::optional<fs::path> find_child_ci(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return std::nullopt;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (iequals(e.path().filename().string(), name)) return e.path();
    }
    return std::nullopt;
}

} // namespace

bool looks_like_game_dir(const std::string& dir) {
    if (dir.empty()) return false;
    return find_child_ci(dir, "GFX").has_value() && find_child_ci(dir, "DATA").has_value();
}

std::optional<std::string> find_game_dir(const std::string& explicit_path) {
    std::vector<std::string> candidates;
    if (!explicit_path.empty()) candidates.push_back(explicit_path);
    if (const char* env = SDL_getenv("SCREAMER_DATA"); env && *env) candidates.emplace_back(env);
    candidates.emplace_back(SCREAMER_DEFAULT_DATA_DIR);
    if (const char* base = SDL_GetBasePath()) candidates.push_back(std::string(base) + "SCREAMER");

    for (const auto& c : candidates) {
        if (looks_like_game_dir(c)) return fs::path(c).lexically_normal().string();
    }
    return std::nullopt;
}

std::optional<std::string> resolve_game_file(const std::string& dir, const std::string& relative) {
    fs::path cur(dir);
    for (const auto& part : fs::path(relative)) {
        const std::string s = part.string();
        if (s.empty() || s == "/" || s == "\\") continue;
        auto next = find_child_ci(cur, s);
        if (!next) return std::nullopt;
        cur = *next;
    }
    std::error_code ec;
    if (!fs::is_regular_file(cur, ec)) return std::nullopt;
    return cur.string();
}

std::optional<std::vector<uint8_t>> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return std::nullopt;
    const std::streamsize size = f.tellg();
    if (size < 0) return std::nullopt;
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.seekg(0);
    if (size > 0 && !f.read(reinterpret_cast<char*>(data.data()), size)) return std::nullopt;
    return data;
}

} // namespace scr
