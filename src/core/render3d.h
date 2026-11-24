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

// Objects and parts whose bounding sphere lies wholly outside the view are skipped; polygons
// are clipped and projected as they come, and their pixels are filled at finish(), by all CPU
// cores at once, each taking bands of rows. Every pixel sees the polygons in the order they
// were drawn, so the picture does not depend on the number of threads.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;

    void set_textures(bool on) { textures_ = on; }
    // Threads that fill pixels (<= 0: one per hardware thread, the default).
    void set_threads(int threads);
    void set_anim_counter(int32_t counter) { anim_counter_ = counter; }  // SimClock::anim_counter()

    // Clears, draws the background panorama and every placement of the track.
    // Equivalent to begin() + draw_placements() + finish().
    RenderStats draw_track(const Track& track, const Camera& cam, Framebuffer& fb, uint8_t sky = 0);

    // Lower-level use: begin a frame, draw the track and extra objects (cars), then finish,
    // which draws the translucent polygons back to front.
    void begin(const Track& track, const Camera& cam, Framebuffer& fb, uint8_t sky = 0);
    void draw_placements();
    // Draws `model` (only the parts whose part_mask entry is true, when a mask is given).
    void draw_model(const Model& model, const Pose& pose, const std::vector<bool>* part_mask = nullptr);
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
    struct Scr {
        float x, y, iz, uz, vz;
    };
    // What a polygon's pixels are filled with.
    struct Fill {
        const Material* mat;
        const uint8_t* texels;  // the frame's pixels when every texel lies inside the library, else null
        int mode;               // Mode
        int frame;
        int wmask, hmask;       // size - 1 for power-of-two sizes, else -1
        uint8_t flat;
        bool use_texels, writes_depth;
        float depth_bias;       // in units of d = 128 - z
    };
    struct Tri {
        Scr a, b, c;
        int y0, y1, x0, x1;
        uint32_t fill;
    };
    // Clips, projects and queues the polygon's triangles.
    void draw_polygon(const ClipVert* poly, size_t count, const Material* mat, uint8_t face_color, int32_t animation,
                      int32_t reflection, float depth_bias = 0);
    bool sphere_visible(float x, float y, float z, float r) const;
    void fill_rows(int y0, int y1);

    const Track* track_ = nullptr;
    const Camera* cam_ = nullptr;
    Framebuffer* fb_ = nullptr;
    Mat3 view_{};
    RenderStats stats_{};
    std::vector<float> depth_;  // 1/z per pixel
    std::vector<Deferred> deferred_;
    std::vector<ClipVert> deferred_verts_;
    std::vector<Fill> fills_;
    std::vector<Tri> tris_;
    // view frustum planes in camera space (unit normals, offsets), for sphere_visible
    std::array<std::array<float, 4>, 4> planes_{};
    int plane_count_ = 0;
    std::unique_ptr<WorkerPool> pool_;
    std::vector<float> wx_, wy_, wz_;
    std::vector<std::array<int, 2>> env_uv_;
    bool textures_ = true;
    int32_t anim_counter_ = 0;
};

} // namespace scr
