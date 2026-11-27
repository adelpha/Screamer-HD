#pragma once
// The game's music is CD audio (cd_play_track over MSCDEX). We play it from a disc image:
// a .cue sheet with one BINARY file of 2352-byte sectors (red book audio: 44.1 kHz 16-bit
// stereo, little endian). Track 1 is data; the music is tracks 2..8.
//
// The image is found in the game directory as SCREAM.cue / SCREAM.bin, or under the names the
// Steam and GOG releases use: SCREAM.DAT (the cue sheet) and SCREAM.gog (the image). A cue sheet
// whose FILE is missing falls back to an image with the cue's name and .bin/.gog/.img.
//
// Instead of (or as well as) an image, the tracks can be audio files ripped from the CD in a
// MUSIC folder in the game directory (find_music_files): MP3, FLAC, Ogg Vorbis or WAV, named by
// CD track number (music_file_track). A track found there wins over the image's; tracks missing
// from the folder still come from the image (CdAudio).
//
// Which track plays (table at 0x101efc, cd_play_track(table[i] + 1)):
//   menu: 4, attract mode: 3, race on map n (0-based): table[n + 2] + 1 -> 3 5 6 3 5 6.

#include "core/sound.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
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

// The CD track number a ripped track's file name stands for, 0 if none. In order:
//   - a number at the start: "02 - Title.flac", "02. Title.mp3", "2.ogg"
//   - a number after "track": "Track02.mp3", "track02.cdda.wav", "Screamer - Track 2.flac"
//   - otherwise the first number of one or two digits standing on its own: "Screamer - 02 - Title.mp3"
// (only 1..99; a number such as a year, 1995, is passed over)
int music_file_track(const std::string& file_name);

// The ripped tracks in <game_dir>/MUSIC (any case): CD track number -> file. Only .mp3, .flac,
// .ogg and .wav files count. The CD's track 1 is data, so its music starts at track 2; when the
// files are numbered from 1 (a rip that numbered the audio tracks only), they are moved up by one.
// Two files for one track: FLAC, then WAV, then Ogg, then MP3.
std::map<int, std::filesystem::path> find_music_files(const std::string& game_dir);

// Streams one audio file (MP3, FLAC, Ogg Vorbis or WAV, by its extension) into the mix, looping
// like cd_play_track(track, 1), resampled from the file's own rate to the mix's.
class AudioFilePlayer : public AudioSource {
public:
    explicit AudioFilePlayer(const std::filesystem::path& file, bool loop = true);
    ~AudioFilePlayer() override;
    bool ok() const { return decoder_ != nullptr; }
    int rate() const { return rate_; }
    void mix(float* out, int frames, int rate, float gain) override;

    struct Decoder;  // (the decoder of the file's format)

private:
    bool read_frame(float& l, float& r);
    std::unique_ptr<Decoder> decoder_;
    int rate_ = 0;
    bool loop_ = true;
    std::vector<float> buffer_;  // stereo frames
    size_t buffer_pos_ = 0;
    double phase_ = 0;
    float prev_[2]{}, next_[2]{};
    bool primed_ = false;
};

// All the CD audio there is: the ripped tracks of the MUSIC folder and/or a CD image.
class CdAudio {
public:
    // Looks for both in the game directory; true when either has an audio track.
    bool open(const std::string& game_dir);
    // The player for a track (from the MUSIC folder if it has it, else the image), or null.
    std::shared_ptr<AudioSource> play(int track, bool loop = true) const;
    // The lowest and highest audio track (0 when there are none).
    int first_track() const;
    int last_track() const;
    std::optional<CdImage> image;
    std::map<int, std::filesystem::path> files;
};

} // namespace scr
