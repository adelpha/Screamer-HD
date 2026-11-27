#pragma once
// Hand-made fixes to how a track looks (ours): corners of the placed objects moved, kept in a
// text file of their own per track file (e.g. TRACKS/MAP1H.TXT next to CHOICE.DAT), so the
// game's files stay as they are. They only change what is drawn: like the automatic joint
// welding (Track::welds), the moves go into the per-placement vertex shifts the renderer
// applies, while the game's own sums (ground, collisions) keep the file's vertices.
//
// File format, one line per moved corner ('#' starts a comment):
//   track MAP1H 1164                         the track file and its number of placements
//   corner <placement> <x> <y> <z> <dx> <dy> <dz>
// x, y, z: the corner in the object's own axes (whole units, as in the .UNR; every vertex of
// the object's still parts at that spot moves with it); dx, dy, dz: how far it is moved from
// where the file puts it, in world axes. A corner listed here is drawn there instead of where
// the automatic welding would put it.

#include "core/framebuffer.h"
#include "core/render3d.h"
#include "core/track.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace scr {

struct CornerKey {
    int placement = 0;
    std::array<int, 3> spot{};  // object axes
    auto operator<=>(const CornerKey&) const = default;
};

struct TrackEdits {
    std::string track;    // e.g. "MAP1H"
    size_t placements = 0;
    std::map<CornerKey, std::array<float, 3>> corners;  // world-axis moves

    // False (and a message) for a malformed file or one made for another track file.
    bool parse(const std::string& text, std::string* error = nullptr);
    std::string serialize() const;
    bool load(const std::string& path, std::string* error = nullptr);  // false also when missing
    bool save(const std::string& path) const;                        // makes the folder
};

// Every corner of the track's placed objects that can be moved: one per spot of the still
// parts (no part placement, not animated) of each placement.
struct EditCorner {
    int placement = 0;
    std::array<int, 3> spot{};
    std::vector<std::pair<uint16_t, uint16_t>> vertices;  // (part, vertex) at the spot
    std::array<float, 3> file{};                          // world position as the file puts it
    bool open = false;  // on an edge that no other face of its part shares (where pieces meet)
};

// Moves from the welding and the edits: t.welds becomes `base` (the automatic welds) with every
// edited corner's vertices moved by its edit instead. Edits for placements or corners the track
// does not have are left out.
void apply_track_edits(Track& t, const std::vector<std::vector<VertexShift>>& base, const TrackEdits& edits);

// The edit mode's state and operations; nothing in it knows about windows or input.
class TrackEditor {
public:
    // base: the automatic welds (Track::welds as load_track left them); edits: what the
    // track's edit file held. Applies them to the track.
    TrackEditor(Track& track, std::vector<std::vector<VertexShift>> base, TrackEdits edits);

    const TrackEdits& edits() const { return edits_; }
    const std::vector<EditCorner>& corners() const { return corners_; }
    const std::vector<std::pair<int, int>>& edges() const { return edges_; }
    // Where the corner is drawn now (file position + its edit, or its weld).
    std::array<float, 3> position(int corner) const;

    // The corner (or edge) nearest to screen point mx, my within radius pixels, -1 if none.
    // Corners hidden behind something drawn (by depth's last frame, which drew fb; may be null) are only
    // taken when no visible one is near.
    int pick_corner(const Camera& cam, const Framebuffer& fb, const Renderer* depth, float mx, float my, float radius) const;
    int pick_edge(const Camera& cam, const Framebuffer& fb, const Renderer* depth, float mx, float my, float radius) const;

    // The selection: items of one corner or two (an edge), in the order they were picked.
    const std::vector<std::vector<int>>& selection() const { return selection_; }
    void clear_selection() { selection_.clear(); }
    // Adds the item, or takes it out when it is already selected (toggle), else replaces the selection.
    void select(std::vector<int> item, bool add);

    // Snapping: two corners, or two edges (each end with the nearer end of the other), meet
    // half way (meet) or the second goes onto the first (join). With more or other items, all
    // the corners go to their middle, or onto the first one picked. False if nothing moved.
    bool meet() { return snap(false); }
    bool join() { return snap(true); }
    bool nudge(float dx, float dy, float dz);  // every selected corner, world axes
    bool reset_selected();                     // back to the file's (or the weld's) position
    bool undo();
    bool redo();

    // Joints the automatic welding left open: pairs of corners of neighbouring road pieces,
    // each the other's nearest, further apart than `min_gap` units (now, with the edits);
    // widest first. Worked out again after each change.
    const std::vector<std::pair<int, int>>& gaps() const { return gaps_; }
    // Selects the next (dir 1) or previous (-1) gap and returns its middle; false if there is none.
    bool select_gap(int dir, std::array<float, 3>& middle);
    float min_gap = 4.0f;

    // Marks drawn over the picture: corners near the camera, the hovered and selected ones,
    // selected edges and the open gaps. Colours are palette indices.
    struct Colors {
        uint8_t corner = 15, hover = 14, selected = 10, gap = 12, shadow = 0;
    };
    void draw_overlay(const Camera& cam, Framebuffer& fb, const Renderer* depth, int hover_corner, int hover_edge,
                      const Colors& colors, float range = 3000.0f) const;

    bool dirty() const { return dirty_; }  // changed since mark_saved()
    void mark_saved() { dirty_ = false; }

private:
    bool snap(bool onto_first);
    void set_position(int corner, const std::array<float, 3>& world);
    void commit();  // after a change: apply, find the gaps, note for undo
    void find_gaps();

    Track& track_;
    std::vector<std::vector<VertexShift>> base_;  // the automatic welds
    TrackEdits edits_;
    std::vector<EditCorner> corners_;
    std::vector<std::pair<int, int>> edges_;
    std::vector<std::array<float, 3>> auto_pos_;  // per corner: where the welding draws it
    std::vector<Mat3> rot_;                    // per placement (identity for those without corners)
    std::map<CornerKey, int> index_;           // corner by key
    std::vector<std::vector<int>> selection_;
    std::vector<std::map<CornerKey, std::array<float, 3>>> undo_, redo_;
    std::vector<std::pair<int, int>> gaps_;
    int gap_at_ = -1;
    bool dirty_ = false;
};

// The edit mode's notes, in a small font of ours (letters, digits, a few signs), scale pixels a
// font pixel, with a shadow.
void draw_overlay_text(Framebuffer& fb, int x, int y, const std::string& text, uint8_t color, uint8_t shadow, int scale);

// Palette indices nearest to the overlay's colours (white, yellow, green, red) in a palette.
TrackEditor::Colors overlay_colors(const Palette& palette);

} // namespace scr
