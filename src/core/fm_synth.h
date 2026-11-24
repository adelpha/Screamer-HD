#pragma once
// MIDI music the way the DOS game plays it without a CD: HMI's MIDI player (MidiMusic_* in
// STARTL.EXE over HMIMDRV.386) with the songs and FM instrument banks of DATA/MUSIC.DAT.
//
//   MUSICn.HMP   HMI's MIDI variant. Header "HMIMIDIP013195" (or "HMIMIDIP" with tracks at
//                0x308); +0x30 track count, +0x38 ticks per second (120), +0x3c length in
//                seconds. Tracks from 0x388: { u32 index, u32 bytes incl. this header, u32 ?,
//                events }. Events are MIDI with running status; delta times are 7-bit groups,
//                least significant first, the last byte has bit 7 set.
//   MELODIC.BNK  AdLib instrument banks: "ADLIB-" header, u16 used, u16 count, u32 names
//   DRUM.BNK     offset, u32 data offset; names { u16 index, u8 flag, char[9] }; data 30 bytes
//                { u8 percussive, u8 voice, 2 x 13 operator bytes (ksl, multiple, feedback,
//                attack, sustain, sustaining, decay, release, level, am, vibrato, ksr,
//                connection), u8 wave 0, u8 wave 1 }. MELODIC has the 128 General MIDI
//                programs; DRUM is indexed by note and its flag byte is the pitch to play.
//
// The sound comes from an emulated Yamaha OPL3 (YMF262, the Sound Blaster 16's FM chip):
// Nuked-OPL3 (tgies/Nuked-OPL3-fast, LGPL-2.1, built as opl3.dll). FmMidiSynth is the MIDI
// driver in front of it: 18 two-operator channels, the patch in the operator registers, the
// note as F-number and block.
// Music number n goes with CD track n + 1 (menu MUSIC3, MAP1 MUSIC2, ...).

#include "core/sound.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct _opl3_chip;

namespace scr {

struct FmOperator {
    int ksl = 0, mult = 1, ar = 15, sl = 0, dr = 0, rr = 0, tl = 0;
    bool sustain = true, am = false, vib = false, ksr = false;
    int wave = 0;
};

struct FmPatch {
    std::array<FmOperator, 2> op{};  // 0 modulator, 1 carrier
    int feedback = 0;
    bool additive = false;           // connection 1: both operators are heard
    int note = -1;                   // DRUM.BNK: the pitch to play this drum at
};

// AdLib .BNK; patches by instrument index (empty on a malformed file).
std::vector<FmPatch> parse_adlib_bank(const std::vector<uint8_t>& bnk);

struct MidiEvent {
    uint32_t tick = 0;
    uint8_t status = 0, a = 0, b = 0;  // meta events are not kept
};

struct HmpSong {
    int ticks_per_second = 120;
    uint32_t length = 0;  // ticks: the end of the longest track
    std::vector<MidiEvent> events;  // all tracks merged, in time order
};

std::optional<HmpSong> parse_hmp(const std::vector<uint8_t>& hmp);

// MIDI messages to the OPL3's eighteen two-operator channels.
class FmMidiSynth {
public:
    FmMidiSynth(std::vector<FmPatch> melodic, std::vector<FmPatch> drums);
    ~FmMidiSynth();
    void message(uint8_t status, uint8_t a, uint8_t b);
    void reset();
    // Adds `frames` stereo frames at `rate` to out.
    void render(float* out, int frames, int rate, float gain);
    int active_voices() const;

private:
    struct Voice;
    struct Channel {
        int program = 0, volume = 100, expression = 127, pan = 64;
        float bend = 0;  // semitones
        bool hold = false;
    };
    void note_on(int ch, int note, int vel);
    void note_off(int ch, int note);
    void update_voice(Voice& v, bool patch);
    void key(Voice& v, bool on);
    void write(int reg, int value);
    void chip_reset(int rate);
    std::vector<FmPatch> melodic_, drums_;
    std::array<Channel, 16> channels_{};
    std::vector<std::unique_ptr<Voice>> voices_;
    std::unique_ptr<_opl3_chip> chip_;
    int rate_ = 0;
    uint64_t age_ = 0;
};

// Plays an HMP song on the FM synth, looping like MidiMusic_play(song, 1).
class HmpPlayer : public AudioSource {
public:
    HmpPlayer(HmpSong song, std::vector<FmPatch> melodic, std::vector<FmPatch> drums, bool loop = true);
    void mix(float* out, int frames, int rate, float gain) override;
    bool finished() const { return done_; }

private:
    HmpSong song_;
    FmMidiSynth synth_;
    bool loop_ = true, done_ = false;
    size_t next_ = 0;
    double tick_ = 0;
};

// MUSIC.DAT: loads MUSIC<n>.HMP with the two banks.
std::shared_ptr<HmpPlayer> load_hmp_music(const std::string& music_dat, int number, bool loop = true);

// The music number for a race on map `map_index` (0 = MAP1) and for the menu (0x101efc).
int music_for_race(int map_index);
constexpr int kMusicMenu = 3;

} // namespace scr
