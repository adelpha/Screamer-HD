#pragma once
// The race's sounds and the announcer, as STARTL.EXE drives them from inside race_frame:
//
//   race_load                  the player's engine starts (sample by surface: automoto on the
//                              road, autogall off it; taz3/taz4 for car 12)
//   car_apply_surface          a new surface switches the engine sample (sfx_play_continue)
//   car_update_engine_gearbox  engine pitch = rpm * 14 + 32768
//   car_update_skid            skidfx while braking hard or drifting
//   car_update_crash_state     dentone and "crash" when the car flips
//   car_update_track_heading   wallh1 when scraping a wall
//   car_resolve_cpu_collision  crash on contact with a CPU car
//   opponent_update_engine_sound  up to 3 CPU engines (oppmotor), nearest first, with doppler,
//                              distance volume and pan from the listener (the player's car)
//   race_countdown_update      "three", "two", "one", "go" (speech_play_event)
//   speech_check_events        wrong way, stopped, hurry, taking/losing the lead, bend ahead,
//                              skidding, falling behind, lapping someone
//   speech_update              plays the queued lines one at a time, 0.7 s apart
//   race_on_finished / race_on_failed  the engine stops; "you win" / "you lose" / "no time"
//
// The announcer table (0x101fb0) maps 27 events to up to three SPFX samples; variant 3 cycles
// through them. Without SPFXFULL only the first of each is used.

#include "core/animations.h"
#include "core/opponent.h"
#include "core/sound.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scr {

class RaceAudio {
public:
    // race_load. car_index: the player's car (12 has its own engine samples).
    void start(Mixer& mixer, const SoundBank& bank, const Race& race, int car_index, uint32_t frame_counter,
               bool speech = true);
    // After Race::update, once per frame.
    void update(const Race& race, float frame_time);
    // sfx_update_positional for the moving scenery (after TrackAnimations::update, same frame).
    void animations(const TrackAnimations& anims);
    void stop();

    // Announcer state (for tests).
    int speech_current() const { return queue_.empty() ? -1 : queue_.front().event; }
    int speech_handle() const { return speech_handle_; }

private:
    struct Queued {
        int event, variant;
        float delay;
    };
    int engine_sample(int surface) const;
    void queue_add(int event, int variant, float delay);
    int pick_sample(int event, int variant);
    int play_event(int event, int variant);
    bool trigger_once_after(bool cond, int event, float time, float dt);
    bool trigger_repeat_after(bool cond, int event, float time, float dt);
    bool say_once(int event, int variant);
    void check_events(const Race& race, float dt);
    void speech_update(float dt);
    void cpu_engines(const Race& race);

    Mixer* mixer_ = nullptr;
    const SoundBank* bank_ = nullptr;
    bool speech_ = false;
    int car_index_ = 0;
    int engine_ = -1, skid_ = -1, wall_ = -1;
    int surface_ = 0;
    std::vector<int> cpu_engine_;     // CpuCar 0x4ec
    std::vector<int> cpu_prev_dist_;  // 0x4f0
    std::vector<float> cpu_rev_;      // 0x4f4 engine speed for the sound
    int cpu_voices_ = 0;              // DAT_00113cc4
    int listener_x_ = 0, listener_z_ = 0;
    float listener_yaw_ = 0;
    bool have_listener_ = false;
    int prev_listener_x_ = 0, prev_listener_z_ = 0;
    float prev_listener_yaw_ = 0;
    int countdown_stage_ = -2;
    bool finished_ = false, failed_ = false;
    float countdown_time_ = 0;
    // speech_*
    std::vector<Queued> queue_;
    int speech_handle_ = -1, speech_sample_ = -1;
    float speech_wait_ = 0.7f;
    std::array<float, 27> timers_{};
    std::array<int, 27> variant_{};
    bool had_lead_ = false;
    std::vector<int> anim_sfx_, anim_prev_dist_;  // AnimationDef runtime +0x20, +0x24
};

} // namespace scr
