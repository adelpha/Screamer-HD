#include "testing.h"

#include "core/animations.h"
#include "core/obstacles.h"
#include "core/archive.h"
#include "core/cd_audio.h"
#include "core/dtr.h"
#include "core/gamedata.h"
#include "core/opponent.h"
#include "core/race_audio.h"
#include "core/sim_clock.h"
#include "core/sound.h"
#include "core/track.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

// A bank of synthetic samples: 0 a 100-byte ramp, 1 a looping 10-byte square wave,
// 2 the ramp with a loop segment over bytes 50..59.
scr::SoundBank test_bank() {
    scr::SoundBank b;
    scr::SoundFx ramp;
    ramp.name = "RAMP";
    for (int i = 0; i < 100; ++i) ramp.data.push_back(uint8_t(128 + i));
    scr::SoundFx loop;
    loop.name = "LOOP";
    loop.loop = -1;
    for (int i = 0; i < 10; ++i) loop.data.push_back(i < 5 ? 255 : 0);
    scr::SoundFx seg = ramp;
    seg.name = "SEG";
    seg.loop = 2;
    seg.loop_start = 50;
    seg.loop_leng = 10;
    b.fx = {ramp, loop, seg};
    b.num_base = 3;
    return b;
}

// Frames until the voice ends, mixing at the driver's own rate.
int frames_until_done(scr::Mixer& m, int handle, int limit = 10000) {
    std::vector<float> buf(2);
    for (int i = 0; i < limit; ++i) {
        if (!m.playing(handle)) return i;
        m.mix(buf.data(), 1, scr::kSampleRate);
    }
    return -1;
}

} // namespace

TEST_CASE("sound: SOUND_FX records") {
    const auto fx = scr::parse_sound_fx(
        "SOUND_FX\n*\tname\tvolume\tloop\tloop_start\tloop_leng\tpitch\tpan\n\n"
        "\tsskidfx\tx10004800\td200\td2943\td2970\tx00010000\tx01000000\n"
        "\tsoppmotor\tx10005000\td-1\td0\td0\tx10010000\tx11000000\nEND_LIST\n");
    REQUIRE(fx.size() == 2);
    CHECK(fx[0].name == "skidfx");
    CHECK(fx[0].volume == 0x10004800u);
    CHECK(fx[0].loop == 200);
    CHECK(fx[0].loop_start == 2943);
    CHECK(fx[0].loop_leng == 2970);
    CHECK(fx[1].name == "oppmotor");
    CHECK(fx[1].loop == -1);
    CHECK(fx[1].pitch == 0x10010000u);
    CHECK(fx[1].pan == 0x11000000u);
}

TEST_CASE("sound: the mixer plays samples at their pitch and loops them") {
    const scr::SoundBank bank = test_bank();
    scr::Mixer m;
    m.smooth = false;
    m.set_bank(&bank);
    // One sample per output frame at 11025 Hz, twice as fast at pitch 2.0.
    int h = m.play(0);
    REQUIRE(h >= 0);
    CHECK(m.sample_of(h) == 0);
    CHECK(frames_until_done(m, h) == 100);
    h = m.play(0);
    m.set_pitch(h, 0x20000);
    CHECK(frames_until_done(m, h) == 50);
    // The segment loop: 100 bytes, then bytes 50..59 twice more.
    h = m.play(2);
    CHECK(frames_until_done(m, h) == 120);
    // Forever: still going; a stale handle does nothing once stopped.
    h = m.play(1);
    CHECK(frames_until_done(m, h, 500) == -1);
    CHECK(m.stop(h) == -1);
    CHECK(!m.playing(h));
    const int h2 = m.play(1);
    m.set_volume(h, 0);  // the old handle must not touch the new voice in the same slot
    std::vector<float> buf(2);
    m.mix(buf.data(), 1, scr::kSampleRate);
    CHECK(buf[0] > 0.1f);
    // Pan: hard left silences the right channel.
    m.set_pan(h2, 0);
    m.mix(buf.data(), 1, scr::kSampleRate);
    CHECK(buf[1] == 0.0f);
    CHECK(buf[0] != 0.0f);
    m.stop_all();
    CHECK(m.active_voices() == 0);
    // sfx_play_continue: the loop hands over to the ramp when it wraps.
    h = m.play(1);
    m.continue_with(h, 0);
    for (int i = 0; i < 12; ++i) m.mix(buf.data(), 1, scr::kSampleRate);
    CHECK(m.sample_of(h) == 0);
    // Out of voices.
    m.stop_all();
    for (int i = 0; i < scr::Mixer::kVoices; ++i) CHECK(m.play(1) >= 0);
    CHECK(m.play(1) == -1);
}

TEST_CASE("sound: cue sheets of the CD image") {
    const auto dir = std::filesystem::temp_directory_path() / "scr_cd_test";
    std::filesystem::create_directories(dir);
    {
        std::ofstream cue(dir / "T.cue");
        cue << "FILE \"T.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"
               "  TRACK 02 AUDIO\n    INDEX 01 00:00:10\n";
        std::ofstream bin(dir / "T.bin", std::ios::binary);
        std::vector<char> data(2352 * 20, 0);
        // A full-scale left channel in track 2.
        for (size_t i = 2352 * 10; i + 4 <= data.size(); i += 4) {
            data[i] = static_cast<char>(-1);
            data[i + 1] = 0x7f;
        }
        bin.write(data.data(), std::streamsize(data.size()));
    }
    scr::CdImage cd;
    REQUIRE(cd.open((dir / "T.cue").string()));
    REQUIRE(cd.tracks.size() == 2);
    CHECK(!cd.tracks[0].audio);
    CHECK(cd.tracks[1].audio);
    CHECK(cd.tracks[1].offset == 2352u * 10);
    CHECK(cd.tracks[1].length == 2352u * 10);
    {
        scr::CdAudioPlayer p(cd, 2);
        REQUIRE(p.ok());
        std::vector<float> buf(200, 0.0f);
        p.mix(buf.data(), 100, 44100, 1.0f);
        CHECK(buf[10] > 0.99f);
        CHECK(buf[11] == 0.0f);
    }
    // The Steam/GOG names: SCREAM.DAT is the cue sheet and SCREAM.gog the image, which the cue
    // may still call by another name.
    {
        std::ofstream cue(dir / "SCREAM.DAT");
        cue << "FILE \"SCREAM.BIN\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"
               "  TRACK 02 AUDIO\n    INDEX 01 00:00:10\n";
    }
    std::filesystem::rename(dir / "T.bin", dir / "SCREAM.gog");
    std::filesystem::remove(dir / "T.cue");
    const auto found = scr::find_cd_cue(dir.string());
    REQUIRE(found.has_value());
    scr::CdImage gog;
    REQUIRE(gog.open(*found));
    CHECK(gog.tracks.size() == 2);
    CHECK(std::filesystem::path(gog.bin_path).filename() == "SCREAM.gog");
    CHECK(scr::cd_track_for_race(0) == 3);
    CHECK(scr::cd_track_for_race(1) == 5);
    CHECK(scr::cd_track_for_race(5) == 6);
    std::filesystem::remove_all(dir);
}

TEST_CASE("sound: the game's sample tables") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto gen = scr::resolve_game_file(*dir, "DATA/GEN.DAT");
    auto snd = scr::resolve_game_file(*dir, "DATA/SOUND.DAT");
    auto map = scr::resolve_game_file(*dir, "DATA/MAP1.DAT");
    REQUIRE(gen && snd && map);
    scr::SoundBank bank;
    REQUIRE(bank.load(*gen, *snd, *map, "MAP1"));
    CHECK(bank.num_base == 12);
    CHECK(bank.fx[3].name == "automoto");
    CHECK(bank.fx[12].name == "truck");     // MAP1DAT.DTR
    CHECK(bank.speech_first == 14);
    CHECK(bank.num_speech == 58);           // SPFXBASE 11 + SPFXFULL 47
    CHECK(bank.speech_full);
    CHECK(bank.fx[size_t(bank.speech_first + 2)].name == "three");
    for (const scr::SoundFx& f : bank.fx) CHECK(!f.data.empty());
}

TEST_CASE("sound: a race on MAP1 - engines, countdown and announcer") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto map = scr::resolve_game_file(*dir, "GFX/MAP1H.DAT");
    auto data = scr::resolve_game_file(*dir, "DATA/MAP1.DAT");
    auto gen = scr::resolve_game_file(*dir, "DATA/GEN.DAT");
    auto snd = scr::resolve_game_file(*dir, "DATA/SOUND.DAT");
    REQUIRE(map && data && gen && snd);
    auto track = scr::load_track(*map, "MAP1");
    auto model = scr::load_object(*map, "AUTO00.3DX");
    auto a = scr::Archive::open(*gen);
    auto text = a ? a->read("CARS.DTR") : std::nullopt;
    REQUIRE(track && model && text);
    auto cars = scr::DtrFile(std::string(text->begin(), text->end())).pack_section("CARS_DATA", 0x40, 0x40);
    REQUIRE(cars.has_value());
    const scr::CarSetup setup = scr::car_setup(cars->data(), *model);
    scr::Race race;
    REQUIRE(race.load(*data, "MAP1"));
    race.start(*track, setup, std::vector<scr::CarSetup>(9, setup));
    scr::SoundBank bank;
    REQUIRE(bank.load(*gen, *snd, *data, "MAP1"));
    scr::Mixer mixer;
    mixer.set_bank(&bank);
    scr::RaceAudio audio;
    audio.start(mixer, bank, race, 0, 0);
    CHECK(mixer.active_voices() == 1);  // the player's engine
    scr::SimClock clock;
    scr::CarInput in;
    in.accel = 1023;
    std::vector<float> buf(2 * 441);
    std::vector<int> heard;  // speech samples that started
    auto run = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            clock.add_ticks(2);
            clock.begin_frame();
            race.update(*track, in, clock.frame_time(), clock.pow_table_row());
            audio.update(race, clock.frame_time());
            if (audio.speech_handle() >= 0) {
                const int s = mixer.sample_of(audio.speech_handle());
                if (s >= 0 && (heard.empty() || heard.back() != s)) heard.push_back(s);
            }
            mixer.mix(buf.data(), 441, 22050);  // 1/50 s
        }
    };
    run(1);
    // "three" straight away, the engine, and three CPU engines (the nearest).
    CHECK(mixer.active_voices() == 1 + 1 + 3);
    run(400);  // past "go"
    CHECK(race.started);
    run(300);
    CHECK(mixer.active_voices() >= 4);
    for (float v : buf) CHECK(std::fabs(v) <= 1.0f);
}

TEST_CASE("animations: the moving scenery of MAP1 follows its splines") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto data = scr::resolve_game_file(*dir, "DATA/MAP1.DAT");
    REQUIRE(data.has_value());
    scr::TrackAnimations anims;
    REQUIRE(anims.load(*data, "MAP1"));
    REQUIRE(anims.list.size() == 5);
    CHECK(anims.list[0].name == "awtruck");
    CHECK(anims.list[4].name == "ajet");
    CHECK(anims.list[4].sound_fx == 1);  // "airplane", after the base table
    anims.start(0.02f);
    CHECK(anims.list[4].y == 9000);
    // Every frame each one moves about speed * frame time along its line (listener close to all).
    for (int f = 0; f < 500; ++f) {
        std::vector<std::array<int, 2>> before;
        for (const auto& a : anims.list) before.push_back({a.x, a.z});
        anims.update(anims.list[0].x, anims.list[0].z, 0.02f);
        const auto& a = anims.list[0];
        const double moved = std::hypot(double(a.x - before[0][0]), double(a.z - before[0][1]));
        CHECK(moved > 16000 * 0.02 * 0.7);
        CHECK(moved < 16000 * 0.02 * 2.0);  // a knot can add a jump, as in the original
        CHECK(a.yaw >= 0.0f);
        CHECK(a.yaw < 6.2832f);
    }
}

TEST_CASE("obstacles: MAP1's cones, barriers and slalom gates") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto data = scr::resolve_game_file(*dir, "DATA/MAP1.DAT");
    auto gfx = scr::resolve_game_file(*dir, "GFX/MAP1H.DAT");
    REQUIRE(data && gfx);
    auto track = scr::load_track(*gfx, "MAP1");
    REQUIRE(track.has_value());
    scr::Obstacles ost;
    REQUIRE(ost.load(*data, *gfx, "MAP1", "OST", *track));
    CHECK(ost.list.size() == 45);
    scr::Obstacles cng;
    REQUIRE(cng.load(*data, *gfx, "MAP1", "CNG", *track));
    CHECK(cng.list.size() == 36);
    CHECK(cng.list[0].x == 930);
    CHECK(cng.list[0].z == 44477);
    // A car driving into the first cone knocks it over and sends it flying
    scr::Car car;
    car.reset(scr::CarSetup{}, *track, 930, 44000);
    car.collision_radius = 300;
    car.pos_x = 930;
    car.pos_z = 44477 + 200;
    car.pos_y = cng.list[0].y;
    car.yaw = 0;
    car.speed = 8000;
    CHECK(cng.collide(car) == 1);
    CHECK(cng.hits == 1);
    CHECK(cng.list[0].hit);
    const int y0 = cng.list[0].y;
    cng.update(*track, 0.02f);
    CHECK(cng.list[0].y > y0);
    CHECK(cng.collide(car) == 0);  // counted once
    scr::Race race;
    REQUIRE(race.load(*data, "MAP1"));
    REQUIRE(race.loads_gates(*data, "MAP1"));
    CHECK(race.gates.size() == 14);
    CHECK(race.gates[0].x == 900);
}
