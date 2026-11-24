#include "core/cd_audio.h"

#include "core/gamedata.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace scr {

namespace {

constexpr uint64_t kSector = 2352;
constexpr int kCdRate = 44100;

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
