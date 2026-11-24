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

// An object placed in the world: position and angles (x = pitch, y = yaw, z = roll).
struct Pose {
    float x = 0, y = 0, z = 0;
    float ax = 0, ay = 0, az = 0;
    int frame = 0;  // material frame (cars: animation bits 13-15 of the instance, see car_frame)
};

// Material frame of a car from its object animation bits (opponent_sync_render_object):
// 0x8000 second paint scheme (CarData.animation), 0x4000 and 0x2000 set by the game state.
constexpr int car_frame(uint32_t animation) { return int((animation >> 13) & 7); }

class Renderer {
public:
    void set_textures(bool on) { textures_ = on; }

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

private:
    struct ClipVert {
        float x, y, z;  // camera space
        float u, v;     // texels
    };
    struct Deferred {
        std::vector<ClipVert> poly;
        const Material* mat;
        uint8_t color;
        int frame;
        float depth;
    };
    void draw_polygon(const std::vector<ClipVert>& poly, const Material* mat, uint8_t face_color, int frame);

    const Track* track_ = nullptr;
    const Camera* cam_ = nullptr;
    Framebuffer* fb_ = nullptr;
    Mat3 view_{};
    RenderStats stats_{};
    std::vector<float> depth_;  // 1/z per pixel
    std::vector<Deferred> deferred_;
    std::vector<float> wx_, wy_, wz_;
    bool textures_ = true;
};

} // namespace scr
