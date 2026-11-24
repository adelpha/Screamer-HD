#pragma once
// The game's music is CD audio (cd_play_track over MSCDEX). We play it from a disc image:
// a .cue sheet with one BINARY file of 2352-byte sectors (red book audio: 44.1 kHz 16-bit
// stereo, little endian). Track 1 is data; the music is tracks 2..8.
//
// The image is found in the game directory as SCREAM.cue / SCREAM.bin, or under the names the
// Steam and GOG releases use: SCREAM.DAT (the cue sheet) and SCREAM.gog (the image). A cue sheet
// whose FILE is missing falls back to an image with the cue's name and .bin/.gog/.img.
//
// Which track plays (table at 0x101efc, cd_play_track(table[i] + 1)):
//   menu: 4, attract mode: 3, race on map n (0-based): table[n + 2] + 1 -> 3 5 6 3 5 6.

#include "core/sound.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace scr {

struct CdTrack {
    int number = 0;
    bool audio = false;
    uint64_t offset = 0;  // bytes into the image
    uint64_t length = 0;  // bytes
};

class CdImage {
public:
    // Parses the cue sheet; the image file is found next to it.
    bool open(const std::string& cue_path);
    const CdTrack* track(int number) const;
    std::vector<CdTrack> tracks;
    std::string bin_path;
};

// The cue sheet of the CD image in the game directory, if any.
std::optional<std::string> find_cd_cue(const std::string& game_dir);

// CD track for a race on map `map_index` (0 = MAP1) and for the menu.
int cd_track_for_race(int map_index);
constexpr int kCdTrackMenu = 4;

// Streams one track from the image into the mix, looping like cd_play_track(track, 1).
class CdAudioPlayer : public AudioSource {
public:
    CdAudioPlayer(const CdImage& image, int track, bool loop = true);
    ~CdAudioPlayer() override;
    bool ok() const { return file_ != nullptr; }
    void mix(float* out, int frames, int rate, float gain) override;

private:
    bool read_frame(float& l, float& r);
    std::FILE* file_ = nullptr;
    CdTrack track_;
    bool loop_ = true;
    uint64_t pos_ = 0;  // bytes into the track
    std::vector<int16_t> buffer_;
    size_t buffer_pos_ = 0;
    double phase_ = 0;  // resampling position between prev_ and next_
    float prev_[2]{}, next_[2]{};
    bool primed_ = false;
};

} // namespace scr
