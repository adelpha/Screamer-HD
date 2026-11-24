#include "core/fm_synth.h"

#include "core/archive.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scr {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kChipRate = 49716.0;  // OPL clock / 288
constexpr int kVoices = 18;

uint32_t rd32(const std::vector<uint8_t>& d, size_t o) {
    return o + 4 <= d.size() ? uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24
                             : 0;
}
uint16_t rd16(const std::vector<uint8_t>& d, size_t o) {
    return o + 2 <= d.size() ? uint16_t(d[o] | d[o + 1] << 8) : 0;
}

// The OPL's frequency multipliers (x2: 0 is one half).
constexpr double kMult[16] = {0.5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15};
// Key scale level at block 7 by the top four F-number bits, dB for 6 dB/octave.
constexpr double kKsl[16] = {0.0, 9.0, 12.0, 13.875, 15.0, 16.125, 16.875, 17.625,
                             18.0, 18.75, 19.125, 19.5, 19.875, 20.25, 20.625, 21.0};

double waveform(int wave, double phase) {
    const double p = phase - std::floor(phase);
    const double s = std::sin(2.0 * kPi * p);
    switch (wave & 7) {
    case 1: return p < 0.5 ? s : 0.0;                          // half sine
    case 2: return std::fabs(s);                               // absolute sine
    case 3: return (p < 0.25 || (p >= 0.5 && p < 0.75)) ? std::fabs(s) : 0.0;  // quarter pulses
    case 4: return p < 0.5 ? std::sin(4.0 * kPi * p) : 0.0;    // OPL3 extras
    case 5: return p < 0.5 ? std::fabs(std::sin(4.0 * kPi * p)) : 0.0;
    case 6: return p < 0.5 ? 1.0 : -1.0;
    case 7: return p < 0.5 ? std::pow(2.0, -p * 16.0) : -std::pow(2.0, -(1.0 - p) * 16.0);
    default: return s;
    }
}

// Time in ms for a full envelope sweep at effective rate r (4 * R + key scaling).
double decay_ms(int r) {
    if (r < 4) return 0;
    static constexpr double frac[4] = {1.0, 0.8, 0.6667, 0.5714};
    return 39280.64 / std::pow(2.0, double((r >> 2) - 1)) * frac[r & 3];
}
double attack_ms(int r) {
    if (r < 4) return 0;
    if (r >= 60) return 0.0001;
    static constexpr double frac[4] = {1.0, 0.7971, 0.6667, 0.5652};
    return 2826.24 / std::pow(2.0, double((r >> 2) - 1)) * frac[r & 3];
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

// --- The FM voice ---

struct FmMidiSynth::Voice {
    struct Op {
        double phase = 0;
        double att = 96.0;  // envelope attenuation, dB
        int stage = 4;      // 0 attack, 1 decay, 2 sustain, 3 release, 4 off
        double out[2]{};
    };
    FmPatch patch;
    std::array<Op, 2> op{};
    int channel = -1, note = -1, play_note = 60;
    int velocity = 0;
    bool on = false, held = false;
    uint64_t age = 0;
    double level_db[2]{};  // extra attenuation from velocity and volume
    double pan_l = 1, pan_r = 1;
    double fnum = 0;
    int block = 0;

    bool active() const { return op[1].stage != 4 || (patch.additive && op[0].stage != 4); }

    void set_pitch(double semitones) {
        const double hz = 440.0 * std::pow(2.0, (semitones - 69.0) / 12.0);
        block = 0;
        fnum = hz * double(1 << 20) / kChipRate;
        while (fnum >= 1024.0 && block < 7) {
            ++block;
            fnum = hz * double(1 << (20 - block)) / kChipRate;
        }
    }
    int key_scale() const { return (block << 1) | (int(fnum) >> 9); }

    void key_on() {
        for (Op& o : op) {
            o.stage = 0;
            o.phase = 0;
        }
        on = true;
    }
    void key_off() {
        for (size_t k = 0; k < 2; ++k)
            if (op[k].stage < 3) op[k].stage = 3;
        on = false;
    }

    // One operator sample: returns its output (-1..1).
    double run(size_t k, double mod, double dt, double am_db, double vib) {
        const FmOperator& p = patch.op[k];
        Op& o = op[k];
        if (o.stage == 4) {
            o.out[1] = o.out[0];
            o.out[0] = 0;
            return 0;
        }
        const int ks = p.ksr ? key_scale() : key_scale() >> 2;
        auto rate = [&](int r) { return r == 0 ? 0 : std::min(63, r * 4 + ks); };
        switch (o.stage) {
        case 0: {
            const double t = attack_ms(rate(p.ar));
            if (t <= 0) break;  // rate 0: stays
            // Exponential approach to full level (ln(960) time constants over the sweep).
            o.att -= o.att * (6.87 * dt * 1000.0 / t) + 1e-6;
            if (o.att <= 0.05) {
                o.att = 0;
                o.stage = 1;
            }
            break;
        }
        case 1: {
            const double sl = p.sl == 15 ? 93.0 : p.sl * 3.0;
            const double t = decay_ms(rate(p.dr));
            if (t > 0) o.att += 96.0 * dt * 1000.0 / t;
            if (o.att >= sl) {
                o.att = sl;
                o.stage = 2;
            }
            break;
        }
        case 2:
            if (!p.sustain) {
                const double t = decay_ms(rate(p.rr));
                if (t > 0) o.att += 96.0 * dt * 1000.0 / t;
            }
            break;
        case 3: {
            const double t = decay_ms(rate(p.rr));
            if (t > 0) o.att += 96.0 * dt * 1000.0 / t;
            break;
        }
        default: break;
        }
        if (o.att >= 96.0) {
            o.att = 96.0;
            if (o.stage >= 2) o.stage = 4;
        }
        // Frequency with vibrato.
        const double hz = fnum * kChipRate / double(1 << (20 - block)) * kMult[p.mult] * (p.vib ? vib : 1.0);
        double ksl = 0;
        if (p.ksl) {
            ksl = std::max(0.0, kKsl[int(fnum) >> 6] - 6.0 * (7 - block));
            ksl *= p.ksl == 1 ? 0.5 : p.ksl == 2 ? 0.25 : 1.0;
        }
        const double att = p.tl * 0.75 + ksl + o.att + level_db[k] + (p.am ? am_db : 0.0);
        const double amp = att >= 96.0 ? 0.0 : std::pow(10.0, -att / 20.0);
        const double v = waveform(p.wave, o.phase + mod) * amp;
        o.phase += hz * dt;
        if (o.phase > 1024.0) o.phase -= std::floor(o.phase);
        o.out[1] = o.out[0];
        o.out[0] = v;
        return v;
    }

    double sample(double dt, double am_db, double vib) {
        const Op& m = op[0];
        // Feedback: the average of the modulator's last two outputs, up to 2 cycles at 7.
        const double fb = patch.feedback ? (m.out[0] + m.out[1]) * 0.5 * std::pow(2.0, patch.feedback - 7) * 2.0 : 0.0;
        const double a = run(0, fb, dt, am_db, vib);
        // FM: the modulator moves the carrier's phase by up to 4 cycles.
        const double b = run(1, patch.additive ? 0.0 : a * 4.0, dt, am_db, vib);
        return patch.additive ? a + b : b;
    }
};

FmMidiSynth::FmMidiSynth(std::vector<FmPatch> melodic, std::vector<FmPatch> drums)
    : melodic_(std::move(melodic)), drums_(std::move(drums)) {
    for (int i = 0; i < kVoices; ++i) voices_.push_back(std::make_unique<Voice>());
}

FmMidiSynth::~FmMidiSynth() = default;

void FmMidiSynth::reset() {
    for (auto& v : voices_) *v = Voice{};
    channels_ = {};
}

int FmMidiSynth::active_voices() const {
    return int(std::count_if(voices_.begin(), voices_.end(), [](const auto& v) { return v->active(); }));
}

void FmMidiSynth::update_voice(Voice& v) {
    const Channel& c = channels_[size_t(v.channel)];
    // Velocity, volume and expression as attenuation (40 log10 like a GM synth's curve).
    const double g = (double(v.velocity) / 127.0) * (double(c.volume) / 127.0) * (double(c.expression) / 127.0);
    const double db = g <= 0.0 ? 96.0 : std::min(96.0, -40.0 * std::log10(g));
    v.level_db[0] = v.patch.additive ? db : 0.0;
    v.level_db[1] = db;
    const double pan = double(c.pan) / 127.0;
    v.pan_l = std::sqrt(1.0 - pan) * 1.41421356;
    v.pan_r = std::sqrt(pan) * 1.41421356;
    v.set_pitch(double(v.play_note) + (v.channel == 9 ? 0.0 : c.bend));
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
    // A free voice, else the oldest released one, else the oldest.
    Voice* pick = nullptr;
    for (auto& v : voices_)
        if (!v->active() && (!pick || v->age < pick->age)) pick = v.get();
    if (!pick)
        for (auto& v : voices_)
            if (!v->on && (!pick || v->age < pick->age)) pick = v.get();
    if (!pick)
        for (auto& v : voices_)
            if (!pick || v->age < pick->age) pick = v.get();
    Voice& v = *pick;
    const bool reuse = v.active();
    v.patch = *patch;
    v.channel = ch;
    v.note = note;
    v.play_note = play;
    v.velocity = vel;
    v.held = false;
    v.age = ++age_;
    update_voice(v);
    if (!reuse)
        for (auto& o : v.op) o.att = 96.0;
    v.key_on();
}

void FmMidiSynth::note_off(int ch, int note) {
    for (auto& v : voices_) {
        if (!v->on || v->channel != ch || v->note != note) continue;
        if (channels_[size_t(ch)].hold) v->held = true;
        else v->key_off();
    }
}

void FmMidiSynth::message(uint8_t status, uint8_t a, uint8_t b) {
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
                        v->key_off();
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
                if (v->channel == ch) v->key_off();
            break;
        default: break;
        }
        if (a == 7 || a == 10 || a == 11)
            for (auto& v : voices_)
                if (v->channel == ch && v->active()) update_voice(*v);
        break;
    case 0xc0: c.program = a & 127; break;
    case 0xe0:
        c.bend = float((int(a) | int(b) << 7) - 8192) / 8192.0f * 2.0f;
        for (auto& v : voices_)
            if (v->channel == ch && v->active()) update_voice(*v);
        break;
    default: break;
    }
}

void FmMidiSynth::render(float* out, int frames, int rate, float gain) {
    const double dt = 1.0 / double(rate);
    for (int i = 0; i < frames; ++i) {
        // Tremolo 1 dB at 3.7 Hz, vibrato 7 cents at 6.1 Hz (the OPL's shallow settings).
        const double am_db = 0.5 * (1.0 - std::cos(2.0 * kPi * 3.7 * lfo_time_));
        const double vib = std::pow(2.0, 7.0 / 1200.0 * std::sin(2.0 * kPi * 6.1 * lfo_time_));
        lfo_time_ += dt;
        double l = 0, r = 0;
        for (auto& v : voices_) {
            if (!v->active()) continue;
            const double s = v->sample(dt, am_db, vib);
            l += s * v->pan_l;
            r += s * v->pan_r;
        }
        out[i * 2] += float(l * 0.3 * gain);
        out[i * 2 + 1] += float(r * 0.3 * gain);
    }
    if (lfo_time_ > 1000.0) lfo_time_ -= 1000.0;
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
