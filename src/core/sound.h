#pragma once
// Sound effects as the original plays them through the HMI SOS DIGI library (sfx_* in STARTL.EXE).
//
// Samples are DATA/SOUND.DAT entries <name>.RAW: unsigned 8-bit mono at 11025 Hz (the rate
// sound_init asks the driver for). Each comes with a SOUND_FX record (SoundFxDef, 0x24 bytes)
// from a .DTR section:
//
//   name[12]  volume  loop  loop_start  loop_leng  pitch  pan
//
// volume, pitch and pan carry flags in the top byte (0x10000000: the game changes it while
// playing); the low bits are the start value (volume 0..0x7fff, pitch 16.16, pan 0..0xffff with
// 0x8000 the centre). loop is a loop count (-1 forever); with loop_leng the sample plays once
// and then repeats [loop_start, loop_start + loop_leng).
//
// The tables are loaded in this order (sfx_load_tables), and the game plays samples by index:
//   GEN.DAT SFXSBASE.DTR  0 skidfx 1 crash 2 oppmotor 3 automoto 4 wallh1 5 hit_0 6 click
//                         7 autogall 8 dentone 9 door 10 taz3 11 taz4
//   MAPn.DAT MAPnDAT.DTR  the track's own (animated trucks, planes, ...)
//   GEN.DAT SPFXBASE.DTR + SPFXFULL.DTR  the announcer (g_speech_first_sfx)
//
// Mixer stands in for the SOS driver: voices with a sample, pitch, volume, pan and loop, mixed
// in software. It has no SDL dependency; the frontend pulls stereo float frames from it.

#include <array>
#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scr {

constexpr int kSampleRate = 11025;  // the driver rate the original asks for (0x2b11)

struct SoundFx {
    std::string name;
    uint32_t volume = 0x7fff, pitch = 0x10000, pan = 0x01000000;
    int loop = 0, loop_start = 0, loop_leng = 0;
    std::vector<uint8_t> data;  // unsigned 8-bit, 11025 Hz
};

// Reads SOUND_FX records from a .DTR text.
std::vector<SoundFx> parse_sound_fx(const std::string& dtr_text);

class SoundBank {
public:
    // gen_dat: DATA/GEN.DAT, sound_dat: DATA/SOUND.DAT, map_dat: DATA/MAPn.DAT (may be empty),
    // track_name "MAP1". speech: load the announcer (SPFXBASE, and SPFXFULL when full_speech).
    bool load(const std::string& gen_dat, const std::string& sound_dat, const std::string& map_dat,
              const std::string& track_name, bool speech = true, bool full_speech = true);
    std::vector<SoundFx> fx;
    int num_base = 0;          // g_num_base_sound_fx
    int speech_first = -1;     // g_speech_first_sfx
    int num_speech = 0;        // g_num_speech_sfx
    bool speech_full = false;  // g_speech_full_set
};

// Something that adds itself to the mix (CD audio).
class AudioSource {
public:
    virtual ~AudioSource() = default;
    // Adds `frames` stereo frames at `rate` Hz to out (interleaved L, R), scaled by gain.
    virtual void mix(float* out, int frames, int rate, float gain) = 0;
};

class Mixer {
public:
    static constexpr int kVoices = 16;

    void set_bank(const SoundBank* bank);

    // sfx_play: starts a sample with its SOUND_FX settings; returns a handle, -1 if none free.
    int play(int sample);
    // sfx_stop: returns -1 (what the game stores back into its handle).
    int stop(int handle);
    void stop_all();
    // sfx_play_continue: the voice switches to another sample when the current one wraps.
    void continue_with(int handle, int sample);
    void set_pitch(int handle, int pitch);    // 16.16 (sosDIGISetPitch); 0 becomes 1
    void set_volume(int handle, int volume);  // 0..0x7fff (sosDIGISetSampleVolume)
    void set_pan(int handle, int pan);        // 0 left .. 0x8000 centre .. 0xffff right
    bool playing(int handle) const;           // the handle still plays its sample
    int sample_of(int handle) const;          // -1 when the voice has ended
    int active_voices() const;

    // Options: master volumes 0..1, linear interpolation instead of the driver's nearest sample.
    std::atomic<float> sfx_volume{1.0f}, music_volume{0.7f};
    std::atomic<bool> smooth{true};
    void set_music(std::shared_ptr<AudioSource> music);
    // Called on every play (sample index), for logging.
    std::function<void(int)> on_play;

    // Mixes `frames` stereo float frames at `rate` Hz (called from the audio thread).
    void mix(float* out, int frames, int rate);

private:
    struct Voice {
        int sample = -1, next = -1;
        uint32_t generation = 0;
        uint64_t pos = 0;      // 32.32 fixed point position in the sample
        uint32_t pitch = 0x10000;
        int volume = 0x7fff, pan = 0x8000;
        bool use_pan = false;
        int loops_left = 0;    // -1 forever
        bool segment = false;  // loops the loop_start..loop_leng segment
    };
    Voice* voice(int handle);
    const Voice* voice(int handle) const;
    void start(Voice& v, int sample);

    mutable std::mutex mutex_;
    const SoundBank* bank_ = nullptr;
    std::array<Voice, kVoices> voices_{};
    uint32_t generation_ = 0;
    std::shared_ptr<AudioSource> music_;
};

} // namespace scr
