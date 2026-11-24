#include "testing.h"

#include "core/archive.h"
#include "core/fm_synth.h"
#include "core/gamedata.h"

#include <array>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace {

// A plain sine: the modulator silent (level 63, no feedback), the carrier at full level.
scr::FmPatch sine_patch() {
    scr::FmPatch p;
    p.op[0].tl = 63;
    p.op[0].ar = 15;
    p.op[1].ar = 15;
    p.op[1].sl = 0;
    p.op[1].rr = 8;
    return p;
}

int zero_crossings(const std::vector<float>& stereo) {
    int n = 0;
    for (size_t i = 2; i < stereo.size(); i += 2)
        if ((stereo[i - 2] < 0.0f) != (stereo[i] < 0.0f)) ++n;
    return n;
}

} // namespace

TEST_CASE("fm synth: A4 is 440 Hz and note off releases") {
    scr::FmMidiSynth synth({sine_patch()}, {});
    synth.message(0x90, 69, 127);
    CHECK(synth.active_voices() == 1);
    std::vector<float> buf(44100 * 2, 0.0f);
    synth.render(buf.data(), 44100, 44100, 1.0f);
    const int zc = zero_crossings(buf);
    CHECK(zc > 870);
    CHECK(zc < 890);
    synth.message(0x80, 69, 0);
    std::fill(buf.begin(), buf.end(), 0.0f);
    synth.render(buf.data(), 44100, 44100, 1.0f);
    CHECK(synth.active_voices() == 0);
    // Pitch bend: +2 semitones at full bend.
    synth.message(0xe0, 0x7f, 0x7f);
    synth.message(0x90, 69, 127);
    std::fill(buf.begin(), buf.end(), 0.0f);
    synth.render(buf.data(), 44100, 44100, 1.0f);
    const int bent = zero_crossings(buf);
    CHECK(bent > 975);  // 493.9 Hz
    CHECK(bent < 995);
}

TEST_CASE("fm synth: HMI's nine voices") {
    scr::FmMidiSynth synth({sine_patch()}, {});
    for (int n = 40; n < 70; ++n) synth.message(0x90, uint8_t(n), 100);
    CHECK(synth.active_voices() == 9);
    scr::FmMidiSynth enhanced({sine_patch()}, {});
    enhanced.set_enhanced(true);
    for (int n = 40; n < 70; ++n) enhanced.message(0x90, uint8_t(n), 100);
    CHECK(enhanced.active_voices() == 18);
}

namespace {

// Peak of each channel over a second of one held note.
std::array<float, 2> peaks(std::initializer_list<std::array<uint8_t, 3>> msgs) {
    scr::FmMidiSynth synth({sine_patch()}, {});
    for (const auto& m : msgs) synth.message(m[0], m[1], m[2]);
    std::vector<float> buf(44100 * 2, 0.0f);
    synth.render(buf.data(), 44100, 44100, 1.0f);
    std::array<float, 2> p{};
    for (size_t i = 22050 * 2; i < buf.size(); ++i) p[i & 1] = std::max(p[i & 1], std::fabs(buf[i]));
    return p;
}

double db(float a, float b) { return 20.0 * std::log10(double(a) / double(b)); }

} // namespace

TEST_CASE("fm synth: HMI's velocity, volume and pan curves") {
    // Velocity 64: v = 64, attenuation table[32] = 13 steps -> level 13 = 9.75 dB down.
    const auto full = peaks({{0x90, 69, 127}});
    const auto half = peaks({{0x90, 69, 64}});
    CHECK(std::fabs(db(half[0], full[0]) + 9.75) < 0.6);
    // Channel volume 32: v = (32 << 7) / 127 * 127 >> 7 = 32 -> table[16] = 27 steps (20.25 dB);
    // the pan pass recomputes one side as volume * velocity >> 7 = 31 -> table[15] = 28 steps.
    const auto vol = peaks({{0xb0, 7, 32}, {0x90, 69, 127}});
    CHECK(std::fabs(db(vol[0], full[0]) + 21.0) < 0.6);
    CHECK(std::fabs(db(vol[1], full[1]) + 20.25) < 0.6);
    // Hard pan: HMI turns down the copy in bank 1 for pan < 64; Nuked's first output is
    // that copy's (output A), so MIDI pan 0 silences the first channel.
    const auto hard = peaks({{0xb0, 10, 0}, {0x90, 69, 127}});
    CHECK(hard[0] < full[0] * 0.01f);
    // ...while the centre position (64) turns the other copy down by one step (0.75 dB).
    CHECK(std::fabs(db(hard[1], full[1]) - 0.75) < 0.2);
}

TEST_CASE("fm synth: a note retriggered in the same tick starts again") {
    // A decaying patch: after a second it is quiet; off + on at the same moment must restart it
    // (on the card the driver's port delays put samples between the two writes).
    scr::FmPatch p = sine_patch();
    p.op[1].sustain = false;
    p.op[1].dr = 5;
    scr::FmMidiSynth synth({p}, {});
    std::vector<float> buf(44100 * 2, 0.0f);
    synth.message(0x90, 69, 127);
    synth.render(buf.data(), 2205, 44100, 1.0f);
    float start = 0;
    for (size_t i = 0; i < 2205 * 2; ++i) start = std::max(start, std::fabs(buf[i]));
    std::fill(buf.begin(), buf.end(), 0.0f);
    synth.render(buf.data(), 44100, 44100, 1.0f);
    float late = 0;
    for (size_t i = 40000 * 2; i < 44100 * 2; ++i) late = std::max(late, std::fabs(buf[i]));
    CHECK(late < start * 0.5f);
    synth.message(0x80, 69, 0);
    synth.message(0x90, 69, 127);
    std::fill(buf.begin(), buf.end(), 0.0f);
    synth.render(buf.data(), 2205, 44100, 1.0f);
    float again = 0;
    for (size_t i = 0; i < 2205 * 2; ++i) again = std::max(again, std::fabs(buf[i]));
    CHECK(again > start * 0.8f);
}

TEST_CASE("fm synth: the game's banks and songs") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto music = scr::resolve_game_file(*dir, "DATA/MUSIC.DAT");
    REQUIRE(music.has_value());
    auto a = scr::Archive::open(*music);
    REQUIRE(a.has_value());
    auto mel = scr::parse_adlib_bank(*a->read("MELODIC.BNK"));
    auto drum = scr::parse_adlib_bank(*a->read("DRUM.BNK"));
    REQUIRE(mel.size() == 127);
    REQUIRE(drum.size() == 127);
    CHECK(mel[0].op[0].mult == 3);  // PIANO1
    CHECK(mel[0].op[1].rr == 5);
    CHECK(!mel[0].additive);
    CHECK(drum[36].note == 35);     // Kick, played at note 35
    auto song = scr::parse_hmp(*a->read("MUSIC1.HMP"));
    REQUIRE(song.has_value());
    CHECK(song->ticks_per_second == 120);
    CHECK(song->length == 11208);   // 93 s, as the header says
    CHECK(song->events.size() > 20000);
    for (size_t i = 1; i < song->events.size(); ++i) REQUIRE(song->events[i - 1].tick <= song->events[i].tick);
    // Every song plays and makes sound.
    for (int n = 1; n <= 7; ++n) {
        auto p = scr::load_hmp_music(*music, n, false);
        REQUIRE(p != nullptr);
        std::vector<float> buf(22050 * 2 * 10, 0.0f);
        p->mix(buf.data(), 22050 * 10, 22050, 1.0f);
        float peak = 0;
        for (float v : buf) peak = std::max(peak, std::fabs(v));
        CHECK(peak > 0.05f);
        CHECK(peak < 4.0f);
    }
    CHECK(scr::music_for_race(0) == 2);
}
