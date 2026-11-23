#pragma once
// Screamer's .DTR text records, as parsed by data_load_section in STARTL.EXE.
//
// A .DTR file (stored inside a .DAT archive) has named sections:
//
//   CARS_DATA
//   *  filename  animation          <- '*' starts a comment that runs to the end of the line
//       sAUTO00  x0
//       f21000   f9000  f1.05 ...
//   END_LIST
//
// Each value is one whitespace-separated token whose first character is its type:
//   s = string, stored as char[12]    d = decimal int    x = hex int    f = float
// Tokens with any other first character are ignored. The game packs the values
// one after another into fixed-size records (record_bytes) placed `stride` bytes
// apart; pack_section reproduces that byte layout exactly.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scr {

struct DtrValue {
    char type = 'd';  // 's', 'd', 'x' or 'f'
    std::string text; // 's': the string (without the type letter)
    int32_t i = 0;    // 'd' and 'x'
    float f = 0.0f;   // 'f'
    size_t byte_size() const { return type == 's' ? 12 : 4; }
};

class DtrFile {
public:
    explicit DtrFile(std::string_view text) : text_(text) {}

    // All values of a section in file order, or nullopt if the section is missing.
    std::optional<std::vector<DtrValue>> section(std::string_view name) const;

    // Values of a section packed like data_load_section(dest, stride, record_bytes):
    // returns the bytes of `count` records spaced `stride` apart (stride >= record_bytes).
    std::optional<std::vector<uint8_t>> pack_section(std::string_view name, size_t stride, size_t record_bytes,
                                                     size_t* count = nullptr) const;

private:
    std::string text_;
};

} // namespace scr
