#include "core/cd_audio.h"

#include "core/gamedata.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

namespace scr {

namespace {

constexpr uint64_t kSector = 2352;
constexpr int kCdRate = 44100;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The formats of the MUSIC folder, best first (the pick when two files are the same track).
constexpr const char* kMusicExtensions[] = {".flac", ".wav", ".ogg", ".mp3"};

int music_extension_rank(const std::filesystem::path& p) {
    const std::string ext = lower(p.extension().string());
    for (int i = 0; i < int(std::size(kMusicExtensions)); ++i)
        if (ext == kMusicExtensions[i]) return i;
    return -1;
}

} // namespace

bool CdImage::open(const std::string& cue_path) {
    tracks.clear();
    std::ifstream in(cue_path);
    if (!in) return false;
    std::string line, file;
    std::vector<std::pair<CdTrack, uint64_t>> found;  // track, INDEX 01 sector
    CdTrack cur;
    bool have = false;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string word;
        ls >> word;
        for (char& c : word) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (word == "FILE") {
            const size_t a = line.find('"'), b = line.rfind('"');
            if (a != std::string::npos && b > a) file = line.substr(a + 1, b - a - 1);
        } else if (word == "TRACK") {
            std::string type;
            cur = CdTrack{};
            ls >> cur.number >> type;
            cur.audio = type == "AUDIO";
            have = true;
        } else if (word == "INDEX" && have) {
            int index = -1;
            std::string msf;
            ls >> index >> msf;
            int m = 0, s = 0, f = 0;
            if (index == 1 && std::sscanf(msf.c_str(), "%d:%d:%d", &m, &s, &f) == 3) {
                found.push_back({cur, uint64_t((m * 60 + s) * 75 + f)});
                have = false;
            }
        }
    }
    if (file.empty() || found.empty()) return false;
    // The FILE as named (any case), else the cue's own name with an image extension.
    const std::filesystem::path cue(cue_path);
    const std::string dir = cue.parent_path().string();
    std::optional<std::string> found_bin = resolve_game_file(dir, file);
    for (const char* ext : {".bin", ".gog", ".img"}) {
        if (found_bin) break;
        found_bin = resolve_game_file(dir, cue.stem().string() + ext);
    }
    if (!found_bin) return false;
    const std::filesystem::path bin(*found_bin);
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(bin, ec);
    if (ec) return false;
    bin_path = bin.string();
    for (size_t i = 0; i < found.size(); ++i) {
        CdTrack t = found[i].first;
        t.offset = found[i].second * kSector;
        const uint64_t end = i + 1 < found.size() ? found[i + 1].second * kSector : size;
        t.length = end > t.offset ? end - t.offset : 0;
        tracks.push_back(t);
    }
    return true;
}

const CdTrack* CdImage::track(int number) const {
    for (const CdTrack& t : tracks)
        if (t.number == number) return &t;
    return nullptr;
}

std::optional<std::string> find_cd_cue(const std::string& game_dir) {
    std::vector<std::string> candidates;
    for (const char* name : {"SCREAM.cue", "SCREAM.DAT"})
        if (auto p = resolve_game_file(game_dir, name)) candidates.push_back(*p);
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(game_dir, ec)) {
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".cue" && e.is_regular_file(ec)) candidates.push_back(e.path().string());
    }
    for (const std::string& c : candidates) {
        // A cue sheet is a small text file; SCREAM.DAT on the GOG/Steam releases is one.
        if (std::filesystem::file_size(c, ec) > 64 * 1024 || ec) continue;
        CdImage probe;
        if (probe.open(c)) return c;
    }
    return std::nullopt;
}

int music_file_track(const std::string& file_name) {
    const std::string name = lower(std::filesystem::path(file_name).stem().string());
    const auto digit = [&](size_t i) { return i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])); };
    // the number of 1..2 digits at i (not part of a longer number), else 0
    const auto number_at = [&](size_t i) {
        if (!digit(i) || (i > 0 && digit(i - 1))) return 0;
        size_t e = i;
        while (digit(e)) ++e;
        if (e - i > 2) return 0;
        const int n = std::stoi(name.substr(i, e - i));
        return n >= 1 && n <= 99 ? n : 0;
    };
    size_t start = 0;
    while (start < name.size() && name[start] == ' ') ++start;
    if (int n = number_at(start)) return n;
    for (size_t at = name.find("track"); at != std::string::npos; at = name.find("track", at + 1)) {
        size_t i = at + 5;
        while (i < name.size() && (name[i] == ' ' || name[i] == '_' || name[i] == '-' || name[i] == '.' || name[i] == '#')) ++i;
        if (int n = number_at(i)) return n;
    }
    for (size_t i = 0; i < name.size(); ++i) {
        if (!digit(i) || (i > 0 && std::isalpha(static_cast<unsigned char>(name[i - 1])))) continue;
        size_t e = i;
        while (digit(e)) ++e;
        if (e < name.size() && std::isalpha(static_cast<unsigned char>(name[e]))) {
            i = e;
            continue;
        }
        if (int n = number_at(i)) return n;
        i = e;
    }
    return 0;
}

std::map<int, std::filesystem::path> find_music_files(const std::string& game_dir) {
    namespace fs = std::filesystem;
    std::map<int, fs::path> files;
    std::error_code ec;
    fs::path folder;
    for (const auto& e : fs::directory_iterator(game_dir, ec))
        if (lower(e.path().filename().string()) == "music" && e.is_directory(ec)) folder = e.path();
    if (folder.empty()) return files;
    std::vector<fs::path> found;
    for (const auto& e : fs::directory_iterator(folder, ec))
        if (e.is_regular_file(ec) && music_extension_rank(e.path()) >= 0) found.push_back(e.path());
    std::sort(found.begin(), found.end());  // (the same pick whatever order the folder lists)
    for (const fs::path& p : found) {
        const int track = music_file_track(p.filename().string());
        if (track == 0) continue;
        auto it = files.find(track);
        if (it == files.end() || music_extension_rank(p) < music_extension_rank(it->second)) files[track] = p;
    }
    // Track 1 is the data track: files from 1 count the audio tracks only, so 1 is CD track 2.
    if (files.count(1)) {
        std::map<int, fs::path> moved;
        for (auto& [track, p] : files)
            if (track < 99) moved[track + 1] = p;
        files.swap(moved);
    }
    return files;
}

// ---- AudioFilePlayer ----------------------------------------------------------------------

struct AudioFilePlayer::Decoder {
    virtual ~Decoder() = default;
    // Up to `frames` frames of `channels` interleaved floats; returns the frames read (0 at the end).
    virtual size_t read(float* out, size_t frames) = 0;
    virtual bool rewind() = 0;
    int channels = 0;
    int rate = 0;
};

namespace {

// The decoders' file openers taking the path as Windows stores it (wide: names in any language).
#ifdef _WIN32
#define SCR_DR_OPEN(fn) fn##_w
#else
#define SCR_DR_OPEN(fn) fn
#endif

struct Mp3Decoder : AudioFilePlayer::Decoder {
    drmp3 mp3{};
    bool open = false;
    explicit Mp3Decoder(const std::filesystem::path& p) {
        open = SCR_DR_OPEN(drmp3_init_file)(&mp3, p.c_str(), nullptr) != 0;
        if (open) {
            channels = int(mp3.channels);
            rate = int(mp3.sampleRate);
        }
    }
    ~Mp3Decoder() override {
        if (open) drmp3_uninit(&mp3);
    }
    size_t read(float* out, size_t frames) override { return size_t(drmp3_read_pcm_frames_f32(&mp3, frames, out)); }
    bool rewind() override { return drmp3_seek_to_pcm_frame(&mp3, 0) != 0; }
};

struct FlacDecoder : AudioFilePlayer::Decoder {
    drflac* flac = nullptr;
    explicit FlacDecoder(const std::filesystem::path& p) {
        flac = SCR_DR_OPEN(drflac_open_file)(p.c_str(), nullptr);
        if (flac) {
            channels = int(flac->channels);
            rate = int(flac->sampleRate);
        }
    }
    ~FlacDecoder() override {
        if (flac) drflac_close(flac);
    }
    size_t read(float* out, size_t frames) override { return size_t(drflac_read_pcm_frames_f32(flac, frames, out)); }
    bool rewind() override { return drflac_seek_to_pcm_frame(flac, 0) != 0; }
};

struct WavDecoder : AudioFilePlayer::Decoder {
    drwav wav{};
    bool open = false;
    explicit WavDecoder(const std::filesystem::path& p) {
        open = SCR_DR_OPEN(drwav_init_file)(&wav, p.c_str(), nullptr) != 0;
        if (open) {
            channels = int(wav.channels);
            rate = int(wav.sampleRate);
        }
    }
    ~WavDecoder() override {
        if (open) drwav_uninit(&wav);
    }
    size_t read(float* out, size_t frames) override { return size_t(drwav_read_pcm_frames_f32(&wav, frames, out)); }
    bool rewind() override { return drwav_seek_to_pcm_frame(&wav, 0) != 0; }
};

struct VorbisDecoder : AudioFilePlayer::Decoder {
    stb_vorbis* vorbis = nullptr;
    explicit VorbisDecoder(const std::filesystem::path& p) {
#ifdef _WIN32
        std::FILE* f = _wfopen(p.c_str(), L"rb");
#else
        std::FILE* f = std::fopen(p.c_str(), "rb");
#endif
        if (!f) return;
        int error = 0;
        vorbis = stb_vorbis_open_file(f, 1, &error, nullptr);  // (closes f when done, or on failure)
        if (!vorbis) return;
        const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        channels = info.channels;
        rate = int(info.sample_rate);
    }
    ~VorbisDecoder() override {
        if (vorbis) stb_vorbis_close(vorbis);
    }
    size_t read(float* out, size_t frames) override {
        return size_t(stb_vorbis_get_samples_float_interleaved(vorbis, channels, out, int(frames) * channels));
    }
    bool rewind() override { return stb_vorbis_seek_start(vorbis) != 0; }
};

} // namespace

AudioFilePlayer::AudioFilePlayer(const std::filesystem::path& file, bool loop) : loop_(loop) {
    std::unique_ptr<Decoder> d;
    switch (music_extension_rank(file)) {
    case 0: d = std::make_unique<FlacDecoder>(file); break;
    case 1: d = std::make_unique<WavDecoder>(file); break;
    case 2: d = std::make_unique<VorbisDecoder>(file); break;
    case 3: d = std::make_unique<Mp3Decoder>(file); break;
    default: return;
    }
    if (d->channels < 1 || d->rate < 1000) return;
    rate_ = d->rate;
    decoder_ = std::move(d);
}

AudioFilePlayer::~AudioFilePlayer() = default;

bool AudioFilePlayer::read_frame(float& l, float& r) {
    if (buffer_pos_ + 2 > buffer_.size()) {
        constexpr size_t kChunk = 4096;
        const int ch = decoder_->channels;
        std::vector<float> raw(kChunk * size_t(ch));
        size_t got = decoder_->read(raw.data(), kChunk);
        if (got == 0) {
            if (!loop_ || !decoder_->rewind()) return false;
            got = decoder_->read(raw.data(), kChunk);
            if (got == 0) return false;
        }
        // to stereo: mono on both sides; of more channels, the front left and right
        buffer_.resize(got * 2);
        for (size_t i = 0; i < got; ++i) {
            buffer_[i * 2] = raw[i * size_t(ch)];
            buffer_[i * 2 + 1] = raw[i * size_t(ch) + (ch > 1 ? 1 : 0)];
        }
        buffer_pos_ = 0;
    }
    l = buffer_[buffer_pos_];
    r = buffer_[buffer_pos_ + 1];
    buffer_pos_ += 2;
    return true;
}

void AudioFilePlayer::mix(float* out, int frames, int rate, float gain) {
    if (!decoder_ || rate <= 0) return;
    if (!primed_) {
        if (!read_frame(prev_[0], prev_[1]) || !read_frame(next_[0], next_[1])) {
            decoder_.reset();
            return;
        }
        primed_ = true;
    }
    const double step = double(rate_) / double(rate);
    for (int i = 0; i < frames; ++i) {
        const float t = float(phase_);
        out[i * 2] += (prev_[0] + (next_[0] - prev_[0]) * t) * gain;
        out[i * 2 + 1] += (prev_[1] + (next_[1] - prev_[1]) * t) * gain;
        phase_ += step;
        while (phase_ >= 1.0) {
            phase_ -= 1.0;
            prev_[0] = next_[0];
            prev_[1] = next_[1];
            if (!read_frame(next_[0], next_[1])) {
                decoder_.reset();
                return;
            }
        }
    }
}

// ---- CdAudio ------------------------------------------------------------------------------

bool CdAudio::open(const std::string& game_dir) {
    files = find_music_files(game_dir);
    image.reset();
    if (auto cue = find_cd_cue(game_dir)) {
        CdImage img;
        if (img.open(*cue)) image = std::move(img);
    }
    return first_track() != 0;
}

std::shared_ptr<AudioSource> CdAudio::play(int track, bool loop) const {
    if (auto it = files.find(track); it != files.end()) {
        auto p = std::make_shared<AudioFilePlayer>(it->second, loop);
        if (p->ok()) return p;
    }
    if (image) {
        auto p = std::make_shared<CdAudioPlayer>(*image, track, loop);
        if (p->ok()) return p;
    }
    return nullptr;
}

int CdAudio::first_track() const {
    int first = files.empty() ? 0 : files.begin()->first;
    if (image)
        for (const CdTrack& t : image->tracks)
            if (t.audio && (first == 0 || t.number < first)) first = t.number;
    return first;
}

int CdAudio::last_track() const {
    int last = files.empty() ? 0 : files.rbegin()->first;
    if (image)
        for (const CdTrack& t : image->tracks)
            if (t.audio) last = std::max(last, t.number);
    return last;
}

int cd_track_for_race(int map_index) {
    static constexpr int kTable[8] = {2, 3, 2, 4, 5, 2, 4, 5};  // 0x101efc
    const int i = std::clamp(map_index, 0, 5) + 2;
    return kTable[i] + 1;
}

CdAudioPlayer::CdAudioPlayer(const CdImage& image, int track, bool loop) : loop_(loop) {
    const CdTrack* t = image.track(track);
    if (!t || !t->audio || t->length < 4) return;
    track_ = *t;
    file_ = std::fopen(image.bin_path.c_str(), "rb");
    if (file_) std::fseek(file_, 0, SEEK_SET);
}

CdAudioPlayer::~CdAudioPlayer() {
    if (file_) std::fclose(file_);
}

bool CdAudioPlayer::read_frame(float& l, float& r) {
    if (buffer_pos_ + 2 > buffer_.size()) {
        if (pos_ >= track_.length) {
            if (!loop_) return false;
            pos_ = 0;
        }
        const uint64_t want = std::min<uint64_t>(track_.length - pos_, 64 * 1024) & ~uint64_t(3);
        if (want == 0) return false;
        buffer_.resize(size_t(want / 2));
        if (_fseeki64(file_, int64_t(track_.offset + pos_), SEEK_SET) != 0) return false;
        const size_t got = std::fread(buffer_.data(), 1, size_t(want), file_);
        if (got < 4) return false;
        buffer_.resize(got / 4 * 2);
        buffer_pos_ = 0;
        pos_ += got / 4 * 4;
    }
    l = float(buffer_[buffer_pos_]) / 32768.0f;
    r = float(buffer_[buffer_pos_ + 1]) / 32768.0f;
    buffer_pos_ += 2;
    return true;
}

void CdAudioPlayer::mix(float* out, int frames, int rate, float gain) {
    if (!file_ || rate <= 0) return;
    if (!primed_) {
        if (!read_frame(prev_[0], prev_[1]) || !read_frame(next_[0], next_[1])) return;
        primed_ = true;
    }
    const double step = double(kCdRate) / double(rate);
    for (int i = 0; i < frames; ++i) {
        const float t = float(phase_);
        out[i * 2] += (prev_[0] + (next_[0] - prev_[0]) * t) * gain;
        out[i * 2 + 1] += (prev_[1] + (next_[1] - prev_[1]) * t) * gain;
        phase_ += step;
        while (phase_ >= 1.0) {
            phase_ -= 1.0;
            prev_[0] = next_[0];
            prev_[1] = next_[1];
            if (!read_frame(next_[0], next_[1])) {
                std::fclose(file_);
                file_ = nullptr;
                return;
            }
        }
    }
}

} // namespace scr
