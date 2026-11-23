#pragma once
// Screamer's .DAT archives (data\*.dat, gfx\*.dat).
//
// Format, from archive_find_entry / archive_read / lzw_decode in STARTL.EXE:
//  * the last dword of the file is the size of a directory stored just before it;
//  * the directory is a list of 0x20-byte entries
//      char name[16]  (each byte XOR 0x3A, up to and including the terminating NUL)
//      int32 offset   (from the start of the file)
//      int32 packed   (bytes stored in the file)
//      int32 size     (bytes after decoding)
//      int32 flags    (bit 1: LZW compressed; bit 0: the game keeps the entry in memory)
//    ending with an entry whose name decodes to "";
//  * compressed entries use LZW with LSB-first codes of 9..12 bits, 0x100 = clear,
//    0x101 = end, first free code 0x102.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scr {

struct ArchiveEntry {
    std::string name;
    uint32_t offset = 0;
    uint32_t packed_size = 0;
    uint32_t size = 0;
    uint32_t flags = 0;
    bool compressed() const { return (flags & 2) != 0; }
};

class Archive {
public:
    // Parses an archive held in memory. Returns nullopt if the directory is malformed.
    static std::optional<Archive> from_bytes(std::vector<uint8_t> bytes);
    // Reads and parses an archive file.
    static std::optional<Archive> open(const std::string& path);

    const std::vector<ArchiveEntry>& entries() const { return entries_; }
    // Case-insensitive lookup, as the game does (stricmp).
    const ArchiveEntry* find(std::string_view name) const;
    // Returns the decoded contents of an entry, or nullopt if absent or corrupt.
    std::optional<std::vector<uint8_t>> read(std::string_view name) const;
    std::optional<std::vector<uint8_t>> read(const ArchiveEntry& entry) const;

private:
    std::vector<uint8_t> bytes_;
    std::vector<ArchiveEntry> entries_;
};

// Decodes Screamer's LZW stream. Stops at the end code, at out_size bytes or at
// the end of the input. Returns nullopt on an invalid code.
std::optional<std::vector<uint8_t>> lzw_decode(const uint8_t* src, size_t src_size, size_t out_size);

} // namespace scr
