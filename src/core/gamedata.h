#pragma once
// Locating and reading the user's original game files. These are never
// shipped with the project: the user points us at their own install.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scr {

// Resolution order: explicit path (e.g. --data), $SCREAMER_DATA, compiled-in
// default (CMake SCREAMER_DATA_DIR; none in release packages), a SCREAMER folder next
// to the program, the program's own folder. Returns the first directory that looks
// like a Screamer install (contains GFX/ and DATA/), or nullopt.
std::optional<std::string> find_game_dir(const std::string& explicit_path = {});

// True if dir contains the core subfolders of a Screamer install.
bool looks_like_game_dir(const std::string& dir);

// Case-insensitive lookup of a relative path such as "GFX/MENU.DAT" under dir.
// Returns the real on-disk path, or nullopt if absent.
std::optional<std::string> resolve_game_file(const std::string& dir, const std::string& relative);

// Reads a whole file. Returns nullopt on failure.
std::optional<std::vector<uint8_t>> read_file(const std::string& path);

} // namespace scr
