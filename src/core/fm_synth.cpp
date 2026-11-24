#include "core/fm_synth.h"

#include "core/archive.h"

#include <opl3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scr {

namespace {

constexpr double kChipRate = 49716.0;  // OPL clock / 288
constexpr int kVoices = 18;

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

// --- The MIDI driver over the OPL3 ---
//
// Each of the 18 two-operator channels of the chip plays one note. The patch goes into the
// operator registers (0x20 tremolo/vibrato/sustain/KSR/multiple, 0x40 KSL/level, 0x60
// attack/decay, 0x80 sustain level/release, 0xE0 waveform, 0xC0 output/feedback/connection),
// the pitch into 0xA0/0xB0 as F-number and block with the key-on bit.

struct FmMidiSynth::Voice {
    int chip_channel = 0;
    int channel = -1, note = -1, play_note = 60;
    int velocity = 0;
    bool on = false, held = false;
    uint64_t age = 0;
    FmPatch patch;
    int fnum = 0, block = 0;
};

namespace {

constexpr int kSlot[9] = {0, 1, 2, 8, 9, 10, 16, 17, 18};

void pitch_to_fnum(double semitones, int& fnum, int& block) {
    const double hz = 440.0 * std::pow(2.0, (semitones - 69.0) / 12.0);
    block = 0;
    double f = hz * double(1 << 20) / kChipRate;
    while (f >= 1023.5 && block < 7) {
        ++block;
        f = hz * double(1 << (20 - block)) / kChipRate;
    }
    fnum = std::clamp(int(std::lround(f)), 0, 1023);
}

} // namespace

FmMidiSynth::FmMidiSynth(std::vector<FmPatch> melodic, std::vector<FmPatch> drums)
    : melodic_(std::move(melodic)), drums_(std::move(drums)), chip_(std::make_unique<opl3_chip>()) {
    for (int i = 0; i < kVoices; ++i) {
        voices_.push_back(std::make_unique<Voice>());
        voices_.back()->chip_channel = i;
    }
}

FmMidiSynth::~FmMidiSynth() = default;

void FmMidiSynth::write(int reg, int value) {
    OPL3_WriteReg(chip_.get(), uint16_t(reg), uint8_t(value));
}

void FmMidiSynth::chip_reset(int rate) {
    OPL3_Reset(chip_.get(), uint32_t(rate));
    rate_ = rate;
    write(0x105, 0x01);  // OPL3 mode: 18 channels, 8 waveforms, stereo
    write(0x104, 0x00);  // no four-operator channels
    write(0x01, 0x20);
    write(0xbd, 0x00);   // shallow tremolo and vibrato, no rhythm mode
    for (auto& v : voices_) {
        v->on = false;
        v->held = false;
    }
}

void FmMidiSynth::reset() {
    channels_ = {};
    if (rate_ > 0) chip_reset(rate_);
}

int FmMidiSynth::active_voices() const {
    return int(std::count_if(voices_.begin(), voices_.end(), [](const auto& v) { return v->on; }));
}

void FmMidiSynth::update_voice(Voice& v, bool patch) {
    const Channel& c = channels_[size_t(v.channel)];
    const int bank = v.chip_channel >= 9 ? 0x100 : 0;
    const int ch = v.chip_channel % 9;
    // Velocity, volume and expression as extra attenuation (40 log10, like a GM synth's
    // curve) in the level registers' 0.75 dB steps.
    const double g = (double(v.velocity) / 127.0) * (double(c.volume) / 127.0) * (double(c.expression) / 127.0);
    const double db = g <= 0.0 ? 48.0 : std::min(48.0, -40.0 * std::log10(g));
    const int extra = int(std::lround(db / 0.75));
    for (int k = 0; k < 2; ++k) {
        const FmOperator& op = v.patch.op[size_t(k)];
        const int slot = bank + kSlot[ch] + (k ? 3 : 0);
        const bool heard = k == 1 || v.patch.additive;
        if (patch) {
            write(0x20 + slot, (op.am ? 0x80 : 0) | (op.vib ? 0x40 : 0) | (op.sustain ? 0x20 : 0) | (op.ksr ? 0x10 : 0) |
                                   (op.mult & 15));
            write(0x60 + slot, (op.ar & 15) << 4 | (op.dr & 15));
            write(0x80 + slot, (op.sl & 15) << 4 | (op.rr & 15));
            write(0xe0 + slot, op.wave & 7);
        }
        write(0x40 + slot, (op.ksl & 3) << 6 | std::min(63, op.tl + (heard ? extra : 0)));
    }
    const int out = c.pan < 32 ? 0x10 : c.pan > 96 ? 0x20 : 0x30;  // the OPL3 has left, right or both
    write(0xc0 + bank + ch, out | (v.patch.feedback & 7) << 1 | (v.patch.additive ? 1 : 0));
    pitch_to_fnum(double(v.play_note) + (v.channel == 9 ? 0.0 : double(c.bend)), v.fnum, v.block);
    write(0xa0 + bank + ch, v.fnum & 0xff);
    write(0xb0 + bank + ch, (v.on ? 0x20 : 0) | (v.block & 7) << 2 | (v.fnum >> 8 & 3));
}

void FmMidiSynth::key(Voice& v, bool on) {
    v.on = on;
    const int bank = v.chip_channel >= 9 ? 0x100 : 0;
    write(0xb0 + bank + v.chip_channel % 9, (on ? 0x20 : 0) | (v.block & 7) << 2 | (v.fnum >> 8 & 3));
}

void FmMidiSynth::note_on(int ch, int note, int vel) {
    if (vel == 0) {
        note_off(ch, note);
        return;
    }
    const FmPatch* patch = nullptr;
    int play = note;
    if (ch == 9) {
        if (size_t(note) >= drums_.size()) return;
        patch = &drums_[size_t(note)];
        if (patch->note > 0 && patch->note < 128) play = patch->note;
    } else {
        const Channel& c = channels_[size_t(ch)];
        if (size_t(c.program) >= melodic_.size()) return;
        patch = &melodic_[size_t(c.program)];
    }
    // The longest released channel, else the oldest playing one.
    Voice* pick = nullptr;
    for (auto& v : voices_)
        if (!v->on && (!pick || v->age < pick->age)) pick = v.get();
    if (!pick)
        for (auto& v : voices_)
            if (!pick || v->age < pick->age) pick = v.get();
    Voice& v = *pick;
    if (v.on) key(v, false);
    v.patch = *patch;
    v.channel = ch;
    v.note = note;
    v.play_note = play;
    v.velocity = vel;
    v.held = false;
    v.age = ++age_;
    v.on = false;
    update_voice(v, true);
    key(v, true);
}

void FmMidiSynth::note_off(int ch, int note) {
    for (auto& v : voices_) {
        if (!v->on || v->channel != ch || v->note != note) continue;
        if (channels_[size_t(ch)].hold) v->held = true;
        else key(*v, false);
    }
}

void FmMidiSynth::message(uint8_t status, uint8_t a, uint8_t b) {
    if (rate_ <= 0) chip_reset(44100);
    const int ch = status & 15;
    Channel& c = channels_[size_t(ch)];
    switch (status & 0xf0) {
    case 0x80: note_off(ch, a); break;
    case 0x90: note_on(ch, a, b); break;
    case 0xb0:
        switch (a) {
        case 7: c.volume = b; break;
        case 10: c.pan = b; break;
        case 11: c.expression = b; break;
        case 64:
            c.hold = b >= 64;
            if (!c.hold)
                for (auto& v : voices_)
                    if (v->channel == ch && v->held) {
                        v->held = false;
                        key(*v, false);
                    }
            break;
        case 121:
            c.volume = 100;
            c.expression = 127;
            c.bend = 0;
            c.hold = false;
            break;
        case 120:
        case 123:
            for (auto& v : voices_)
                if (v->channel == ch && v->on) key(*v, false);
            break;
        default: break;
        }
        if (a == 7 || a == 10 || a == 11)
            for (auto& v : voices_)
                if (v->channel == ch && v->on) update_voice(*v, false);
        break;
    case 0xc0: c.program = a & 127; break;
    case 0xe0:
        c.bend = float((int(a) | int(b) << 7) - 8192) / 8192.0f * 2.0f;
        for (auto& v : voices_)
            if (v->channel == ch && v->on) update_voice(*v, false);
        break;
    default: break;
    }
}

void FmMidiSynth::render(float* out, int frames, int rate, float gain) {
    if (rate != rate_) {
        // The chip resamples to the output rate; a new rate needs a reset (and the patches again).
        chip_reset(rate);
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
