#pragma once
// Software renderer for the decoded tracks and objects. It does not imitate the
// original's column-based fillers: it draws perspective-correct textured polygons with a
// depth buffer into a Framebuffer of any size, using the original's palette indices.
//
// Coordinate conventions follow the game: y up, angles in 2048 steps per turn, camera
// angles (pitch, yaw, roll) as in camera_build_matrices. The projection matches the original's PERSP01.TAB at 320x200:
// screen = centre + 128 * camera_xy / (128 + depth), scaled with the output height.

#include "core/framebuffer.h"
#include "core/track.h"

#include <vector>

namespace scr {

struct Camera {
    float x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0, roll = 0;  // game angle units (0..2048)
    float focal = 128.0f;                 // at 200 lines; scaled with the output height
    float far_distance = 60000.0f;
};

struct RenderStats {
    int objects = 0;
    int faces = 0;
};

class Renderer {
public:
    void set_textures(bool on) { textures_ = on; }
    // Draws the background panorama (if the track has one) and then the track.
    RenderStats draw_track(const Track& track, const Camera& cam, Framebuffer& fb, uint8_t sky = 0);
    // The sky/background band, like render_draw_background: panorama row 234 sits on the
    // horizon, the column offset follows the yaw ((0x7ff - yaw) / 2, wrapping at 512).
    static void draw_background(const PcxImage& bck, const Camera& cam, Framebuffer& fb);

private:
    struct ClipVert {
        float x, y, z;  // camera space
        float u, v;     // texels
    };
    void draw_polygon(const std::vector<ClipVert>& poly, const Material* mat, const MaterialLibrary* lib,
                      Framebuffer& fb, const Camera& cam);
    std::vector<float> depth_;  // 1/z per pixel
    bool textures_ = true;
};

} // namespace scr
