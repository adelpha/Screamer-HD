#include "testing.h"

#include "core/archive.h"
#include "core/gamedata.h"

#include <cstring>
#include <string>

namespace {

// Minimal encoder for the game's LZW variant (LSB-first codes, 9..12 bits,
// 0x100 clear, 0x101 end), used only to test the decoder without game data.
std::vector<uint8_t> lzw_encode(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    uint64_t acc = 0;
    int nbits = 0, width = 9;
    uint32_t next = 0x102, limit = 0x200;
    auto put = [&](uint32_t code) {
        acc |= uint64_t(code) << nbits;
        nbits += width;
        while (nbits >= 8) {
            out.push_back(uint8_t(acc));
            acc >>= 8;
            nbits -= 8;
        }
    };
    std::vector<std::vector<uint8_t>> dict;
    auto find = [&](const std::vector<uint8_t>& s) -> int {
        if (s.size() == 1) return s[0];
        for (size_t i = 0; i < dict.size(); ++i)
            if (dict[i] == s) return int(0x102 + i);
        return -1;
    };
    put(0x100);
    std::vector<uint8_t> w;
    for (uint8_t c : in) {
        auto wc = w;
        wc.push_back(c);
        if (find(wc) >= 0) {
            w = wc;
            continue;
        }
        put(uint32_t(find(w)));
        if (next < 4096) {
            dict.push_back(wc);
            ++next;
            if (next > limit && width < 12) {  // the decoder adds its entries one code later
                ++width;
                limit <<= 1;
            }
        }
        w = {c};
    }
    if (!w.empty()) put(uint32_t(find(w)));
    put(0x101);
    if (nbits) out.push_back(uint8_t(acc));
    return out;
}

void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = uint8_t(x >> (8 * i));
}

} // namespace

TEST_CASE("archive: LZW round trip, including the KwKwK case and code growth") {
    std::vector<uint8_t> data;
    for (int i = 0; i < 3000; ++i) data.push_back(uint8_t((i * 7) % 13 + (i / 100)));
    for (int i = 0; i < 200; ++i) data.push_back('a');
    auto packed = lzw_encode(data);
    auto out = scr::lzw_decode(packed.data(), packed.size(), data.size());
    REQUIRE(out.has_value());
    CHECK(*out == data);
}

TEST_CASE("archive: directory parsing, XOR names, case-insensitive lookup") {
    const std::string text = "CARS_DATA\r\n\tsAUTO00\tx0\r\nEND_LIST\r\n";
    std::vector<uint8_t> raw(text.begin(), text.end());
    auto packed = lzw_encode(raw);

    std::vector<uint8_t> file = {'p', 'l', 'a', 'i', 'n'};
    const uint32_t plain_off = 0, packed_off = uint32_t(file.size());
    file.insert(file.end(), packed.begin(), packed.end());
    const size_t dir = file.size();
    file.resize(dir + 3 * 0x20 + 4, 0);
    auto entry = [&](size_t at, const char* name, uint32_t off, uint32_t psize, uint32_t size, uint32_t flags) {
        for (size_t i = 0; i <= std::strlen(name); ++i) file[at + i] = uint8_t(name[i] ^ 0x3A);
        put32(file, at + 16, off);
        put32(file, at + 20, psize);
        put32(file, at + 24, size);
        put32(file, at + 28, flags);
    };
    entry(dir, "PLAIN.TXT", plain_off, 5, 5, 0);
    entry(dir + 0x20, "CARS.DTR", packed_off, uint32_t(packed.size()), uint32_t(raw.size()), 3);
    entry(dir + 0x40, "", 0, 0, 0, 0);
    put32(file, file.size() - 4, 3 * 0x20);

    auto a = scr::Archive::from_bytes(file);
    REQUIRE(a.has_value());
    CHECK(a->entries().size() == 2);
    auto plain = a->read("plain.txt");
    REQUIRE(plain.has_value());
    CHECK(std::string(plain->begin(), plain->end()) == "plain");
    auto dtr = a->read("cars.dtr");
    REQUIRE(dtr.has_value());
    CHECK(*dtr == raw);
    CHECK(!a->read("missing").has_value());
}

TEST_CASE("archive: every entry of the original archives decodes to its stated size") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    int archives = 0, entries = 0;
    for (const char* rel : {"DATA/GEN.DAT", "DATA/LOCAL.DAT", "DATA/MAP1.DAT", "DATA/SOUND.DAT", "DATA/MUSIC.DAT",
                            "GFX/GRAF.DAT", "GFX/MENU.DAT", "GFX/GAME.DAT", "GFX/MAP1L.DAT"}) {
        auto path = scr::resolve_game_file(*dir, rel);
        REQUIRE(path.has_value());
        auto a = scr::Archive::open(*path);
        REQUIRE(a.has_value());
        ++archives;
        for (const auto& e : a->entries()) {
            auto data = a->read(e);
            CHECK(data.has_value());
            if (data) CHECK(data->size() == e.size);
            ++entries;
        }
    }
    CHECK(archives == 9);
    CHECK(entries > 50);
}
