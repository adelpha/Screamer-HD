#include "testing.h"

#include "core/archive.h"
#include "core/fm_synth.h"
#include "core/gamedata.h"

#include <cmath>
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

TEST_CASE("fm synth: voices run out and get reused") {
    scr::FmMidiSynth synth({sine_patch()}, {});
    for (int n = 40; n < 70; ++n) synth.message(0x90, uint8_t(n), 100);
    CHECK(synth.active_voices() == 18);
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
