#pragma once
// The hardware renderer: fills the triangles the software renderer's geometry stage makes
// (core/render3d.h, SceneSink) on the graphics card, in full colour, into a texture the size of
// the picture. The presenter lays the software-drawn HUD over it (Framebuffer layer mode).
//
// What the CPU filler does per pixel is done per fragment: the same screen-space interpolation
// of 1/z and texel/z, depth biases, edge sealing, the view-distance dither and the sky panorama.
// The palette's colours are looked up when the textures are made; the translucent and darkening
// materials (palette tables in the original) become a per-channel multiply-add fitted to the
// tables and done by the blender (dual-source blending).

#include "core/render3d.h"
#include "video/gl_api.h"
#include "video/presenter.h"

#include <memory>

namespace scr {
class TexturePack;
}

namespace scr::video {

class GlScene final : public SceneSink {
public:
    GlScene();
    ~GlScene() override;
    bool init();
    void draw_scene(const SceneFrame& frame) override;
    // The track's textures made and uploaded now (a fraction of a second), not at its first frame.
    void prepare(const Track& track) override;
    // FILTER, ANISO, AA and FADE (Quality), from the next scene on.
    void set_quality(const Quality& q);
    // Replacement textures (null: none); the track's textures are made again when they change.
    void set_textures(std::shared_ptr<const TexturePack> pack);
    // The last scene drawn (RGBA, width x height, top row first), or 0.
    GLuint texture() const;
    int width() const;
    int height() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace scr::video
