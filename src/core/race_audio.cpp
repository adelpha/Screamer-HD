#include "core/race_audio.h"

#include <algorithm>
#include <cmath>

namespace scr {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717958f;
constexpr float kSpeechGap = 0.7f;  // 0x3f333333: the delay every queued line waits for

// Base samples (SFXSBASE.DTR).
enum : int {
    kSkidFx = 0, kCrash = 1, kOppMotor = 2, kAutoMoto = 3, kWall = 4, kDentOne = 8, kTaz3 = 10,
};

// The announcer table at 0x101fb0: three SPFX sample indices per event, -1 none.
constexpr int kSpeech[27][3] = {
    {2, -1, -1},   {3, -1, -1},   {4, -1, -1},   {5, -1, -1},    // 0-3 three, two, one, go
    {8, 13, 14},   {6, 48, 7},    {-1, -1, -1},  {46, 47, 53},   // 4 checkpoint, 5 lap, 7 lap record
    {44, 45, -1},  {41, 42, 43},  {21, 51, -1},  {1, 11, 12},    // 8 beat, 9 crash, 10 skid, 11 win
    {0, 27, 52},   {17, 18, 54},  {15, 16, -1},  {25, 26, -1},   // 12 lose, 13 bad, 14 well, 15 hit car
    {20, 49, -1},  {19, 50, -1},  {-1, -1, -1},  {55, 56, 57},   // 16 wrong way, 17 stopped, 19 cones
    {22, 23, 24},  {31, 32, 33},  {38, 39, 40},  {37, -1, -1},   // 20 bend, 21 lead, 22 hurry, 23 lapped
    {34, 35, 36},  {9, 30, -1},   {10, 28, 29},                  // 24 lost lead, 25 no time, 26 all
};


// angle_within: |a - b| wrapped into 0..2pi is within tol of 0 or 2pi.
bool angle_within(float a, float b, float tol) {
    float d = std::fabs(a - b);
    while (kTwoPi < d) d += -kTwoPi;
    return d <= tol || kTwoPi - tol <= d;
}

} // namespace

int RaceAudio::engine_sample(int surface) const {
    // 0x101d78: {automoto, autogall} and {taz3, taz4} for car 12, by surface.
    static constexpr int kEngine[4] = {kAutoMoto, 7, kTaz3, 11};
    const int i = std::clamp(surface, 0, 1) + (car_index_ == 12 ? 2 : 0);
    return kEngine[i];
}

void RaceAudio::start(Mixer& mixer, const SoundBank& bank, const Race& race, int car_index, uint32_t frame_counter,
                      bool speech) {
    stop();
    mixer_ = &mixer;
    bank_ = &bank;
    car_index_ = car_index;
    speech_ = speech && bank.speech_first >= 0 && bank.num_speech > 0;
    // speech_init
    queue_.clear();
    speech_handle_ = speech_sample_ = -1;
    speech_wait_ = kSpeechGap;
    timers_.fill(0.0f);
    variant_.fill((frame_counter & 2) == 0 ? 2 : int(frame_counter & 1));
    had_lead_ = false;
    countdown_stage_ = -2;
    countdown_time_ = 0;
    finished_ = race.finished;
    failed_ = race.failed;
    surface_ = race.player.marker_flags;
    engine_ = mixer.play(engine_sample(surface_));
    cpu_engine_.assign(race.opponents.size(), -1);
    cpu_prev_dist_.assign(race.opponents.size(), 0);
    cpu_rev_.assign(race.opponents.size(), 0.0f);
    cpu_voices_ = 0;
    have_listener_ = false;
}

void RaceAudio::stop() {
    if (mixer_) mixer_->stop_all();
    engine_ = skid_ = wall_ = -1;
    cpu_engine_.clear();
    cpu_voices_ = 0;
    anim_sfx_.clear();
    anim_prev_dist_.clear();
    queue_.clear();
    speech_handle_ = -1;
}

void RaceAudio::queue_add(int event, int variant, float delay) {
    if (!speech_) return;
    for (const Queued& q : queue_)
        if (q.event == event) return;
    queue_.push_back({event, variant, delay});
}

int RaceAudio::pick_sample(int event, int variant) {
    if (event < 0 || event >= 27) return -1;
    int s = -1;
    if (variant >= 0 && variant < 3) {
        s = kSpeech[event][variant];
    } else if (bank_->speech_full) {
        // speech_cycle_variant
        int tries = 0;
        do {
            if (++variant_[size_t(event)] == 3) variant_[size_t(event)] = 0;
        } while (kSpeech[event][variant_[size_t(event)]] == -1 && ++tries < 3);
        s = kSpeech[event][variant_[size_t(event)]];
    } else {
        s = kSpeech[event][0];
    }
    if (!bank_->speech_full) {
        if (bank_->num_speech <= s) s = kSpeech[event][0];
        if (bank_->num_speech <= s) s = -1;
    }
    return s;
}

int RaceAudio::play_event(int event, int variant) {
    // speech_play_event: straight away, not through the queue.
    if (!speech_) return -1;
    const int s = pick_sample(event, variant);
    return s < 0 ? -1 : mixer_->play(s + bank_->speech_first);
}

bool RaceAudio::trigger_once_after(bool cond, int event, float time, float dt) {
    float& t = timers_[size_t(event)];
    if (!cond) {
        t = 0.0f;
    } else if (0.0f <= t) {
        t = dt + t;
        if (time <= t) {
            queue_add(event, 3, kSpeechGap);
            t = -1.0f;
            return true;
        }
    }
    return false;
}

bool RaceAudio::trigger_repeat_after(bool cond, int event, float time, float dt) {
    float& t = timers_[size_t(event)];
    if (!cond) {
        t = 0.0f;
        return false;
    }
    t = dt + t;
    if (time <= t) {
        queue_add(event, 3, kSpeechGap);
        t = 0.0f;
        return true;
    }
    return false;
}

bool RaceAudio::say_once(int event, int variant) {
    float& t = timers_[size_t(event)];
    if (t == -1.0f) return false;
    queue_add(event, variant, kSpeechGap);
    t = -1.0f;
    return true;
}

void RaceAudio::check_events(const Race& race, float dt) {
    // speech_check_events (races against the CPU cars: game modes 1 and 2).
    const Car& car = race.player;
    const bool wrong_way = angle_within(car.track_heading + kPi, car.yaw, 0.7853982f);
    trigger_repeat_after(wrong_way, 0x10, 5.0f, dt);
    trigger_repeat_after(car.speed < 1000.0f, 0x11, 8.0f, dt);
    if (race.time_left <= 5.0f) say_once(0x16, 3);
    const int position = race.player_position;
    if (trigger_once_after(position == 1, 0x15, 5.0f, dt)) had_lead_ = true;
    if (trigger_once_after(had_lead_ && 1 < position, 0x18, 5.0f, dt)) had_lead_ = false;
    if (!wrong_way) trigger_once_after((race.player_cue & 4) != 0, 0x14, 0.0f, dt);
    trigger_once_after(int(car.skid) != 0, 0x0a, float(double(car.setup.grip) * 2.9), dt);
    const int expected = std::max(2, int(race.opponents.size()));  // PlayerCar 0x788
    trigger_once_after(expected < position, 0x0d, 8.0f, dt);
    bool lapping = false;
    for (const Opponent& o : race.opponents) {
        if ((o.lap < car.lap && o.track_progress - float(o.lap * 100) < car.track_progress - float(car.lap * 100)) ||
            1 < car.lap - o.lap) {
            lapping = true;
            break;
        }
    }
    trigger_once_after(lapping, 0x17, 2.0f, dt);
}

void RaceAudio::speech_update(float dt) {
    if (!speech_) return;
    if (speech_handle_ == -1) {
        speech_wait_ = dt + speech_wait_;
        if (queue_.empty()) return;
        // speech_queue_pick_next: a line queued without a delay goes first.
        size_t pick = 0;
        float least = 0.01f;
        for (size_t i = 0; i < queue_.size(); ++i) {
            if (queue_[i].delay < least) {
                least = queue_[i].delay;
                pick = i;
            }
        }
        std::swap(queue_[0], queue_[pick]);
        if (queue_[0].delay <= speech_wait_) {
            const int s = pick_sample(queue_[0].event, queue_[0].variant);
            if (s != -1) {
                speech_sample_ = s + bank_->speech_first;
                speech_handle_ = mixer_->play(speech_sample_);
            }
            if (speech_handle_ == -1) {
                queue_.erase(queue_.begin());
                speech_wait_ = 0.0f;
            }
        }
    } else if (mixer_->sample_of(speech_handle_) != speech_sample_) {
        // sfx_is_done
        queue_.erase(queue_.begin());
        speech_handle_ = -1;
        speech_wait_ = 0.0f;
    }
}

void RaceAudio::cpu_engines(const Race& race) {
    // opponent_update_engine_sound for each CPU car.
    for (size_t i = 0; i < race.opponents.size() && i < cpu_engine_.size(); ++i) {
        const Opponent& o = race.opponents[i];
        int& handle = cpu_engine_[i];
        if (o.active && handle == -1) {
            int stolen = -1;
            if (cpu_voices_ == 3) {
                // Take the engine of a car further away.
                for (size_t j = 0; j < race.opponents.size(); ++j) {
                    if (j == i || cpu_engine_[j] == -1) continue;
                    if (o.viewer_distance < race.opponents[j].viewer_distance) {
                        stolen = cpu_engine_[j];
                        cpu_engine_[j] = -1;
                        --cpu_voices_;
                        break;
                    }
                }
            }
            if (cpu_voices_ < 3) {
                cpu_prev_dist_[i] = int(std::trunc(o.viewer_distance));
                handle = stolen >= 0 ? stolen : mixer_->play(kOppMotor);
                ++cpu_voices_;
            }
        }
        if (handle == -1) continue;
        if (!o.active) {
            handle = mixer_->stop(handle);
            --cpu_voices_;
            continue;
        }
        if (race.started) cpu_rev_[i] = o.speed;
        int lx, lz;
        rotate_xz(lx, lz, listener_x_ - o.pos_x, listener_z_ - o.pos_z, listener_yaw_);
        const float d = std::max(o.viewer_distance, 1.0f);
        int pitch = int(std::trunc((float(cpu_prev_dist_[i]) - o.viewer_distance) * 9.0f + double(cpu_rev_[i]) * 2.4 + 13568.0));
        // Before the start the revs swing through negative values; the driver would take those as
        // huge unsigned pitches, so they play as their magnitude here.
        pitch = std::max(std::abs(pitch), 1);
        const float closeness = (35000.0f - o.viewer_distance) / 35000.0f;
        const int volume = int(std::trunc(closeness * closeness * 23552.0f));
        const int pan = int(std::trunc(std::sqrt(std::fabs(double(lx) / d)) * 32767.0 * (lx < 0 ? -1.0 : 1.0))) + 0x8000;
        cpu_prev_dist_[i] = int(std::trunc(o.viewer_distance));
        mixer_->set_pitch(handle, pitch);
        mixer_->set_volume(handle, volume);
        mixer_->set_pan(handle, pan);
    }
}

void RaceAudio::animations(const TrackAnimations& anims) {
    if (!mixer_ || !bank_) return;
    if (anim_sfx_.size() != anims.list.size()) {
        anim_sfx_.assign(anims.list.size(), -1);
        anim_prev_dist_.assign(anims.list.size(), 0);
    }
    for (size_t i = 0; i < anims.list.size(); ++i) {
        const TrackAnimation& a = anims.list[i];
        // Only the animations animations_update moved this frame.
        if (std::abs(prev_listener_x_ - a.x) >= 52000 || std::abs(prev_listener_z_ - a.z) >= 52000) continue;
        int& handle = anim_sfx_[i];
        const int dx = (a.x - prev_listener_x_) >> 3, dz = (a.z - prev_listener_z_) >> 3;
        const float d = float(std::sqrt(double(dx * dx + dz * dz)) * 8.0);
        if (handle == -1) {
            if (d < 40000.0f) {
                anim_prev_dist_[i] = int(std::trunc(d));
                handle = mixer_->play(bank_->num_base + a.sound_fx);
            }
        } else if (40000.0f < d) {
            handle = mixer_->stop(handle);
        }
        if (handle == -1) continue;
        int lx, lz;
        rotate_xz(lx, lz, prev_listener_x_ - a.x, prev_listener_z_ - a.z, prev_listener_yaw_);
        const float closeness = (40000.0f - d) / 40000.0f;
        const int pitch = int(std::trunc((float(anim_prev_dist_[i]) - d) * 9.0f + 65536.0f));
        const int volume = int(std::trunc(closeness * closeness * 32767.0f));
        const float dd = std::max(d, 1.0f);
        const int pan = int(std::trunc(std::sqrt(std::fabs(double(lx) / dd)) * 32767.0 * (lx < 0 ? -1.0 : 1.0))) + 0x8000;
        anim_prev_dist_[i] = int(std::trunc(d));
        mixer_->set_pitch(handle, std::max(pitch, 1));
        mixer_->set_volume(handle, volume);
        mixer_->set_pan(handle, pan);
    }
}

void RaceAudio::update(const Race& race, float frame_time) {
    if (!mixer_ || !bank_) return;
    const Car& car = race.player;
    if (!have_listener_) {
        listener_x_ = car.pos_x;
        listener_z_ = car.pos_z;
        listener_yaw_ = car.yaw;
        have_listener_ = true;
    }
    const bool over = race.finished || race.failed;
    // The player's car.
    if (engine_ != -1) {
        if (car.marker_flags != surface_) mixer_->continue_with(engine_, engine_sample(car.marker_flags));
        mixer_->set_pitch(engine_, int(std::trunc(car.engine_rpm * 14.0f + 32768.0f)));
    }
    surface_ = car.marker_flags;
    if (car.skid_sound && !over) {
        if (skid_ == -1 || !mixer_->playing(skid_)) skid_ = mixer_->play(kSkidFx);
    } else if (skid_ != -1 && (car.crash_state < 3 || over)) {
        skid_ = mixer_->stop(skid_);
    }
    if (car.crash_sound) {
        mixer_->play(kDentOne);
        queue_add(9, 3, 0.5f);
    }
    if (car.wall_sound && mixer_->sample_of(wall_) != kWall) wall_ = mixer_->play(kWall);
    if (race.cpu_hit_sound) mixer_->play(kCrash);
    // The CPU cars, heard from where the player was at the end of the last frame.
    cpu_engines(race);
    if (!race.started) {
        // race_countdown_update: the CPU engines rev while waiting.
        countdown_time_ += frame_time;
        for (size_t i = 0; i < cpu_rev_.size(); ++i)
            cpu_rev_[i] = float(std::sin(double(float(i) * 0.5f * countdown_time_)) * 15000.0);
    }
    // Announcer lines from the race (checkpoints, laps).
    for (const Race::SpeechCue& c : race.speech_cues) {
        if (c.event < 0) timers_[size_t(-c.event)] = 0.0f;  // speech_once_reset
        else queue_add(c.event, c.variant, c.delay);
    }
    if (race.started && !over) check_events(race, frame_time);
    // race_countdown_update: "three", "two", "one", "go".
    if (race.countdown_stage >= 0 && race.countdown_stage != countdown_stage_) play_event(race.countdown_stage, 0);
    countdown_stage_ = race.countdown_stage;
    speech_update(frame_time);
    // race_on_finished / race_on_failed
    if (race.finished && !finished_) {
        engine_ = mixer_->stop(engine_);
        queue_add(race.player_position == 1 ? 0x0b : 0x0c, 3, 0.0f);
    }
    if (race.failed && !failed_) {
        engine_ = mixer_->stop(engine_);
        queue_add(0x19, 3, 0.0f);
    }
    finished_ = race.finished;
    failed_ = race.failed;
    // camera_set_listener (animations() still uses the one from the last frame)
    prev_listener_x_ = listener_x_;
    prev_listener_z_ = listener_z_;
    prev_listener_yaw_ = listener_yaw_;
    listener_x_ = car.pos_x;
    listener_z_ = car.pos_z;
    listener_yaw_ = car.yaw;
}

} // namespace scr
