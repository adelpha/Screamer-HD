#include "core/fm_synth.h"

#include "core/archive.h"

#include <opl3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scr {

namespace {

constexpr double kChipRate = 49716.0;  // OPL clock / 288

uint32_t rd32(const std::vector<uint8_t>& d, size_t o) {
    return o + 4 <= d.size() ? uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24
                             : 0;
}
uint16_t rd16(const std::vector<uint8_t>& d, size_t o) {
    return o + 2 <= d.size() ? uint16_t(d[o] | d[o + 1] << 8) : 0;
}

} // namespace

std::vector<FmPatch> parse_adlib_bank(const std::vector<uint8_t>& d) {
    std::vector<FmPatch> out;
    if (d.size() < 28 || std::memcmp(d.data() + 2, "ADLIB-", 6) != 0) return out;
    const int count = rd16(d, 10);
    const size_t names = rd32(d, 12), data = rd32(d, 16);
    out.resize(size_t(count));
    for (int i = 0; i < count; ++i) {
        const size_t n = names + size_t(i) * 12;
        if (n + 12 > d.size()) break;
        const int index = rd16(d, n);
        const int flag = d[n + 2];
        const size_t r = data + size_t(index) * 30;
        if (r + 30 > d.size() || i >= count) continue;
        FmPatch p;
        for (int k = 0; k < 2; ++k) {
            const uint8_t* o = d.data() + r + 2 + k * 13;
            FmOperator& op = p.op[size_t(k)];
            op.ksl = o[0] & 3;
            op.mult = o[1] & 15;
            if (k == 0) p.feedback = o[2] & 7;
            op.ar = o[3] & 15;
            op.sl = o[4] & 15;
            op.sustain = o[5] != 0;
            op.dr = o[6] & 15;
            op.rr = o[7] & 15;
            op.tl = o[8] & 63;
            op.am = o[9] != 0;
            op.vib = o[10] != 0;
            op.ksr = o[11] != 0;
            if (k == 0) p.additive = o[12] != 0;  // the OPL bit: 1 additive (most patches are FM)
            op.wave = d[r + 28 + size_t(k)] & 7;
        }
        p.note = flag;
        out[size_t(i)] = p;
    }
    return out;
}

std::optional<HmpSong> parse_hmp(const std::vector<uint8_t>& d) {
    if (d.size() < 0x40 || std::memcmp(d.data(), "HMIMIDIP", 8) != 0) return std::nullopt;
    HmpSong song;
    const uint32_t tracks = rd32(d, 0x30);
    song.ticks_per_second = int(rd32(d, 0x38));
    if (song.ticks_per_second <= 0) song.ticks_per_second = 120;
    size_t pos = std::memcmp(d.data() + 8, "013195", 6) == 0 ? 0x388 : 0x308;
    struct Ev {
        MidiEvent e;
        uint32_t order;
    };
    std::vector<Ev> all;
    uint32_t order = 0;
    for (uint32_t t = 0; t < tracks && pos + 12 <= d.size(); ++t) {
        const uint32_t len = rd32(d, pos + 4);
        if (len < 12 || pos + len > d.size()) return std::nullopt;
        size_t i = pos + 12;
        const size_t end = pos + len;
        pos = end;
        uint32_t tick = 0;
        uint8_t status = 0;
        while (i < end) {
            uint32_t delta = 0;
            int shift = 0;
            while (i < end) {
                const uint8_t b = d[i++];
                delta |= uint32_t(b & 0x7f) << shift;
                shift += 7;
                if (b & 0x80) break;
            }
            tick += delta;
            if (i >= end) break;
            if (d[i] & 0x80) status = d[i++];
            if (status == 0xff) {
                if (i + 2 > end) break;
                const uint8_t type = d[i];
                const size_t l = d[i + 1];
                i += 2 + l;
                if (type == 0x2f) break;
                continue;
            }
            if (status == 0xf0 || status == 0xf7) {  // sysex: skip to its end
                while (i < end && d[i] != 0xf7) ++i;
                ++i;
                continue;
            }
            const int hi = status & 0xf0;
            const int n = (hi == 0xc0 || hi == 0xd0) ? 1 : 2;
            if (i + size_t(n) > end || hi < 0x80) break;
            MidiEvent e;
            e.tick = tick;
            e.status = status;
            e.a = d[i];
            e.b = n == 2 ? d[i + 1] : 0;
            i += size_t(n);
            all.push_back({e, order++});
        }
        song.length = std::max(song.length, tick);
    }
    std::stable_sort(all.begin(), all.end(), [](const Ev& x, const Ev& y) { return x.e.tick < y.e.tick; });
    for (const Ev& e : all) song.events.push_back(e.e);
    return song;
}

// --- HMI's OPL3 MIDI driver ("fmmidi3.com" in HMIMDRV.386, driver 0xa009) ---
//
// Reverse engineered from the driver blob. It plays 9 voices; each is written twice, to the
// same channel of both register banks, bank 0 sent to output B (0x20) and bank 1 to output A
// (0x10), so a voice can be panned by turning one copy down. Everything below follows the
// driver's integer maths:
//
//   note on   v = ((volume << 7) / 127) * velocity >> 7
//             level = (0x2000 - (0x40 - kAtten[v >> 1]) * 2 * (0x40 - patch_tl)) >> 7
//             written to the carrier (and the modulator of additive patches) of both copies
//   pan (CC10, and after every note on)
//             p = pan < 0x40 ? pan : 0x7f - pan
//             u = (p << 1) * ((volume * velocity >> 7) & 0xff) >> 7, level as above from u,
//             written to the copy in bank 1 when pan < 0x40, else to bank 0
//   volume (CC7)   recomputes the other copy of each sounding voice, then the pan
//   pitch bend     MSB only, +-2 semitones by default (CC 102 sets the range): linear between
//                  the F-numbers of the notes around, the driver's own way across blocks
//   sustain (CC64) note offs wait (up to 32 per channel) until the controller goes to 0
//   voices         the first free one; else one playing on the lowest channel without an
//                  active pitch bend; else voice = channel (minus 9)
// Not handled by the driver, so not here either: expression (CC11), modulation, aftertouch.

namespace {

// Attenuation in level steps by v >> 1 (the driver's table at 0x35d0).
constexpr uint8_t kAtten[64] = {63, 58, 53, 48, 44, 41, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28,
                                27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 14, 13,
                                13, 12, 12, 11, 11, 10, 10, 9,  9,  8,  8,  7,  7,  6,  6,  6,
                                5,  5,  5,  4,  4,  4,  4,  3,  3,  3,  2,  2,  2,  1,  1,  0};

// Note -> A0/B0 word (F-number, block << 10) at 0x35e4. Notes below 12 read into the table
// above in the original; they are rebuilt here from the same bytes.
uint32_t note_word(int note) {
    static const std::array<uint32_t, 128> table = [] {
        std::array<uint32_t, 128> t{};
        // 0x35e4..0x360f: bytes 20..63 of kAtten, then 0x3610 = 0.
        uint8_t raw[48]{};
        for (int i = 0; i < 44; ++i) raw[i] = kAtten[20 + i];
        for (int n = 0; n < 12; ++n)
            t[size_t(n)] = uint32_t(raw[n * 4]) | uint32_t(raw[n * 4 + 1]) << 8 | uint32_t(raw[n * 4 + 2]) << 16 |
                           uint32_t(raw[n * 4 + 3]) << 24;
        static constexpr uint16_t kFnum[12] = {0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca,
                                               0x1e5, 0x202, 0x220, 0x241, 0x263, 0x287};
        for (int n = 12; n < 108; ++n) t[size_t(n)] = uint32_t(kFnum[(n - 12) % 12]) | uint32_t((n - 12) / 12) << 10;
        static constexpr uint16_t kHigh[20] = {0x1eae, 0x1eb7, 0x1f02, 0x1f30, 0x1f60, 0x1f94, 0x1fca,
                                               0x144,  0x132,  0x121,  0x110,  0x101,  0xf8,   0xe5,
                                               0xd8,   0xcc,   0xc1,   0xb6,   0xac,   0xa1};
        for (int n = 108; n < 128; ++n) t[size_t(n)] = kHigh[n - 108];
        return t;
    }();
    return table[size_t(note & 127)];
}

// The driver's voice -> operator slots (modulator, carrier).
constexpr int kVoiceSlots[9][2] = {{0, 3}, {1, 4}, {2, 5}, {8, 11}, {9, 12}, {10, 13}, {16, 19}, {17, 20}, {18, 21}};

int level(int v, int reg40) {
    return ((0x2000 - (0x40 - int(kAtten[(v & 0xff) >> 1])) * 2 * (0x40 - (reg40 & 0x3f))) >> 7) & 0xff;
}

} // namespace

struct FmMidiSynth::Voice {};

FmMidiSynth::FmMidiSynth(std::vector<FmPatch> melodic, std::vector<FmPatch> drums)
    : melodic_(std::move(melodic)), drums_(std::move(drums)), chip_(std::make_unique<opl3_chip>()) {}

FmMidiSynth::~FmMidiSynth() = default;

void FmMidiSynth::write(int bank, int reg, int value) {
    // Spaced out in time like on the card: the driver's port delays make each write take about
    // two of the chip's samples, so a key off followed by a key on is seen as both (without
    // this a note retriggered on the same channel never restarts its envelope).
    OPL3_WriteRegBuffered(chip_.get(), uint16_t((bank ? 0x100 : 0) + reg), uint8_t(value));
}

void FmMidiSynth::chip_reset(int rate) {
    OPL3_Reset(chip_.get(), uint32_t(rate));
    rate_ = rate;
    write(1, 0x05, 0x01);  // OPL3 mode
    write(1, 0x04, 0x00);
    write(0, 0xbd, 0x00);
    for (int v = 0; v < 9; ++v) {
        voice_note_[size_t(v)] = 0;
        b0_[size_t(v)] = 0;
        write(0, 0x40 + kVoiceSlots[v][1], 0xff);
        write(1, 0x40 + kVoiceSlots[v][1], 0xff);
    }
}

void FmMidiSynth::reset() {
    // FUN_00000a5b
    for (int v = 0; v < 9; ++v) {
        voice_note_[size_t(v)] = 0;
        voice_channel_[size_t(v)] = 0;
        voice_velocity_[size_t(v)] = 0x7f;
    }
    for (int c = 0; c < 16; ++c) {
        ch_bend_[size_t(c)] = 0x40;
        ch_program_[size_t(c)] = 0;
        ch_bending_[size_t(c)] = false;
        ch_volume_[size_t(c)] = 0x7f;
        ch_pan_[size_t(c)] = 0x40;
        ch_sustain_[size_t(c)] = 0;
        ch_range_[size_t(c)] = 2;
        held_[size_t(c)].clear();
    }
    if (rate_ > 0) chip_reset(rate_);
}

int FmMidiSynth::active_voices() const {
    return int(std::count_if(voice_note_.begin(), voice_note_.end(), [](int n) { return n != 0; }));
}

void FmMidiSynth::set_freq(int v, uint32_t word, bool retrigger) {
    // FUN_00002a0a (key off, then on) / FUN_00002b26 (just the new pitch)
    a0_[size_t(v)] = int(word & 0xff);
    b0_[size_t(v)] = int((word >> 8) & 0xff) | 0x20;
    for (int bank = 0; bank < 2; ++bank) {
        write(bank, 0xa0 + v, a0_[size_t(v)]);
        if (retrigger) write(bank, 0xb0 + v, b0_[size_t(v)] & 0xdf);
        write(bank, 0xb0 + v, b0_[size_t(v)]);
    }
}

void FmMidiSynth::key_off(int v) {
    // FUN_000020d8
    if (voice_note_[size_t(v)] == 0) return;
    b0_[size_t(v)] &= 0xdf;
    write(0, 0xb0 + v, b0_[size_t(v)]);
    write(1, 0xb0 + v, b0_[size_t(v)]);
    voice_note_[size_t(v)] = 0;
}

void FmMidiSynth::load_patch(int v, const FmPatch& p) {
    // FUN_00002162 with the bank converted like FUN_00000cd6 (BNK fields -> register bytes)
    for (int k = 0; k < 2; ++k) {
        const FmOperator& op = p.op[size_t(k)];
        const int slot = kVoiceSlots[v][k];
        const int r20 = (op.am ? 0x80 : 0) | (op.vib ? 0x40 : 0) | (op.sustain ? 0x20 : 0) | (op.ksr ? 0x10 : 0) | (op.mult & 15);
        const int r40 = (op.ksl & 3) << 6 | (op.tl & 63);
        reg40_[size_t(slot)] = r40;
        for (int bank = 0; bank < 2; ++bank) {
            write(bank, 0x20 + slot, r20);
            write(bank, 0x40 + slot, r40);
            write(bank, 0x60 + slot, (op.ar & 15) << 4 | (op.dr & 15));
            write(bank, 0x80 + slot, (op.sl & 15) << 4 | (op.rr & 15));
            write(bank, 0xe0 + slot, op.wave & 7);
        }
    }
    const int c0 = ((p.feedback & 7) << 1 | (p.additive ? 1 : 0)) & 0x1f;
    write(0, 0xc0 + v, c0 | 0x20);
    write(1, 0xc0 + v, c0 | 0x10);
    voice_additive_[size_t(v)] = p.additive;
}

void FmMidiSynth::write_level(int bank, int v, int value) {
    // carrier, and the modulator when both are heard
    const int car = kVoiceSlots[v][1], mod = kVoiceSlots[v][0];
    if (voice_additive_[size_t(v)]) write(bank, 0x40 + mod, level(value, reg40_[size_t(mod)]) | (reg40_[size_t(mod)] & 0xc0));
    write(bank, 0x40 + car, level(value, reg40_[size_t(car)]) | (reg40_[size_t(car)] & 0xc0));
}

void FmMidiSynth::pan(int ch, int value) {
    // FUN_0000276d
    ch_pan_[size_t(ch)] = value;
    int p = value;
    if (0x3f < p) p = 0x7f - p;
    for (int v = 0; v < 9; ++v) {
        if (voice_note_[size_t(v)] == 0 || voice_channel_[size_t(v)] != ch) continue;
        const int u = ((p << 1) & 0xff) * (((ch_volume_[size_t(ch)] * voice_velocity_[size_t(v)]) >> 7) & 0xff) >> 7;
        write_level(ch_pan_[size_t(ch)] < 0x40 ? 1 : 0, v, u);
    }
}

void FmMidiSynth::volume(int ch, int value) {
    // FUN_000024cb
    ch_volume_[size_t(ch)] = value;
    for (int v = 0; v < 9; ++v) {
        if (voice_note_[size_t(v)] == 0 || voice_channel_[size_t(v)] != ch) continue;
        const int u = int(((uint64_t(value) << 7) / 0x7f) * uint64_t(voice_velocity_[size_t(v)]) >> 7);
        write_level(ch_pan_[size_t(ch)] < 0x40 ? 0 : 1, v, u);
    }
    pan(ch, ch_pan_[size_t(ch)]);
}

uint32_t FmMidiSynth::bend_word(int msb, int note, int v) const {
    // FUN_00001b5f, including its use of the voice number as a channel when bending down.
    const int i = note - 12;
    int r = i;
    while (11 < r) r -= 12;
    auto T = [](int k) { return note_word(std::clamp(k + 12, 0, 127)); };
    uint32_t base = T(i);
    uint32_t delta;
    if (msb < 0x40) {
        delta = base - T(i - ch_range_[size_t(v & 15)]);
        if (0x2cf < delta) delta = ((base & 0x3ff) - note_word(114 + ch_range_[size_t(voice_channel_[size_t(v)])])) & 0x3ff;
        base = base - delta * uint32_t(((0x3f - msb) * 1000) >> 6) / 1000;
    } else {
        const int range = ch_range_[size_t(voice_channel_[size_t(v)])];
        delta = T(i + range) - base;
        if (0x2cf < delta) {
            base = ((base & 0x1c00) + 0x400) | note_word(std::clamp(115 + (11 - r), 0, 127));
            delta = T(i + range) - base;
        }
        base = base + delta * uint32_t(((msb - 0x40) * 1000) >> 6) / 1000;
    }
    return base;
}

int FmMidiSynth::pick_voice(int ch) const {
    // FUN_00002033
    for (int v = 0; v < 9; ++v)
        if (voice_note_[size_t(v)] == 0) return v;
    for (int c = 0; c < 16; ++c) {
        if (ch_bending_[size_t(c)]) continue;
        for (int v = 0; v < 9; ++v)
            if (voice_channel_[size_t(v)] == c) return v;
    }
    return ch > 8 ? ch - 9 : ch;
}

void FmMidiSynth::note_on(int ch, int note, int vel) {
    // FUN_00000ef9
    if (vel == 0) {
        note_off(ch, note);
        return;
    }
    const FmPatch* patch;
    uint32_t word;
    if (ch == 9) {
        if (size_t(note) >= drums_.size()) return;
        patch = &drums_[size_t(note)];
        word = note_word(patch->note);
    } else {
        if (size_t(ch_program_[size_t(ch)]) >= melodic_.size()) return;
        patch = &melodic_[size_t(ch_program_[size_t(ch)])];
        word = note_word(note);
    }
    const int v = pick_voice(ch);
    key_off(v);
    voice_channel_[size_t(v)] = ch;
    load_patch(v, *patch);
    voice_velocity_[size_t(v)] = vel;
    const int u = int(((uint64_t(ch_volume_[size_t(ch)]) << 7) / 0x7f) * uint64_t(vel) >> 7);
    write_level(1, v, u);
    write_level(0, v, u);
    voice_note_[size_t(v)] = note;
    pan(ch, ch_pan_[size_t(ch)]);
    set_freq(v, word, true);
    if (ch != 9 && ch_bending_[size_t(ch)]) set_freq(v, bend_word(ch_bend_[size_t(ch)], note, v), false);
}

void FmMidiSynth::note_off(int ch, int note) {
    // FUN_00001695
    auto& held = held_[size_t(ch)];
    if (ch_sustain_[size_t(ch)] != 0 && held.size() < 32) {
        held.push_back(note);
        return;
    }
    for (int v = 0; v < 9; ++v)
        if (voice_note_[size_t(v)] == note && voice_channel_[size_t(v)] == ch) key_off(v);
}

void FmMidiSynth::message(uint8_t status, uint8_t a, uint8_t b) {
    if (rate_ <= 0) {
        chip_reset(44100);
        reset();
    }
    const int ch = status & 15;
    switch (status & 0xf0) {
    case 0x80: note_off(ch, a); break;
    case 0x90: note_on(ch, a, b); break;
    case 0xb0:
        if (a == 7) {
            volume(ch, b);
        } else if (a == 10) {
            pan(ch, b);
        } else if (a == 64) {
            ch_sustain_[size_t(ch)] = b;
            if (b == 0) {
                auto held = std::move(held_[size_t(ch)]);
                held_[size_t(ch)].clear();
                while (!held.empty()) {
                    note_off(ch, held.back());
                    held.pop_back();
                }
            }
        } else if (a == 102) {
            ch_range_[size_t(ch)] = b;  // HMI's pitch bend range controller
        } else if (a == 121) {
            ch_volume_[size_t(ch)] = 0x7f;
            if (ch < 9) voice_velocity_[size_t(ch)] = 0x7f;  // the driver indexes voices by channel here
            ch_sustain_[size_t(ch)] = 0;
            ch_bend_[size_t(ch)] = 0x40;
            ch_range_[size_t(ch)] = 2;
        } else if (a == 123) {
            for (int v = 0; v < 9; ++v)
                if (voice_channel_[size_t(v)] == ch) key_off(v);
        }
        break;
    case 0xc0: ch_program_[size_t(ch)] = a & 127; break;
    case 0xe0:
        // FUN_00001a9d: the MSB, not on the drum channel
        if (ch == 9) break;
        ch_bend_[size_t(ch)] = b;
        ch_bending_[size_t(ch)] = true;
        for (int v = 0; v < 9; ++v)
            if (voice_note_[size_t(v)] != 0 && voice_channel_[size_t(v)] == ch)
                set_freq(v, bend_word(b, voice_note_[size_t(v)], v), false);
        break;
    default: break;
    }
}

void FmMidiSynth::render(float* out, int frames, int rate, float gain) {
    if (rate != rate_) {
        // The chip resamples to the output rate; a new rate needs a reset.
        chip_reset(rate);
        reset();
    }
    int16_t s[2];
    const float k = gain * (2.5f / 32768.0f);
    for (int i = 0; i < frames; ++i) {
        OPL3_GenerateResampled(chip_.get(), s);
        out[i * 2] += float(s[0]) * k;
        out[i * 2 + 1] += float(s[1]) * k;
    }
}

HmpPlayer::HmpPlayer(HmpSong song, std::vector<FmPatch> melodic, std::vector<FmPatch> drums, bool loop)
    : song_(std::move(song)), synth_(std::move(melodic), std::move(drums)), loop_(loop) {}

void HmpPlayer::mix(float* out, int frames, int rate, float gain) {
    if (done_ || rate <= 0) return;
    const double ticks_per_frame = double(song_.ticks_per_second) / double(rate);
    int done = 0;
    while (done < frames) {
        // Events due now.
        while (next_ < song_.events.size() && double(song_.events[next_].tick) <= tick_) {
            const MidiEvent& e = song_.events[next_++];
            synth_.message(e.status, e.a, e.b);
        }
        if (next_ >= song_.events.size() && tick_ >= double(song_.length)) {
            if (!loop_) {
                done_ = true;
                return;
            }
            // MidiMusic_update restarts the song when it ends.
            for (int ch = 0; ch < 16; ++ch) synth_.message(uint8_t(0xb0 | ch), 123, 0);
            next_ = 0;
            tick_ = 0;
            continue;
        }
        // Render up to the next event.
        double until = next_ < song_.events.size() ? double(song_.events[next_].tick) : double(song_.length);
        int n = int(std::ceil((until - tick_) / ticks_per_frame));
        n = std::clamp(n, 1, frames - done);
        synth_.render(out + size_t(done) * 2, n, rate, gain);
        tick_ += double(n) * ticks_per_frame;
        done += n;
    }
}

std::shared_ptr<HmpPlayer> load_hmp_music(const std::string& music_dat, int number, bool loop) {
    auto a = Archive::open(music_dat);
    if (!a) return nullptr;
    auto hmp = a->read("MUSIC" + std::to_string(number) + ".HMP");
    auto mel = a->read("MELODIC.BNK");
    auto drum = a->read("DRUM.BNK");
    if (!hmp || !mel || !drum) return nullptr;
    auto song = parse_hmp(*hmp);
    auto m = parse_adlib_bank(*mel);
    auto d = parse_adlib_bank(*drum);
    if (!song || m.empty()) return nullptr;
    return std::make_shared<HmpPlayer>(std::move(*song), std::move(m), std::move(d), loop);
}

int music_for_race(int map_index) {
    static constexpr int kTable[8] = {2, 3, 2, 4, 5, 2, 4, 5};  // 0x101efc
    return kTable[std::clamp(map_index, 0, 5) + 2];
}

} // namespace scr
