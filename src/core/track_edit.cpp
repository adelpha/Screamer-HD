#include "core/track_edit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>

namespace scr {
namespace {

std::array<float, 3> turn(const Mat3& r, float x, float y, float z) {
    return {r.m[0][0] * x + r.m[0][1] * y + r.m[0][2] * z, r.m[1][0] * x + r.m[1][1] * y + r.m[1][2] * z,
            r.m[2][0] * x + r.m[2][1] * y + r.m[2][2] * z};
}
// (a rotation's inverse is its transpose)
std::array<float, 3> turn_back(const Mat3& r, float x, float y, float z) {
    return {r.m[0][0] * x + r.m[1][0] * y + r.m[2][0] * z, r.m[0][1] * x + r.m[1][1] * y + r.m[2][1] * z,
            r.m[0][2] * x + r.m[1][2] * y + r.m[2][2] * z};
}

constexpr float kPickRange = 30000.0f;  // corners further away than this are not picked

bool still_part(const Part& p) { return p.angles == std::array<int32_t, 3>{} && p.frames.empty(); }

std::array<int, 3> spot_of(const Part& part, const Vec3i16& v) {
    return {v.x + part.offset[0], v.y + part.offset[1], v.z + part.offset[2]};
}

Mat3 rotation_of(const Placement& p) { return placement_rotation(float(p.ax), float(p.ay), float(p.az)); }

float dist2(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

// Whether a point at 1/depth iz drawn at sx, sy is in sight: some pixel around it shows
// something no nearer than it (with a little slack: it lies on the surfaces drawn there).
bool in_sight(const Renderer* depth, float sx, float sy, float iz) {
    if (!depth) return true;
    const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
    for (int y = y0 - 1; y <= y0 + 1; ++y)
        for (int x = x0 - 1; x <= x0 + 1; ++x)
            if (depth->depth_at(x, y) <= iz * 1.03f) return true;
    return false;
}

} // namespace

bool TrackEdits::parse(const std::string& text, std::string* error) {
    std::istringstream in(text);
    std::string line;
    int number = 0;
    auto fail = [&](const char* what) {
        if (error) *error = "line " + std::to_string(number) + ": " + what;
        return false;
    };
    while (std::getline(in, line)) {
        ++number;
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string word;
        if (!(ls >> word)) continue;
        if (word == "track") {
            std::string name;
            size_t count = 0;
            if (!(ls >> name >> count)) return fail("expected: track <name> <placements>");
            if (!track.empty() && name != track) return fail("made for another track file");
            if (placements && count != placements) return fail("made for a track file with another number of placements");
            track = name;
            placements = count;
        } else if (word == "corner") {
            CornerKey k;
            std::array<float, 3> d{};
            if (!(ls >> k.placement >> k.spot[0] >> k.spot[1] >> k.spot[2] >> d[0] >> d[1] >> d[2]))
                return fail("expected: corner <placement> <x> <y> <z> <dx> <dy> <dz>");
            if (k.placement < 0 || (placements && size_t(k.placement) >= placements)) return fail("no such placement");
            corners[k] = d;
        } else {
            return fail("unknown line");
        }
    }
    return true;
}

std::string TrackEdits::serialize() const {
    std::string s = "# Screamer HD track edits: corners of the placed objects moved (drawing only)\n"
                    "# corner <placement> <x y z in the object> <dx dy dz moved, world axes>\n";
    char buf[160];
    std::snprintf(buf, sizeof buf, "track %s %zu\n", track.c_str(), placements);
    s += buf;
    for (const auto& [k, d] : corners) {
        std::snprintf(buf, sizeof buf, "corner %d %d %d %d %g %g %g\n", k.placement, k.spot[0], k.spot[1], k.spot[2], double(d[0]),
                      double(d[1]), double(d[2]));
        s += buf;
    }
    return s;
}

bool TrackEdits::load(const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str(), error);
}

bool TrackEdits::save(const std::string& path) const {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    const std::string tmp = path + ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << serialize();
        if (!f) return false;
    }
    std::filesystem::rename(tmp, p, ec);
    return !ec;
}

void apply_track_edits(Track& t, const std::vector<std::vector<VertexShift>>& base, const TrackEdits& edits) {
    t.welds = base;
    t.welds.resize(t.placements.size());
    // the edits of each placement, by spot
    std::map<int, std::map<std::array<int, 3>, std::array<float, 3>>> by_placement;
    for (const auto& [k, d] : edits.corners)
        if (k.placement >= 0 && size_t(k.placement) < t.placements.size()) by_placement[k.placement][k.spot] = d;
    for (const auto& [pi, spots] : by_placement) {
        const Placement& p = t.placements[size_t(pi)];
        if (p.object < 0 || size_t(p.object) >= t.objects.size()) continue;
        const Model& m = t.objects[size_t(p.object)];
        const Mat3 r = rotation_of(p);
        std::vector<VertexShift> moved;
        std::set<std::pair<uint16_t, uint16_t>> taken;
        for (size_t part_index = 0; part_index < m.parts.size(); ++part_index) {
            const Part& part = m.parts[part_index];
            if (!still_part(part)) continue;
            for (size_t vi = 0; vi < part.vertices.size(); ++vi) {
                const auto it = spots.find(spot_of(part, part.vertices[vi]));
                if (it == spots.end()) continue;
                const auto o = turn_back(r, it->second[0], it->second[1], it->second[2]);
                moved.push_back({uint16_t(part_index), uint16_t(vi), o[0], o[1], o[2]});
                taken.insert({uint16_t(part_index), uint16_t(vi)});
            }
        }
        auto& w = t.welds[size_t(pi)];
        w.erase(std::remove_if(w.begin(), w.end(), [&](const VertexShift& s) { return taken.count({s.part, s.vertex}) != 0; }), w.end());
        w.insert(w.end(), moved.begin(), moved.end());
    }
}

TrackEditor::TrackEditor(Track& track, std::vector<std::vector<VertexShift>> base, TrackEdits edits)
    : track_(track), base_(std::move(base)), edits_(std::move(edits)) {
    base_.resize(track_.placements.size());
    rot_.resize(track_.placements.size());
    std::set<std::pair<int, int>> edge_set;
    for (size_t pi = 0; pi < track_.placements.size(); ++pi) {
        const Placement& p = track_.placements[pi];
        rot_[pi] = rotation_of(p);
        if (p.object < 0 || size_t(p.object) >= track_.objects.size()) continue;
        const Model& m = track_.objects[size_t(p.object)];
        std::map<std::array<int, 3>, int> here;  // spot -> corner
        auto corner_at = [&](size_t part_index, uint16_t vi) {
            const Part& part = m.parts[part_index];
            const std::array<int, 3> spot = spot_of(part, part.vertices[vi]);
            auto [it, fresh] = here.try_emplace(spot, int(corners_.size()));
            if (fresh) {
                EditCorner c;
                c.placement = int(pi);
                c.spot = spot;
                const auto w = turn(rot_[pi], float(spot[0]), float(spot[1]), float(spot[2]));
                c.file = {w[0] + float(p.x), w[1] + float(p.y), w[2] + float(p.z)};
                corners_.push_back(c);
                index_[{int(pi), spot}] = it->second;
            }
            auto& list = corners_[size_t(it->second)].vertices;
            const std::pair<uint16_t, uint16_t> ref{uint16_t(part_index), vi};
            if (std::find(list.begin(), list.end(), ref) == list.end()) list.push_back(ref);
            return it->second;
        };
        for (size_t part_index = 0; part_index < m.parts.size(); ++part_index) {
            const Part& part = m.parts[part_index];
            if (!still_part(part)) continue;
            for (const Face& f : part.faces)
                for (int i = 0; i < f.count && i < 6; ++i) {
                    const uint16_t a = f.vtx[size_t(i)], b = f.vtx[size_t((i + 1) % f.count)];
                    if (a >= part.vertices.size() || b >= part.vertices.size()) continue;
                    const int ca = corner_at(part_index, a), cb = corner_at(part_index, b);
                    if ((f.open_edges >> i) & 1) corners_[size_t(ca)].open = corners_[size_t(cb)].open = true;
                    if (ca != cb) edge_set.insert({std::min(ca, cb), std::max(ca, cb)});
                }
        }
    }
    edges_.assign(edge_set.begin(), edge_set.end());
    // where the welding draws each corner (every vertex at the spot gets the same shift)
    auto_pos_.resize(corners_.size());
    std::vector<std::unordered_map<uint32_t, const VertexShift*>> shifts(base_.size());
    for (size_t pi = 0; pi < base_.size(); ++pi)
        for (const VertexShift& s : base_[pi]) shifts[pi][uint32_t(s.part) << 16 | s.vertex] = &s;
    for (size_t i = 0; i < corners_.size(); ++i) {
        const EditCorner& c = corners_[i];
        auto_pos_[i] = c.file;
        for (const auto& [part, vertex] : c.vertices) {
            const auto it = shifts[size_t(c.placement)].find(uint32_t(part) << 16 | vertex);
            if (it == shifts[size_t(c.placement)].end()) continue;
            const auto w = turn(rot_[size_t(c.placement)], it->second->dx, it->second->dy, it->second->dz);
            auto_pos_[i] = {c.file[0] + w[0], c.file[1] + w[1], c.file[2] + w[2]};
            break;
        }
    }
    // drop edits for corners the track does not have
    for (auto it = edits_.corners.begin(); it != edits_.corners.end();)
        it = index_.count(it->first) ? std::next(it) : edits_.corners.erase(it);
    edits_.placements = track_.placements.size();
    apply_track_edits(track_, base_, edits_);
    find_gaps();
}

std::array<float, 3> TrackEditor::position(int corner) const {
    const EditCorner& c = corners_[size_t(corner)];
    if (const auto it = edits_.corners.find({c.placement, c.spot}); it != edits_.corners.end())
        return {c.file[0] + it->second[0], c.file[1] + it->second[1], c.file[2] + it->second[2]};
    return auto_pos_[size_t(corner)];
}

int TrackEditor::pick_corner(const Camera& cam, const Framebuffer& fb, const Renderer* depth, float mx, float my, float radius) const {
    const ScreenProjection project(cam, fb);
    int best = -1, best_hidden = -1;
    float bd = radius * radius, bdh = radius * radius, best_iz = 0, best_iz_h = 0;
    for (size_t i = 0; i < corners_.size(); ++i) {
        const auto p = position(int(i));
        if (std::fabs(p[0] - cam.x) > kPickRange || std::fabs(p[2] - cam.z) > kPickRange) continue;
        float sx, sy, iz;
        if (!project(p[0], p[1], p[2], sx, sy, iz)) continue;
        const float d = (sx - mx) * (sx - mx) + (sy - my) * (sy - my);
        // (several corners at one spot of the screen: the nearest to the eye)
        const bool seen = in_sight(depth, sx, sy, iz);
        float& b = seen ? bd : bdh;
        float& biz = seen ? best_iz : best_iz_h;
        int& who = seen ? best : best_hidden;
        if (d < b - 0.5f || (d <= b + 0.5f && iz > biz)) {
            b = d;
            biz = iz;
            who = int(i);
        }
    }
    return best >= 0 ? best : best_hidden;
}

int TrackEditor::pick_edge(const Camera& cam, const Framebuffer& fb, const Renderer* depth, float mx, float my, float radius) const {
    const ScreenProjection project(cam, fb);
    int best = -1, best_hidden = -1;
    float bd = radius * radius, bdh = radius * radius;
    for (size_t e = 0; e < edges_.size(); ++e) {
        const auto a = position(edges_[e].first), b = position(edges_[e].second);
        if (std::fabs(a[0] - cam.x) > kPickRange || std::fabs(a[2] - cam.z) > kPickRange) continue;
        float ax, ay, az, bx, by, bz;
        if (!project(a[0], a[1], a[2], ax, ay, az) || !project(b[0], b[1], b[2], bx, by, bz)) continue;
        const float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
        const float t = len2 > 0 ? std::clamp(((mx - ax) * vx + (my - ay) * vy) / len2, 0.0f, 1.0f) : 0.0f;
        const float px = ax + vx * t, py = ay + vy * t, d = (px - mx) * (px - mx) + (py - my) * (py - my);
        const bool seen = in_sight(depth, px, py, az + (bz - az) * t);
        if (seen && d < bd) {
            bd = d;
            best = int(e);
        } else if (!seen && d < bdh) {
            bdh = d;
            best_hidden = int(e);
        }
    }
    return best >= 0 ? best : best_hidden;
}

void TrackEditor::select(std::vector<int> item, bool add) {
    if (!add) {
        selection_ = {std::move(item)};
        return;
    }
    std::vector<int> sorted = item;
    std::sort(sorted.begin(), sorted.end());
    for (auto it = selection_.begin(); it != selection_.end(); ++it) {
        std::vector<int> s = *it;
        std::sort(s.begin(), s.end());
        if (s == sorted) {
            selection_.erase(it);
            return;
        }
    }
    selection_.push_back(std::move(item));
}

void TrackEditor::set_position(int corner, const std::array<float, 3>& world) {
    const EditCorner& c = corners_[size_t(corner)];
    edits_.corners[{c.placement, c.spot}] = {world[0] - c.file[0], world[1] - c.file[1], world[2] - c.file[2]};
}

bool TrackEditor::snap(bool onto_first) {
    if (selection_.size() < 2) return false;
    const auto before = edits_.corners;
    std::vector<std::pair<int, int>> pairs;  // (stays or meets, moves or meets)
    if (selection_.size() == 2 && selection_[0].size() == selection_[1].size() && selection_[0].size() <= 2) {
        const auto& a = selection_[0];
        const auto& b = selection_[1];
        if (a.size() == 1) {
            pairs = {{a[0], b[0]}};
        } else {
            // each end of the first edge with the nearer end of the second
            const float straight = dist2(position(a[0]), position(b[0])) + dist2(position(a[1]), position(b[1]));
            const float crossed = dist2(position(a[0]), position(b[1])) + dist2(position(a[1]), position(b[0]));
            pairs = straight <= crossed ? std::vector<std::pair<int, int>>{{a[0], b[0]}, {a[1], b[1]}}
                                        : std::vector<std::pair<int, int>>{{a[0], b[1]}, {a[1], b[0]}};
        }
        for (const auto& [x, y] : pairs) {
            if (x == y) continue;
            const auto px = position(x), py = position(y);
            if (onto_first) {
                set_position(y, px);
            } else {
                const std::array<float, 3> mid{(px[0] + py[0]) * 0.5f, (px[1] + py[1]) * 0.5f, (px[2] + py[2]) * 0.5f};
                set_position(x, mid);
                set_position(y, mid);
            }
        }
    } else {
        std::vector<int> all;
        for (const auto& item : selection_)
            for (const int c : item)
                if (std::find(all.begin(), all.end(), c) == all.end()) all.push_back(c);
        std::array<float, 3> target = position(all[0]);
        if (!onto_first) {
            target = {0, 0, 0};
            for (const int c : all) {
                const auto p = position(c);
                for (int k = 0; k < 3; ++k) target[size_t(k)] += p[size_t(k)];
            }
            for (float& v : target) v /= float(all.size());
        }
        for (const int c : all) set_position(c, target);
    }
    if (edits_.corners == before) return false;
    undo_.push_back(before);
    redo_.clear();
    commit();
    return true;
}

bool TrackEditor::nudge(float dx, float dy, float dz) {
    if (selection_.empty()) return false;
    const auto before = edits_.corners;
    std::set<int> done;
    for (const auto& item : selection_)
        for (const int c : item)
            if (done.insert(c).second) {
                const auto p = position(c);
                set_position(c, {p[0] + dx, p[1] + dy, p[2] + dz});
            }
    undo_.push_back(before);
    redo_.clear();
    commit();
    return true;
}

bool TrackEditor::reset_selected() {
    const auto before = edits_.corners;
    for (const auto& item : selection_)
        for (const int c : item) edits_.corners.erase({corners_[size_t(c)].placement, corners_[size_t(c)].spot});
    if (edits_.corners == before) return false;
    undo_.push_back(before);
    redo_.clear();
    commit();
    return true;
}

bool TrackEditor::undo() {
    if (undo_.empty()) return false;
    redo_.push_back(edits_.corners);
    edits_.corners = undo_.back();
    undo_.pop_back();
    commit();
    return true;
}

bool TrackEditor::redo() {
    if (redo_.empty()) return false;
    undo_.push_back(edits_.corners);
    edits_.corners = redo_.back();
    redo_.pop_back();
    commit();
    return true;
}

void TrackEditor::commit() {
    apply_track_edits(track_, base_, edits_);
    find_gaps();
    dirty_ = true;
}

void TrackEditor::find_gaps() {
    gaps_.clear();
    // the corners where road pieces meet (the welding's: road pieces, and scenery with painted lines)
    std::vector<int> joint;
    for (size_t i = 0; i < corners_.size(); ++i) {
        const EditCorner& c = corners_[i];
        const Placement& p = track_.placements[size_t(c.placement)];
        if (!c.open || p.object < 0 || size_t(p.object) >= track_.objects.size()) continue;
        if (p.slot != 0 && !track_.objects[size_t(p.object)].has_strips) continue;
        joint.push_back(int(i));
    }
    constexpr float kFar = 200.0f;
    std::vector<std::array<float, 3>> pos(joint.size());
    std::unordered_map<int64_t, std::vector<int>> grid;
    auto cell = [&](float v) { return int64_t(std::floor(v / kFar)); };
    auto key = [](int64_t gx, int64_t gz) { return (gx << 32) ^ (gz & 0xffffffff); };
    for (size_t i = 0; i < joint.size(); ++i) {
        pos[i] = position(joint[i]);
        grid[key(cell(pos[i][0]), cell(pos[i][2]))].push_back(int(i));
    }
    std::vector<int> nearest(joint.size(), -1);
    std::vector<float> nd(joint.size(), kFar * kFar);
    for (size_t i = 0; i < joint.size(); ++i) {
        const int pl = corners_[size_t(joint[i])].placement;
        for (int64_t gx = cell(pos[i][0]) - 1; gx <= cell(pos[i][0]) + 1; ++gx)
            for (int64_t gz = cell(pos[i][2]) - 1; gz <= cell(pos[i][2]) + 1; ++gz) {
                const auto it = grid.find(key(gx, gz));
                if (it == grid.end()) continue;
                for (const int j : it->second) {
                    if (corners_[size_t(joint[size_t(j)])].placement == pl) continue;
                    const float d = dist2(pos[i], pos[size_t(j)]);
                    if (d < nd[i]) {
                        nd[i] = d;
                        nearest[i] = j;
                    }
                }
            }
    }
    std::vector<std::pair<float, std::pair<int, int>>> found;
    for (size_t i = 0; i < joint.size(); ++i) {
        const int j = nearest[i];
        if (j < 0 || nearest[size_t(j)] != int(i) || j < int(i) || nd[i] <= min_gap * min_gap) continue;
        found.push_back({nd[i], {joint[i], joint[size_t(j)]}});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& f : found) gaps_.push_back(f.second);
    if (gap_at_ >= int(gaps_.size())) gap_at_ = -1;
}

bool TrackEditor::select_gap(int dir, std::array<float, 3>& middle) {
    if (gaps_.empty()) return false;
    const int n = int(gaps_.size());
    gap_at_ = gap_at_ < 0 ? (dir >= 0 ? 0 : n - 1) : ((gap_at_ + (dir >= 0 ? 1 : -1)) % n + n) % n;
    const auto [a, b] = gaps_[size_t(gap_at_)];
    selection_ = {{a}, {b}};
    const auto pa = position(a), pb = position(b);
    middle = {(pa[0] + pb[0]) * 0.5f, (pa[1] + pb[1]) * 0.5f, (pa[2] + pb[2]) * 0.5f};
    return true;
}

void TrackEditor::draw_overlay(const Camera& cam, Framebuffer& fb, const Renderer* depth, int hover_corner, int hover_edge,
                               const Colors& colors, float range) const {
    const ScreenProjection project(cam, fb);
    const int w = fb.width(), h = fb.height();
    const int unit = std::max(1, h / 360);  // mark size, in pixels
    auto plot = [&](int x, int y, uint8_t c) {
        if (x >= 0 && y >= 0 && x < w && y < h) fb.at(x, y) = c;
    };
    auto box = [&](float sx, float sy, int r, uint8_t c, bool filled) {
        const int x0 = int(std::lround(sx)), y0 = int(std::lround(sy));
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x)
                if (filled || std::abs(x) == r || std::abs(y) == r) plot(x0 + x, y0 + y, c);
    };
    auto line = [&](float x0, float y0, float x1, float y1, uint8_t c) {
        const int steps = int(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0))) + 1;
        if (steps > 8192) return;
        for (int i = 0; i <= steps; ++i) {
            const float t = float(i) / float(steps);
            const int x = int(std::lround(x0 + (x1 - x0) * t)), y = int(std::lround(y0 + (y1 - y0) * t));
            for (int k = 0; k < unit; ++k) plot(x + k, y, c);
        }
    };
    auto near_eye = [&](const std::array<float, 3>& p) {
        return std::fabs(p[0] - cam.x) < range && std::fabs(p[1] - cam.y) < range && std::fabs(p[2] - cam.z) < range;
    };
    auto edge = [&](int a, int b, uint8_t c) {
        const auto pa = position(a), pb = position(b);
        float ax, ay, az, bx, by, bz;
        if (project(pa[0], pa[1], pa[2], ax, ay, az) && project(pb[0], pb[1], pb[2], bx, by, bz))
            line(ax, ay, bx, by, c);
    };
    // the corners where pieces meet, near the eye and in sight
    for (size_t i = 0; i < corners_.size(); ++i) {
        if (!corners_[i].open) continue;
        const auto p = position(int(i));
        if (!near_eye(p)) continue;
        float sx, sy, iz;
        if (project(p[0], p[1], p[2], sx, sy, iz) && in_sight(depth, sx, sy, iz)) box(sx, sy, 0, colors.corner, true);
    }
    // the open gaps nearby
    for (const auto& [a, b] : gaps_) {
        const auto pa = position(a);
        if (!near_eye(pa)) continue;
        edge(a, b, colors.gap);
        float sx, sy, iz;
        if (project(pa[0], pa[1], pa[2], sx, sy, iz)) box(sx, sy, 2 * unit, colors.gap, false);
    }
    if (hover_edge >= 0 && size_t(hover_edge) < edges_.size()) edge(edges_[size_t(hover_edge)].first, edges_[size_t(hover_edge)].second, colors.hover);
    for (const auto& item : selection_) {
        if (item.size() == 2) edge(item[0], item[1], colors.selected);
        for (const int c : item) {
            const auto p = position(c);
            float sx, sy, iz;
            if (project(p[0], p[1], p[2], sx, sy, iz)) box(sx, sy, 2 * unit, colors.selected, true);
        }
    }
    if (hover_corner >= 0 && size_t(hover_corner) < corners_.size()) {
        const auto p = position(hover_corner);
        float sx, sy, iz;
        if (project(p[0], p[1], p[2], sx, sy, iz)) box(sx, sy, 3 * unit, colors.hover, false);
    }
}

TrackEditor::Colors overlay_colors(const Palette& palette) {
    auto nearest = [&](int r, int g, int b) {
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < 256; ++i) {
            const int dr = palette[size_t(i)].r - r, dg = palette[size_t(i)].g - g, db = palette[size_t(i)].b - b;
            const int d = dr * dr + dg * dg + db * db;
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
        return uint8_t(best);
    };
    TrackEditor::Colors c;
    c.corner = nearest(255, 255, 255);
    c.hover = nearest(255, 255, 0);
    c.selected = nearest(0, 255, 0);
    c.gap = nearest(255, 0, 0);
    c.shadow = nearest(0, 0, 0);
    return c;
}

} // namespace scr

namespace scr {
namespace {
// A 5 x 7 pixel font for the edit mode's notes: each glyph 7 rows, bit 4 the leftmost pixel.
struct Glyph {
    char c;
    uint8_t rows[7];
};
constexpr Glyph kGlyphs[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {14, 4, 4, 4, 4, 4, 14}},
    {'J', {7, 2, 2, 2, 2, 18, 12}},      {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 17, 25, 21, 19, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},       {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},  {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},     {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},      {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {31, 2, 4, 2, 1, 17, 14}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 30, 1, 1, 17, 14}},   {'6', {6, 8, 16, 30, 17, 17, 14}},
    {'7', {31, 1, 2, 4, 8, 8, 8}},       {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 2, 12}},
    {'.', {0, 0, 0, 0, 0, 12, 12}},      {',', {0, 0, 0, 0, 12, 4, 8}},       {':', {0, 12, 12, 0, 12, 12, 0}},
    {'/', {1, 1, 2, 4, 8, 16, 16}},      {'\\', {16, 16, 8, 4, 2, 1, 1}},     {'(', {2, 4, 8, 8, 8, 4, 2}},
    {')', {8, 4, 2, 2, 2, 4, 8}},        {'+', {0, 4, 4, 31, 4, 4, 0}},       {'-', {0, 0, 0, 31, 0, 0, 0}},
    {'=', {0, 0, 31, 0, 31, 0, 0}},      {'_', {0, 0, 0, 0, 0, 0, 31}},       {'?', {14, 17, 1, 2, 4, 0, 4}},
    {'\'', {4, 4, 8, 0, 0, 0, 0}},       {'!', {4, 4, 4, 4, 4, 0, 4}},
};
} // namespace

void draw_overlay_text(Framebuffer& fb, int x, int y, const std::string& text, uint8_t color, uint8_t shadow, int scale) {
    auto plot = [&](int px, int py, uint8_t c) {
        for (int dy = 0; dy < scale; ++dy)
            for (int dx = 0; dx < scale; ++dx) {
                const int fx = px + dx, fy = py + dy;
                if (fx >= 0 && fy >= 0 && fx < fb.width() && fy < fb.height()) fb.at(fx, fy) = c;
            }
    };
    for (const char raw : text) {
        const char c = raw >= 'a' && raw <= 'z' ? char(raw - 'a' + 'A') : raw;
        if (c != ' ') {
            const Glyph* g = nullptr;
            for (const Glyph& k : kGlyphs)
                if (k.c == c) g = &k;
            if (!g) g = &kGlyphs[std::size(kGlyphs) - 3];  // '?'
            for (int pass = 0; pass < 2; ++pass)  // a shadow down and to the right, then the letter
                for (int r = 0; r < 7; ++r)
                    for (int b = 0; b < 5; ++b)
                        if ((g->rows[r] >> (4 - b)) & 1) plot(x + (b + 1 - pass) * scale, y + (r + 1 - pass) * scale, pass ? color : shadow);
        }
        x += 6 * scale;
    }
}

} // namespace scr
