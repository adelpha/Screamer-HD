#include "core/sound.h"

#include "core/archive.h"
#include "core/dtr.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scr {

std::vector<SoundFx> parse_sound_fx(const std::string& dtr_text) {
    std::vector<SoundFx> out;
    auto values = DtrFile(dtr_text).section("SOUND_FX");
    if (!values) return out;
    // Records of 7 values: name, volume, loop, loop_start, loop_leng, pitch, pan.
    for (size_t i = 0; i + 7 <= values->size(); i += 7) {
        const auto& v = *values;
        SoundFx fx;
        fx.name = v[i].text;
        fx.volume = uint32_t(v[i + 1].i);
        fx.loop = v[i + 2].i;
        fx.loop_start = v[i + 3].i;
        fx.loop_leng = v[i + 4].i;
        fx.pitch = uint32_t(v[i + 5].i);
        fx.pan = uint32_t(v[i + 6].i);
        out.push_back(std::move(fx));
    }
    return out;
}

namespace {

std::string text_of(const Archive& a, const std::string& name) {
    auto d = a.read(name);
    return d ? std::string(d->begin(), d->end()) : std::string();
}

} // namespace

bool SoundBank::load(const std::string& gen_dat, const std::string& sound_dat, const std::string& map_dat,
                     const std::string& track_name, bool speech, bool full_speech) {
    fx.clear();
    num_base = 0;
    speech_first = -1;
    num_speech = 0;
    speech_full = false;
    auto gen = Archive::open(gen_dat);
    auto snd = Archive::open(sound_dat);
    if (!gen || !snd) return false;
    // sfx_load_tables: SFXSBASE, the track's SOUND_FX, then the announcer (sfx_load_special).
    fx = parse_sound_fx(text_of(*gen, "SFXSBASE.DTR"));
    num_base = int(fx.size());
    if (!map_dat.empty() && !track_name.empty()) {
        if (auto map = Archive::open(map_dat)) {
            auto track = parse_sound_fx(text_of(*map, track_name + "DAT.DTR"));
            fx.insert(fx.end(), track.begin(), track.end());
        }
    }
    if (speech) {
        speech_first = int(fx.size());
        auto base = parse_sound_fx(text_of(*gen, "SPFXBASE.DTR"));
        fx.insert(fx.end(), base.begin(), base.end());
        if (full_speech) {
            auto full = parse_sound_fx(text_of(*gen, "SPFXFULL.DTR"));
            speech_full = !full.empty();
            fx.insert(fx.end(), full.begin(), full.end());
        }
        num_speech = int(fx.size()) - speech_first;
    }
    // sfx_load_samples: <name>.RAW from SOUND.DAT.
    for (SoundFx& f : fx) {
        auto data = snd->read(f.name + ".RAW");
        if (data) f.data = std::move(*data);
    }
    return num_base > 0;
}

void Mixer::set_bank(const SoundBank* bank) {
    std::lock_guard lock(mutex_);
    bank_ = bank;
    for (Voice& v : voices_) v.sample = -1;
}

void Mixer::set_music(std::shared_ptr<AudioSource> music) {
    std::lock_guard lock(mutex_);
    music_ = std::move(music);
}

Mixer::Voice* Mixer::voice(int handle) {
    if (handle < 0) return nullptr;
    Voice& v = voices_[size_t(handle & (kVoices - 1))];
    if (v.sample < 0 || v.generation != uint32_t(handle) >> 4) return nullptr;
    return &v;
}

const Mixer::Voice* Mixer::voice(int handle) const {
    return const_cast<Mixer*>(this)->voice(handle);
}

void Mixer::start(Voice& v, int sample) {
    const SoundFx& f = bank_->fx[size_t(sample)];
    v.sample = sample;
    v.next = -1;
    v.pos = 0;
    v.pitch = f.pitch & 0x0fffffff;
    if (v.pitch == 0) v.pitch = 0x10000;
    v.volume = int(std::min<uint32_t>(f.volume & 0xffff, 0x7fff));
    v.use_pan = ((f.pan >> 24) & 0xf) != 0 && (f.pan & 0x10000000) != 0;
    v.pan = 0x8000;
    v.loops_left = f.loop;
    v.segment = f.loop_leng != 0;
}

int Mixer::play(int sample, SoundGroup group) {
    std::lock_guard lock(mutex_);
    if (!bank_ || sample < 0 || size_t(sample) >= bank_->fx.size() || bank_->fx[size_t(sample)].data.empty()) return -1;
    for (size_t i = 0; i < voices_.size(); ++i) {
        Voice& v = voices_[i];
        if (v.sample >= 0) continue;
        start(v, sample);
        v.group = group;
        generation_ = (generation_ + 1) & 0x07ffffff;
        v.generation = generation_;
        if (on_play) on_play(sample);
        return int(v.generation << 4 | uint32_t(i));
    }
    return -1;
}

int Mixer::stop(int handle) {
    std::lock_guard lock(mutex_);
    if (Voice* v = voice(handle)) v->sample = -1;
    return -1;
}

void Mixer::stop_all() {
    std::lock_guard lock(mutex_);
    for (Voice& v : voices_) v.sample = -1;
}

void Mixer::continue_with(int handle, int sample) {
    std::lock_guard lock(mutex_);
    if (!bank_ || sample < 0 || size_t(sample) >= bank_->fx.size()) return;
    if (Voice* v = voice(handle)) v->next = sample;
}

void Mixer::set_pitch(int handle, int pitch) {
    std::lock_guard lock(mutex_);
    if (Voice* v = voice(handle)) v->pitch = uint32_t(std::max(pitch, 1));
}

void Mixer::set_volume(int handle, int volume) {
    std::lock_guard lock(mutex_);
    if (Voice* v = voice(handle)) v->volume = std::clamp(volume, 0, 0x7fff);
}

void Mixer::set_pan(int handle, int pan) {
    std::lock_guard lock(mutex_);
    if (Voice* v = voice(handle)) {
        v->pan = std::clamp(pan, 0, 0xffff);
        v->use_pan = true;
    }
}

bool Mixer::playing(int handle) const {
    std::lock_guard lock(mutex_);
    return voice(handle) != nullptr;
}

int Mixer::sample_of(int handle) const {
    std::lock_guard lock(mutex_);
    const Voice* v = voice(handle);
    return v ? v->sample : -1;
}

int Mixer::active_voices() const {
    std::lock_guard lock(mutex_);
    return int(std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.sample >= 0; }));
}

void Mixer::mix(float* out, int frames, int rate) {
    std::fill(out, out + size_t(frames) * 2, 0.0f);
    std::lock_guard lock(mutex_);
    if (rate <= 0) return;
    const float sfx_gain = sfx_volume.load() * 0.5f;
    const bool lerp = smooth.load();
    if (bank_ && sfx_gain > 0.0f) {
        for (Voice& v : voices_) {
            if (v.sample < 0) continue;
            const float gain = float(v.volume) / 32767.0f * sfx_gain * group_volume[size_t(v.group)].load();
            float gl = gain, gr = gain;
            if (v.use_pan) {
                gl *= std::min(1.0f, float(0x10000 - v.pan) / 32768.0f);
                gr *= std::min(1.0f, float(v.pan) / 32768.0f);
            }
            // 32.32 step: pitch is 16.16 at the driver's 11025 Hz.
            const uint64_t step = (uint64_t(v.pitch) << 16) * uint64_t(kSampleRate) / uint64_t(rate);
            for (int i = 0; i < frames && v.sample >= 0; ++i) {
                const SoundFx& f = bank_->fx[size_t(v.sample)];
                const uint64_t size = f.data.size();
                uint64_t end = size;
                uint64_t loop_from = 0;
                if (v.segment && v.loops_left != 0) {
                    loop_from = uint64_t(std::clamp<int64_t>(f.loop_start, 0, int64_t(size)));
                    end = std::min<uint64_t>(size, loop_from + uint64_t(std::max(f.loop_leng, 0)));
                }
                uint64_t idx = v.pos >> 32;
                if (idx >= end) {
                    // The end of the sample (or of the loop segment).
                    if (v.next >= 0) {
                        const int volume = v.volume, pan = v.pan;
                        const uint32_t pitch = v.pitch;
                        const bool use_pan = v.use_pan;
                        start(v, v.next);
                        v.volume = volume;
                        v.pan = pan;
                        v.pitch = pitch;
                        v.use_pan = use_pan;
                        --i;
                        continue;
                    }
                    if (v.loops_left != 0 && end > loop_from) {
                        if (v.loops_left > 0) --v.loops_left;
                        v.pos -= (end - loop_from) << 32;
                        idx = v.pos >> 32;
                    } else {
                        v.sample = -1;
                        break;
                    }
                }
                float s = (float(f.data[size_t(idx)]) - 128.0f) / 128.0f;
                if (lerp && idx + 1 < size) {
                    const float t = float(uint32_t(v.pos)) / 4294967296.0f;
                    const float s1 = (float(f.data[size_t(idx + 1)]) - 128.0f) / 128.0f;
                    s += (s1 - s) * t;
                }
                out[i * 2] += s * gl;
                out[i * 2 + 1] += s * gr;
                v.pos += step;
                // A sample that will not loop or continue ends with its last byte.
                if ((v.pos >> 32) >= size && v.loops_left == 0 && v.next < 0) v.sample = -1;
            }
        }
    }
    if (music_) {
        const float mg = music_volume.load() * group_volume[size_t(SoundGroup::Music)].load();
        if (mg > 0.0f) music_->mix(out, frames, rate, mg);
    }
    // A soft knee instead of hard clipping when many voices and the music add up.
    for (int i = 0; i < frames * 2; ++i) {
        const float a = std::fabs(out[i]);
        if (a > 0.75f) out[i] = std::copysign(0.75f + 0.25f * std::tanh((a - 0.75f) / 0.25f), out[i]);
    }
}

} // namespace scr
