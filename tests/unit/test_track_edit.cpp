#include "testing.h"

#include "core/model3d.h"
#include "core/render3d.h"
#include "core/track.h"
#include "core/track_edit.h"

#include <cmath>
#include <filesystem>

// The edit mode's corner moves (core/track_edit.h) on a made-up track: two square road
// pieces end to end, the second 150 units to the side, too far for the automatic welding.

namespace {

scr::Track two_pieces(int side = 150) {
    scr::Track t;
    scr::Model m;
    scr::Part part;
    part.vertices = {{0, 0, 0}, {1000, 0, 0}, {1000, 0, 1000}, {0, 0, 1000}};
    scr::Face f;
    f.count = 4;
    f.vtx = {0, 1, 2, 3, 0, 0};
    part.faces = {f};
    m.parts.push_back(part);
    scr::compute_bounds(m);
    t.objects.push_back(m);
    scr::Placement a, b;
    a.slot = b.slot = 0;
    b.x = side;
    b.z = 1000;
    t.placements = {a, b};
    scr::weld_road_joints(t);
    return t;
}

float distance(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// the corner of placement p at object spot x, z
int corner(const scr::TrackEditor& e, int p, int x, int z) {
    for (size_t i = 0; i < e.corners().size(); ++i)
        if (e.corners()[i].placement == p && e.corners()[i].spot == std::array<int, 3>{x, 0, z}) return int(i);
    return -1;
}

} // namespace

TEST_CASE("track edits: the file reads back as written and refuses another track's") {
    scr::TrackEdits e;
    e.track = "MAP1H";
    e.placements = 1164;
    e.corners[{12, {-1024, 5, 2048}}] = {1.5f, -2.0f, 0.25f};
    e.corners[{3, {0, 0, 0}}] = {10.0f, 0.0f, 0.0f};
    scr::TrackEdits back;
    back.track = "MAP1H";
    back.placements = 1164;
    std::string err;
    REQUIRE(back.parse(e.serialize(), &err));
    CHECK(back.corners == e.corners);

    scr::TrackEdits other;
    other.track = "MAP2H";
    other.placements = 1164;
    CHECK(!other.parse(e.serialize(), &err));
    scr::TrackEdits fewer;
    fewer.track = "MAP1H";
    fewer.placements = 100;
    CHECK(!fewer.parse(e.serialize(), &err));
    scr::TrackEdits junk;
    CHECK(!junk.parse("corner 1 2 3\n", &err));
    CHECK(junk.parse("# nothing but a comment\n\n", &err));

    const auto dir = std::filesystem::temp_directory_path() / "screamer_track_edit_test";
    const std::string path = (dir / "TRACKS" / "MAP1H.TXT").string();
    REQUIRE(e.save(path));
    scr::TrackEdits loaded;
    REQUIRE(loaded.load(path, &err));
    CHECK(loaded.corners == e.corners);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("track edits: two corners meet half way, or the second goes onto the first") {
    scr::Track t = two_pieces();
    // 150 units apart: the welding leaves them
    REQUIRE(t.welds.size() == 2);
    CHECK(t.welds[0].empty());
    CHECK(t.welds[1].empty());
    const auto base = t.welds;
    scr::TrackEditor e(t, base, {});
    const int a = corner(e, 0, 0, 1000), b = corner(e, 1, 0, 0);
    REQUIRE(a >= 0);
    REQUIRE(b >= 0);
    CHECK(distance(e.position(a), e.position(b)) == 150.0f);
    // the open joints: both ends of the pieces' meeting edges
    CHECK(e.gaps().size() == 2);

    e.select({a}, false);
    e.select({b}, true);
    REQUIRE(e.meet());
    CHECK(distance(e.position(a), e.position(b)) < 1e-3f);
    CHECK(std::fabs(e.position(a)[0] - 75.0f) < 1e-3f);
    CHECK(e.gaps().size() == 1);
    CHECK(e.dirty());
    // the renderer's shifts: one vertex of each piece, 75 units across
    REQUIRE(t.welds[0].size() == 1);
    REQUIRE(t.welds[1].size() == 1);
    CHECK(t.welds[0][0].vertex == 3);
    CHECK(std::fabs(t.welds[0][0].dx - 75.0f) < 1e-3f);
    CHECK(std::fabs(t.welds[1][0].dx + 75.0f) < 1e-3f);

    REQUIRE(e.undo());
    CHECK(distance(e.position(a), e.position(b)) == 150.0f);
    CHECK(t.welds[0].empty());
    REQUIRE(e.redo());
    CHECK(distance(e.position(a), e.position(b)) < 1e-3f);
    REQUIRE(e.undo());

    REQUIRE(e.join());  // b onto a
    CHECK(distance(e.position(a), e.position(b)) < 1e-3f);
    CHECK(e.position(a)[0] == 0.0f);
    CHECK(t.welds[0].empty());

    // nudging and putting back
    e.select({a}, false);
    REQUIRE(e.nudge(0.0f, 4.0f, 0.0f));
    CHECK(std::fabs(e.position(a)[1] - 4.0f) < 1e-3f);
    REQUIRE(e.reset_selected());
    CHECK(e.position(a) == e.corners()[size_t(a)].file);
}

TEST_CASE("track edits: two edges are snapped end to end") {
    scr::Track t = two_pieces();
    scr::TrackEditor e(t, t.welds, {});
    const int a0 = corner(e, 0, 0, 1000), a1 = corner(e, 0, 1000, 1000);
    const int b0 = corner(e, 1, 0, 0), b1 = corner(e, 1, 1000, 0);
    // picked the other way round: each end still goes to the nearer end
    e.select({a0, a1}, false);
    e.select({b1, b0}, true);
    REQUIRE(e.meet());
    CHECK(distance(e.position(a0), e.position(b0)) < 1e-3f);
    CHECK(distance(e.position(a1), e.position(b1)) < 1e-3f);
    CHECK(e.gaps().empty());
    // picking an item again takes it out
    e.select({b0, b1}, true);
    CHECK(e.selection().size() == 1);
}

TEST_CASE("track edits: edits saved for a track are applied when it loads, over its welds") {
    scr::Track t = two_pieces(10);  // close enough for the welding
    REQUIRE(!t.welds[0].empty());
    const auto base = t.welds;
    scr::TrackEdits edits;
    edits.corners[{0, {0, 0, 1000}}] = {0.0f, 7.0f, 0.0f};
    edits.corners[{5, {0, 0, 0}}] = {1.0f, 1.0f, 1.0f};  // no such placement: left out
    scr::apply_track_edits(t, base, edits);
    int edited = 0;
    for (const scr::VertexShift& s : t.welds[0])
        if (s.vertex == 3) {
            ++edited;
            CHECK(s.dx == 0.0f);
            CHECK(s.dy == 7.0f);
        }
    CHECK(edited == 1);
    CHECK(t.welds[0].size() == base[0].size());  // the other welded corner stays welded
    CHECK(t.welds[1].size() == base[1].size());
    // the editor reads the same back
    scr::TrackEditor e(t, base, edits);
    CHECK(e.edits().corners.size() == 1);
    CHECK(std::fabs(e.position(corner(e, 0, 0, 1000))[1] - 7.0f) < 1e-3f);
}

TEST_CASE("track edits: corners are picked where the renderer draws them") {
    scr::Track t = two_pieces();
    scr::TrackEditor e(t, t.welds, {});
    scr::Framebuffer fb(320, 200);
    scr::Camera cam;  // looking down -z from above the first piece's far corner
    cam.x = 0;
    cam.y = 300;
    cam.z = 3000;
    float sx, sy, iz;
    const int a = corner(e, 0, 0, 1000);
    const auto p = e.position(a);
    REQUIRE(scr::project_point(cam, fb, p[0], p[1], p[2], sx, sy, iz));
    CHECK(std::fabs(sx - 160.0f) < 0.01f);  // straight ahead, below the centre
    CHECK(sy > cam.center_y);
    CHECK(e.pick_corner(cam, fb, nullptr, sx + 2.0f, sy - 1.0f, 8.0f) == a);
    CHECK(e.pick_corner(cam, fb, nullptr, sx + 40.0f, sy, 8.0f) != a);
    // behind the eye: not on screen
    CHECK(!scr::project_point(cam, fb, 0.0f, 300.0f, 3100.0f, sx, sy, iz));
}
