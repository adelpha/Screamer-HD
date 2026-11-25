#pragma once
// Software renderer for the decoded tracks and objects. It does not imitate the
// original's column-based fillers: it draws perspective-correct textured polygons with a
// depth buffer into a Framebuffer of any size, using the original's palette indices.
//
// Coordinate conventions follow the game: y up, angles in 2048 steps per turn, camera
// angles (pitch, yaw, roll) as in camera_build_matrices. The projection matches the original's PERSP01.TAB at 320x200:
// screen = centre + 128 * camera_xy / (128 + depth), scaled with the output height.
//
// Material types (g_material_handlers, four handlers per type):
//   0-2   mat0_draw   textured
//   3-7   mat3_draw   textured, texel 0 transparent
//   8-9   mat8_draw   flat fill with the face's colour (face +0x02)
//   10-11 mat10_draw  textured (separate span filler in the original)
//   12-13 mat12_draw  flat darken: new = darken[old] (build_darken_tables, x0.6)
//   14-15 mat14_draw  translucent: new = shade_table[texel * 256 + old] (MAPn.SMG)

#include "core/framebuffer.h"
#include "core/render3d_fill.h"
#include "core/track.h"

#include <array>
#include <memory>
#include <vector>

namespace scr {

struct Camera {
    float x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0, roll = 0;  // game angle units (0..2048)
    float focal = 128.0f;                 // at 200 lines; scaled with the output height
    float center_y = 130.0f;              // projection centre row at 200 lines (g_render_camera:
                                          // x 160, y 130 for the 320x200 race view)
    float far_distance = 300000.0f;
    float far_depth = 1e30f;  // view distance: nothing further away (camera depth) is drawn
    float fade_depth = 1e30f; // from here to far_depth, pixels fade out in an ordered dither (ours)
};

struct RenderStats {
    int objects = 0;
    int faces = 0;
};

struct Mat3 {
    float m[3][3];
};

// Scroll state of an object's environment-mapped glass (instance +0x24..+0x38 in the game):
// the object's movement, turned into camera space, accumulates into a texture offset.
struct EnvScroll {
    float acc[3]{};
    int prev[3]{};
    bool started = false;
};

// An object placed in the world: position and angles (x = pitch, y = yaw, z = roll).
struct Pose {
    float x = 0, y = 0, z = 0;
    float ax = 0, ay = 0, az = 0;
    // Object animation word (instance +0x20, placement +0x20): a texture phase in bits 2-15,
    // or negative (track placements) to follow the global animation counter. Cars use 0x8000
    // for the second paint scheme (CarData.animation), 0x4000 and 0x2000 for game state.
    int32_t animation = -1;
    // Instance +0x3c (car_sync_render_object: surface_render_flag, opponents: render_flags,
    // placements: record +0x3c): picks the frame of material types 10/11, the car windows'
    // reflections (frames * value >> 16): 0 open sky, 0x8000 / 0xff00 tunnels and the like.
    int32_t reflection = 0;
    // object_transform_float: parts flagged 0x2000 get reflection UVs every frame for their
    // type 10/11 faces (the car glass). env keeps the scroll between frames; env_rate is how fast
    // movement scrolls it (0.002 on the road, 0.03 off it, set by car_apply_surface; 0.008
    // until the first car_apply_surface).
    EnvScroll* env = nullptr;
    float env_rate = 0.008f;
    // Drawn as if this many units nearer the camera in the depth test (negative: further). The original sorts
    // whole objects and never depth-tests, so a car's shadow (a flat quad tilted with the car)
    // always lies on the road; a bias keeps it from sinking into slopes here.
    float depth_bias = 0;
    // Turned to face the camera whatever its angles (the tyre smoke: ours; the original's
    // puffs keep the angles they were made with and look thin from the side).
    bool billboard = false;
    // Per-part placements for this frame (cars: lean, suspension, steering, wheel roll), by
    // part index; parts beyond the end or without flags keep the file's.
    const std::vector<PartPose>* parts = nullptr;
    // Road pieces (track placements of slot 0): they meet edge to edge but are transformed
    // separately, and the hairline cracks between them would show the sky; their polygons
    // reach a third of an original pixel past their edges.
    bool seal_edges = false;
    // A car (object_compute_depth_dynamic: half the distance for the detail levels).
    bool dynamic = false;
};

// The flat ground pieces of the scenery are tested and written as if this much further away,
// so the road and its verges, drawn after them in the original, stay on top where the
// ground pokes up through them.
constexpr float kFlatGroundBias = 4096.0f;
// Car wheels (Part::wheel) are tested as if this much nearer.
constexpr float kWheelBias = 64.0f;

// Material frame shown by cell_begin + mat*_draw: frames * phase >> 14, where the phase is
// animation >> 2, or the low 14 bits of the global animation counter for animation < 0.
constexpr int material_frame(int frames, int32_t animation, int32_t anim_counter) {
    const int32_t phase = animation >= 0 ? (animation >> 2) & 0x3fff : anim_counter & 0x3fff;
    return int((int64_t(frames) * phase) >> 14);
}

class WorkerPool;

// Objects and parts whose bounding sphere lies wholly outside the view are skipped. The draw
// calls only note what to draw; flush_geometry() (or finish()) then transforms, clips and
// projects everything on all CPU cores, and finish() fills the pixels, each core taking bands
// of rows (render3d_fill.h). Every pixel sees the polygons in the order they were drawn, so
// the picture does not depend on the number of threads.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;

    void set_textures(bool on) { textures_ = on; }
    // The original's distant versions of objects (off: always the full detail).
    void set_lod(bool on) { lod_ = on; }
    // Threads that fill pixels (<= 0: one per hardware thread, the default).
    void set_threads(int threads);
    // Which build of the pixel filler runs: Auto is the widest the CPU has. All give the same
    // picture; choosing one is for tests and measurements. False if the CPU cannot run it.
    enum class FillPath { Auto, Sse2, Avx2 };
    bool set_fill_path(FillPath path);
    const char* fill_path_name() const;
    WorkerPool* pool() { return pool_.get(); }
    // Skipped frames (a game step that is not shown): begin .. finish do nothing.
    void set_skip(bool skip) { skip_ = skip; }  // the fill threads, for other per-pixel work
    void set_anim_counter(int32_t counter) { anim_counter_ = counter; }  // SimClock::anim_counter()

    // Clears, draws the background panorama and every placement of the track.
    // Equivalent to begin() + draw_placements() + finish().
    RenderStats draw_track(const Track& track, const Camera& cam, Framebuffer& fb, uint8_t sky = 0);

    // Lower-level use: begin a frame, draw the track and extra objects (cars), then finish,
    // which draws the translucent polygons back to front.
    // clear_at_finish: the picture is cleared (sky, background) by finish() rather than here,
    // which is quicker; for callers that draw nothing into fb themselves in between.
    void begin(const Track& track, const Camera& cam, Framebuffer& fb, uint8_t sky = 0, bool clear_at_finish = false);
    void draw_placements();
    // Draws `model` (only the parts whose part_mask entry is true, when a mask is given).
    // The drawing itself happens later, on all threads (flush_geometry): the model, the mask
    // and what the pose points to (parts, env) must stay as they are until then, and no two
    // models drawn in a frame may share an EnvScroll.
    void draw_model(const Model& model, const Pose& pose, const std::vector<bool>* part_mask = nullptr);
    // Transforms, clips and projects everything drawn since begin() (finish() does it if the
    // caller has not; apart from it, only for timing the two stages separately).
    void flush_geometry();
    RenderStats finish();

    // The sky/background band, like render_draw_background: panorama row 234 sits on the
    // horizon, the column offset follows the yaw ((0x7ff - yaw) / 2, wrapping at 512).
    static void draw_background(const PcxImage& bck, const Camera& cam, Framebuffer& fb);
    static void draw_background_rows(const PcxImage& bck, const Camera& cam, Framebuffer& fb, int y0, int y1);

private:
    struct ClipVert {
        float x, y, z;  // camera space
        float u, v;     // texels
    };
    struct Deferred {
        uint32_t first, count;  // in deferred_verts_
        const Material* mat;
        uint8_t color;
        int32_t animation, reflection;
        float depth;
        float depth_bias;
    };
    using Scr = detail::Scr;
    using Fill = detail::Fill;  // what a polygon's pixels are filled with
    using Tri = detail::Tri;
    // Clips, projects and queues the polygon's triangles.
    // What drawing produces before the pixels are filled: one set per thread while the
    // placements are drawn, joined in order afterwards.
    struct Geo {
        std::vector<Deferred> deferred;
        std::vector<ClipVert> deferred_verts;
        std::vector<Fill> fills;
        std::vector<Tri> tris;
        std::vector<std::array<int, 2>> tri_rows;  // each triangle's first and last row (for the fill threads to pick theirs)
        std::vector<float> wx, wy, wz;
        std::vector<std::array<int, 2>> env_uv;
        float edge_slack = 0;  // for the polygons of the model being drawn
        RenderStats stats{};
        void clear();
    };
    struct ModelJob {
        const Model* model;
        Pose pose;
        const std::vector<bool>* part_mask;
    };
    void draw_placement_range(Geo& g, size_t i0, size_t i1);
    void draw_model_into(Geo& g, const Model& model, const Pose& pose, const std::vector<bool>* part_mask);
    void draw_polygon(Geo& g, const ClipVert* poly, size_t count, const Material* mat, uint8_t face_color,
                      int32_t animation, int32_t reflection, float depth_bias = 0);
    bool sphere_visible(float x, float y, float z, float r) const;
    // Rows y0 .. y1 - 1 before anything is drawn: no depth, the background or the sky colour.
    void clear_rows(int y0, int y1);
    // The panorama column under each framebuffer column (empty: no panorama).
    static void background_columns(const PcxImage& bck, const Camera& cam, const Framebuffer& fb, std::vector<int>& column);
    static void draw_background_rows(const PcxImage& bck, const Camera& cam, Framebuffer& fb, int y0, int y1, const int* column);


    const Track* track_ = nullptr;
    const Camera* cam_ = nullptr;
    Framebuffer* fb_ = nullptr;
    Mat3 view_{};
    std::vector<float> depth_;  // 1/z per pixel
    Geo geo_;                          // the translucent polygons of all jobs, and their triangles
    std::vector<Geo> job_geo_;         // what each geometry job produced (the first job_count_ are this frame's)
    size_t job_count_ = 0;
    std::vector<ModelJob> model_jobs_; // draw_model() calls since begin(), in order
    bool placements_pending_ = false;  // draw_placements() was called,
    size_t placements_at_ = 0;         // after this many draw_model() calls
    bool geometry_done_ = false;
    std::vector<int> bg_columns_;  // background_columns() of this frame
    uint8_t sky_ = 0;
    bool clear_at_finish_ = false;
    // view frustum planes in camera space (unit normals, offsets), for sphere_visible
    std::array<std::array<float, 4>, 4> planes_{};
    int plane_count_ = 0;
    std::unique_ptr<WorkerPool> pool_;
    detail::FillRowsFn fill_rows_ = nullptr;  // null: not chosen yet (Auto)
    bool textures_ = true;
    bool lod_ = false;
    bool skip_ = false;
    bool fade_ = false;            // the view-distance fade is on this frame
    float fade_iz_ = 0;            // 1 / (128 + fade_depth): pixels with a smaller 1/z are in the fade
    float fade_start_ = 0, fade_range_ = 1;
    int32_t anim_counter_ = 0;
};

} // namespace scr
