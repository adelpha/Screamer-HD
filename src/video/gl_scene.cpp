#include "video/gl_scene.h"

#include "core/pcx.h"
#include "core/texture_pack.h"
#include "core/track.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace scr::video {
namespace {

constexpr float kAngle = 6.28318530718f / 2048.0f;
// Texture arrays (one per texture size) the fragment shader can sample from.
constexpr int kMaxGroups = 12;
constexpr int kFirstGroupUnit = 4;

// One vertex of a triangle as the CPU filler would get it: screen position, 1/z and texel/z
// (interpolated linearly across the screen, as the filler does), the weights of the triangle's
// own corners (outside 0..1 where it was moved out to seal an edge) and what fills it.
struct Vertex {
    float x, y;
    float iz, uz, vz;
    float w0, w1, w2;
    int16_t group, layer, mode, flags;
    float tw, th, bias, flat;
    float lw, lh;  // the size of the texture layer the material is in (it repeats across it)
    // The pixels the filler looks at for the triangle (its own bounding box, in whole pixels):
    // a sealed sliver's moved-out edges meet far beyond its corners, and are cut off there.
    float x0, y0, x1, y1;
};
static_assert(sizeof(Vertex) == 80);

enum Flags : int16_t { kClampUv = 1, kSealBehind = 2, kTexels = 4, kTrueDepth = 8 };

constexpr const char* kSceneVs = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec3 a_izuv;
layout(location = 2) in vec3 a_w;
layout(location = 3) in ivec4 a_fill;   // group, layer, mode, flags
layout(location = 4) in vec4 a_fill2;   // texture width, height, depth bias, flat colour
layout(location = 5) in vec2 a_layer;   // the layer's width and height
layout(location = 6) in vec4 a_box;     // the pixels the triangle may cover: x0, y0, x1, y1 (inclusive)
uniform vec2 u_size;
noperspective out float v_iz;
noperspective out vec2 v_uvz;
noperspective out vec3 v_w;
flat out ivec4 v_fill;
flat out vec4 v_fill2;
flat out vec2 v_layer;
flat out vec4 v_box;
void main() {
    gl_Position = vec4(a_pos / u_size * 2.0 - 1.0, 0.0, 1.0);
    v_iz = a_izuv.x;
    v_uvz = a_izuv.yz;
    v_w = a_w;
    v_fill = a_fill;
    v_fill2 = a_fill2;
    v_layer = a_layer;
    v_box = a_box;
}
)";

// The sky's palette index at a pixel, as Renderer::draw_background_rows works it out (the sky
// pass and the fog both use it).
constexpr const char* kSkyFunc = R"(
uniform sampler2D u_palette;     // 256 x 1
uniform sampler2D u_pano;   // palette indices, top row first
uniform sampler2D u_pano_rgb;    // a replacement panorama in full colour (core/texture_pack.h)
uniform int u_has_rgb;
uniform int u_has_pano;
uniform int u_sky;          // palette index without a panorama
uniform int u_fill;         // above and below the panorama
uniform int u_wide;
uniform vec4 u_a;           // x_offset, mid, cx, cy
uniform vec4 u_b;           // across, down, scale, top
uniform vec2 u_rot;         // sin, cos of the roll (0, 1: level)
// where the pixel falls on the panorama, in its texels; false above or below it
bool sky_pos(vec2 frag, out vec2 p) {
    float x = floor(frag.x), y = floor(frag.y);
    float x_offset = u_a.x, mid = u_a.y, cx = u_a.z, cy = u_a.w;
    float across = u_b.x, down = u_b.y, scale = u_b.z, top = u_b.w;
    float s2 = u_rot.x, c2 = u_rot.y;
    float dy = y - cy;
    float u0, v0, dv;
    if (u_wide != 0) {
        u0 = x_offset + mid + (cx - c2 * cx) * across - s2 * dy * down;
        v0 = (cy + c2 * dy - top) / scale - s2 * cx * across;
        dv = s2 * across;
    } else {
        u0 = x_offset + (cx - c2 * cx - s2 * dy) * across;
        v0 = (cy - s2 * cx + c2 * dy - top) / scale;
        dv = s2 / scale;
    }
    p = vec2(u0 + c2 * across * x, v0 + dv * x);
    float row = floor(p.y);
    return row >= 0.0 && row < float(textureSize(u_pano, 0).y);
}
int sky_index(vec2 frag) {
    if (u_has_pano == 0) return u_sky;
    vec2 p;
    if (!sky_pos(frag, p)) return u_fill;
    ivec2 size = textureSize(u_pano, 0);
    int row = int(floor(p.y));
    int col = int(floor(p.x)) % size.x;
    if (col < 0) col += size.x;
    return int(texelFetch(u_pano, ivec2(col, row), 0).r * 255.0 + 0.5);
}
// the sky's colour at a pixel (a replacement panorama: filtered)
vec3 sky_color(vec2 frag) {
    vec2 p;
    if (u_has_pano != 0 && u_has_rgb != 0 && sky_pos(frag, p))
        return texture(u_pano_rgb, p / vec2(textureSize(u_pano, 0))).rgb;
    return texelFetch(u_palette, ivec2(sky_index(frag), 0), 0).rgb;
}
)";

constexpr const char* kSceneFs = R"(
noperspective in float v_iz;
noperspective in vec2 v_uvz;
noperspective in vec3 v_w;
flat in ivec4 v_fill;
flat in vec4 v_fill2;
flat in vec2 v_layer;
flat in vec4 v_box;
uniform sampler2D u_shade;    // 64 x 2: multiply, add (the translucency table, fitted)
uniform vec3 u_darken_mul, u_darken_add;
uniform sampler2DArray u_tex0, u_tex1, u_tex2, u_tex3, u_tex4, u_tex5, u_tex6, u_tex7, u_tex8, u_tex9, u_tex10, u_tex11;
uniform vec4 u_fade;          // on, 1/z where it starts, start depth, range
uniform int u_true_depth;     // the second pass of a BiasTest batch: the real depth
uniform float u_bias_add;     // added to every polygon's depth bias (a car's parts over its earlier ones)
uniform float u_grad_scale;   // texel gradients times this pick the mipmap level
uniform int u_linear;         // the textures are filtered (bilinear, trilinear)
uniform int u_fog;            // the fade as fog towards the sky (else the dither)
uniform int u_a2c;            // multisampled: transparent texels by alpha to coverage
layout(location = 0, index = 0) out vec4 o_add;
layout(location = 0, index = 1) out vec4 o_mul;

const float kBayer[16] = float[16](0.03125, 0.53125, 0.15625, 0.65625, 0.78125, 0.28125, 0.90625, 0.40625,
                                   0.21875, 0.71875, 0.09375, 0.59375, 0.96875, 0.46875, 0.84375, 0.34375);

vec4 sample_group(int g, vec3 c, vec2 dx, vec2 dy) {
    if (g == 0) return textureGrad(u_tex0, c, dx, dy);
    if (g == 1) return textureGrad(u_tex1, c, dx, dy);
    if (g == 2) return textureGrad(u_tex2, c, dx, dy);
    if (g == 3) return textureGrad(u_tex3, c, dx, dy);
    if (g == 4) return textureGrad(u_tex4, c, dx, dy);
    if (g == 5) return textureGrad(u_tex5, c, dx, dy);
    if (g == 6) return textureGrad(u_tex6, c, dx, dy);
    if (g == 7) return textureGrad(u_tex7, c, dx, dy);
    if (g == 8) return textureGrad(u_tex8, c, dx, dy);
    if (g == 9) return textureGrad(u_tex9, c, dx, dy);
    if (g == 10) return textureGrad(u_tex10, c, dx, dy);
    return textureGrad(u_tex11, c, dx, dy);
}
vec4 fetch_group(int g, ivec3 c) {
    if (g == 0) return texelFetch(u_tex0, c, 0);
    if (g == 1) return texelFetch(u_tex1, c, 0);
    if (g == 2) return texelFetch(u_tex2, c, 0);
    if (g == 3) return texelFetch(u_tex3, c, 0);
    if (g == 4) return texelFetch(u_tex4, c, 0);
    if (g == 5) return texelFetch(u_tex5, c, 0);
    if (g == 6) return texelFetch(u_tex6, c, 0);
    if (g == 7) return texelFetch(u_tex7, c, 0);
    if (g == 8) return texelFetch(u_tex8, c, 0);
    if (g == 9) return texelFetch(u_tex9, c, 0);
    if (g == 10) return texelFetch(u_tex10, c, 0);
    return texelFetch(u_tex11, c, 0);
}

// The filler lets a pixel be drawn over one up to 0.05% nearer (tz * 1.0005 >= the depth
// there): of nearly coplanar faces the one drawn later shows. A depth test cannot compare one
// value and write another, so the depths are written in steps of that size instead (1/z on a
// log scale, rounded down): within a step the later face wins (GEQUAL), as with the filler.
float depth_value(float tz) {
    float steps = floor(log2(max(tz, exp2(-40.0))) * 1386.64);  // 1 / log2(1.0005)
    return (steps + 60000.0) / 131072.0;
}

void main() {
    int group = v_fill.x, layer = v_fill.y, mode = v_fill.z, flags = v_fill.w;
    vec2 size = v_fill2.xy;
    float bias = v_fill2.z + u_bias_add;
    // texel coordinates and their change across the screen (before any branching)
    vec2 uv = v_uvz / v_iz;
    vec2 uvn = uv / v_layer;
    vec2 gx = dFdx(uvn) * u_grad_scale, gy = dFdy(uvn) * u_grad_scale;

    float iz = v_iz;
    ivec2 px = ivec2(gl_FragCoord.xy);
    if (float(px.x) < v_box.x || float(px.x) > v_box.z || float(px.y) < v_box.y || float(px.y) > v_box.w) discard;
    float fog = 0.0;
    if (u_fade.x > 0.0 && iz < u_fade.y) {
        // the view-distance fade over its last quarter: an ordered dither, or fog
        float frac = (1.0 / iz - 128.0 - u_fade.z) / u_fade.w;
        if (u_fog != 0) {
            if (frac >= 1.0) discard;
            fog = smoothstep(0.0, 1.0, clamp(frac, 0.0, 1.0));
        } else if (!(frac < kBayer[(px.y & 3) * 4 + (px.x & 3)])) {
            discard;
        }
    }
    float tz = bias != 0.0 ? iz / max(1e-3, 1.0 - bias * iz) : iz;
    // pixels past the polygon's own edges (sealing): as if a little further away
    if ((flags & 2) != 0 && any(lessThan(v_w, vec3(0.0)))) tz *= 0.995;
    gl_FragDepth = depth_value(u_true_depth != 0 ? iz : tz);

    vec3 fog_rgb = fog > 0.0 ? sky_color(gl_FragCoord.xy) : vec3(0.0);

    o_mul = vec4(0.0);
    if (mode == 3) {  // darken (fading out with the fog)
        o_add = vec4(u_darken_add * (1.0 - fog), 0.0);
        o_mul = vec4(mix(u_darken_mul, vec3(1.0), fog), 1.0);
        return;
    }
    if (mode == 2 || (flags & 4) == 0) {  // flat
        o_add = vec4(mix(texelFetch(u_palette, ivec2(int(v_fill2.w), 0), 0).rgb, fog_rgb, fog), 1.0);
        return;
    }
    // wrapped to the material (or kept on it), in the layer it repeats across (filtered: kept
    // half a texel inside, where the filter would reach past the edge)
    if ((flags & 1) != 0) uv = u_linear != 0 ? clamp(uv, vec2(0.5), size - vec2(0.5)) : clamp(uv, vec2(0.0), size - vec2(1.0 / 256.0));
    else uv -= size * floor(uv / size);
    if (mode == 4) {  // translucent: the palette index in alpha, fitted table
        ivec2 t = clamp(ivec2(floor(uv)), ivec2(0), ivec2(size) - 1);
        int idx = int(fetch_group(group, ivec3(t, layer)).a * 255.0 + 0.5) & 63;
        o_mul = vec4(mix(texelFetch(u_shade, ivec2(idx, 0), 0).rgb, vec3(1.0), fog), 1.0);
        o_add = vec4(texelFetch(u_shade, ivec2(idx, 1), 0).rgb * (1.0 - fog), 0.0);
        return;
    }
    vec4 c = sample_group(group, vec3(uv / v_layer, float(layer)), gx, gy);
    float alpha = 1.0;
    if (mode == 1) {  // transparent texels
        if (u_a2c != 0) {
            // (alpha to coverage: the edge sharpened to about a pixel's width)
            alpha = clamp((c.a - 0.5) / max(fwidth(c.a), 1e-4) + 0.5, 0.0, 1.0);
            if (alpha <= 0.0) discard;
        } else if (c.a < 0.5) {
            discard;
        }
    }
    o_add = vec4(mix(c.rgb, fog_rgb, fog), alpha);
}
)";

// The sky: the background panorama (or the sky colour) under everything, as
// Renderer::draw_background_rows works it out for each pixel.
constexpr const char* kSkyVs = R"(#version 330 core
void main() {
    vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID >> 1) * 4 - 1));
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
constexpr const char* kSkyFs = R"(
out vec4 o_color;
void main() {
    o_color = vec4(sky_color(gl_FragCoord.xy), 1.0);
}
)";

// new = table[t * 256 + old] (a 64 x 256 palette table) as new = old * mul + add, per colour
// channel, least squares over the 256 old colours, each counted as often as it is used (the
// colours that are really under a shadow or a pane of glass).
void fit_table(const Palette& pal, const uint8_t* table, const double* weight, float mul[3], float add[3]) {
    for (int ch = 0; ch < 3; ++ch) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
        for (int o = 0; o < 256; ++o) {
            const double wt = weight[o];
            const auto& a = pal[size_t(o)];
            const auto& b = pal[table[o]];
            const double x = (ch == 0 ? a.r : ch == 1 ? a.g : a.b) / 255.0;
            const double y = (ch == 0 ? b.r : ch == 1 ? b.g : b.b) / 255.0;
            sx += wt * x;
            sy += wt * y;
            sxx += wt * x * x;
            sxy += wt * x * y;
            n += wt;
        }
        const double den = n * sxx - sx * sx;
        double m = den > 1e-12 ? (n * sxy - sx * sy) / den : 0.0;
        m = std::clamp(m, 0.0, 1.0);
        mul[ch] = float(m);
        add[ch] = float(std::clamp((sy - m * sx) / n, 0.0, 1.0));
    }
}

GLuint make_texture_2d(GLenum internal, int w, int h, GLenum format, GLenum type, const void* data) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GLint(internal), w, h, 0, format, type, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

} // namespace

struct GlScene::Impl {
    GLuint scene_prog = 0, sky_prog = 0, vao = 0, vbo = 0, sky_vao = 0;
    GLint u_size = -1, u_darken_mul = -1, u_darken_add = -1, u_fade = -1, u_true_depth = -1, u_grad_scale = -1, u_bias_add = -1;
    GLint u_linear = -1, u_fog = -1, u_a2c = -1;
    // the sky's uniforms in each program (sky pass, scene)
    struct SkyLoc {
        GLint has_pano = -1, sky = -1, fill = -1, wide = -1, a = -1, b = -1, rot = -1, has_rgb = -1;
        void find(GLuint p) {
            has_pano = gl::GetUniformLocation(p, "u_has_pano");
            sky = gl::GetUniformLocation(p, "u_sky");
            fill = gl::GetUniformLocation(p, "u_fill");
            wide = gl::GetUniformLocation(p, "u_wide");
            a = gl::GetUniformLocation(p, "u_a");
            b = gl::GetUniformLocation(p, "u_b");
            rot = gl::GetUniformLocation(p, "u_rot");
            has_rgb = gl::GetUniformLocation(p, "u_has_rgb");
            gl::Uniform1i(gl::GetUniformLocation(p, "u_pano_rgb"), 2);
        }
    };
    SkyLoc sky_loc, scene_sky_loc;
    // the picture (multisampled: drawn in ms_fbo, then resolved into fbo's colour texture)
    GLuint fbo = 0, color = 0, depth = 0;
    GLuint ms_fbo = 0, ms_color = 0, ms_depth = 0;
    int w = 0, h = 0, samples = 0;
    bool drawn = false;
    Quality quality;
    float max_aniso = 1.0f;
    int max_samples = 1;
    // the arrays' sampler state as last set: filter, anisotropy, mipmaps
    int set_filter = -1, set_aniso = -1;
    // the track's textures
    const MaterialLibrary* lib = nullptr;
    const PcxImage* pano_of = nullptr;
    Palette palette{};
    std::vector<uint8_t> shade_of;
    GLuint palette_tex = 0, shade_tex = 0, pano_tex = 0, pano_rgb_tex = 0;
    // replacement textures (core/texture_pack.h); remade when they change
    std::shared_ptr<const TexturePack> pack;
    bool pack_changed = false;
    size_t replaced = 0;
    struct Group {
        GLuint tex = 0;
        int w = 0, h = 0, layers = 0;
    };
    std::vector<Group> groups;
    struct Entry {
        int group = -1, layer = 0, frames = 1;
        float lw = 1, lh = 1;  // the layer's size in the material's texels (a replacement may have more)
    };
    std::vector<Entry> entries;  // by material index
    bool mip_filter = false;     // what the arrays' filters are set to
    float darken_mul[3]{0.6f, 0.6f, 0.6f}, darken_add[3]{};
    std::vector<Vertex> verts;

    ~Impl() {
        free_textures();
        if (pano_tex) glDeleteTextures(1, &pano_tex);
        if (pano_rgb_tex) glDeleteTextures(1, &pano_rgb_tex);
        free_target();
        if (vbo) gl::DeleteBuffers(1, &vbo);
        if (vao) gl::DeleteVertexArrays(1, &vao);
        if (sky_vao) gl::DeleteVertexArrays(1, &sky_vao);
        if (scene_prog) gl::DeleteProgram(scene_prog);
        if (sky_prog) gl::DeleteProgram(sky_prog);
    }

    void free_textures() {
        for (Group& g : groups)
            if (g.tex) glDeleteTextures(1, &g.tex);
        groups.clear();
        entries.clear();
        if (palette_tex) glDeleteTextures(1, &palette_tex);
        if (shade_tex) glDeleteTextures(1, &shade_tex);
        palette_tex = shade_tex = 0;
        lib = nullptr;
    }

    void free_target() {
        if (fbo) gl::DeleteFramebuffers(1, &fbo);
        if (color) glDeleteTextures(1, &color);
        if (depth) gl::DeleteRenderbuffers(1, &depth);
        if (ms_fbo) gl::DeleteFramebuffers(1, &ms_fbo);
        if (ms_color) gl::DeleteRenderbuffers(1, &ms_color);
        if (ms_depth) gl::DeleteRenderbuffers(1, &ms_depth);
        fbo = color = depth = ms_fbo = ms_color = ms_depth = 0;
        w = h = samples = 0;
        drawn = false;
    }

    // The picture's target, multisampled with ns samples (0: none).
    bool ensure_target(int nw, int nh, int ns) {
        ns = ns > 1 ? std::min(ns, max_samples) : 0;
        if (fbo && w == nw && h == nh && samples == ns) return true;
        free_target();
        color = make_texture_2d(GL_RGBA8, nw, nh, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl::GenRenderbuffers(1, &depth);
        gl::BindRenderbuffer(GL_RENDERBUFFER, depth);
        gl::RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, nw, nh);
        gl::GenFramebuffers(1, &fbo);
        gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
        bool ok = gl::CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (ok && ns) {
            gl::GenRenderbuffers(1, &ms_color);
            gl::BindRenderbuffer(GL_RENDERBUFFER, ms_color);
            gl::RenderbufferStorageMultisample(GL_RENDERBUFFER, ns, GL_RGBA8, nw, nh);
            gl::GenRenderbuffers(1, &ms_depth);
            gl::BindRenderbuffer(GL_RENDERBUFFER, ms_depth);
            gl::RenderbufferStorageMultisample(GL_RENDERBUFFER, ns, GL_DEPTH32F_STENCIL8, nw, nh);
            gl::GenFramebuffers(1, &ms_fbo);
            gl::BindFramebuffer(GL_FRAMEBUFFER, ms_fbo);
            gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ms_color);
            gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ms_depth);
            ok = gl::CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!ok) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "OpenGL: cannot draw a %dx%d scene (%d samples)", nw, nh, ns);
            free_target();
            return false;
        }
        w = nw;
        h = nh;
        samples = ns;
        return true;
    }

    // The track's materials as texture arrays, one per size, every animation frame a layer, in
    // the track's palette; and the tables the translucent and darkening materials use.
    void ensure_textures(const Track& t) {
        const MaterialLibrary* l = t.materials ? &*t.materials : nullptr;
        if (l == lib && t.palette == palette && t.shade_table == shade_of && palette_tex && !pack_changed) return;
        pack_changed = false;
        free_textures();
        lib = l;
        palette = t.palette;
        shade_of = t.shade_table;
        std::array<uint8_t, 256 * 4> pal{};
        for (size_t i = 0; i < 256; ++i) {
            pal[i * 4 + 0] = palette[i].r;
            pal[i * 4 + 1] = palette[i].g;
            pal[i * 4 + 2] = palette[i].b;
            pal[i * 4 + 3] = 255;
        }
        palette_tex = make_texture_2d(GL_RGBA8, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, pal.data());
        // how often each colour is used by the track's textures and its background
        std::array<double, 256> weight;
        weight.fill(1.0);
        if (const MaterialLibrary* ml = t.materials ? &*t.materials : nullptr)
            for (uint8_t c : ml->data()) weight[c] += 1.0;
        if (t.background)
            for (uint8_t c : t.background->pixels) weight[c] += 1.0;
        std::array<float, 64 * 2 * 4> shade{};
        if (t.shade_table.size() >= 64 * 256) {
            for (int k = 0; k < 64; ++k) {
                float mul[3], add[3];
                fit_table(palette, t.shade_table.data() + size_t(k) * 256, weight.data(), mul, add);
                for (int ch = 0; ch < 3; ++ch) {
                    shade[size_t(k * 4 + ch)] = mul[ch];
                    shade[size_t((64 + k) * 4 + ch)] = add[ch];
                }
            }
        }
        shade_tex = make_texture_2d(GL_RGBA32F, 64, 2, GL_RGBA, GL_FLOAT, shade.data());
        fit_table(palette, t.darken.data(), weight.data(), darken_mul, darken_add);
        if (!lib) return;

        const uint64_t t0 = SDL_GetTicksNS();
        const auto& mats = lib->materials();
        entries.assign(mats.size(), Entry{});
        // Each material goes into a layer of the smallest power-of-two size that holds it,
        // repeated across the layer (so that a mipmap's texels at the material's edge average
        // with its other side, as wrapping wants); the shader wraps u and v to the material.
        // Sizes in use beyond what the shader can sample from at once go into larger layers.
        // Replacements (TexturePack) come in at a whole multiple of the material's size (its
        // scale); the material's other frames, if any lack one, are scaled up to match.
        std::vector<std::vector<RgbaImage>> repl(mats.size());
        std::vector<int> scale(mats.size(), 1);
        if (pack && !pack->empty()) {
            parallel(mats.size(), [&](size_t mi) {
                const Material& m = mats[mi];
                if (!texture_replaceable(m)) return;
                const uint64_t h = pack->find(texture_hash(*lib, m, palette), texture_index_hash(*lib, m));
                if (!h) return;
                const int frames = texture_frames(m);
                std::vector<RgbaImage> imgs(static_cast<size_t>(frames));
                int s = 1;
                for (int f = 0; f < frames; ++f) {
                    auto img = pack->load(h, f);
                    if (!img || img->width <= 0 || img->height <= 0) continue;
                    s = std::max(s, std::clamp(int(std::lround(double(img->width) / m.width)), 1, 16));
                    imgs[size_t(f)] = std::move(*img);
                }
                const bool holes = texture_has_holes(m);
                for (int f = 0; f < frames; ++f) {
                    RgbaImage& img = imgs[size_t(f)];
                    if (img.pixels.empty()) img = resample(material_rgba(*lib, m, f, palette), m.width * s, m.height * s);  // (nearest: whole multiples)
                    else img = resample(img, m.width * s, m.height * s);
                    if (!holes)
                        for (size_t i = 3; i < img.pixels.size(); i += 4) img.pixels[i] = 255;
                }
                repl[mi] = std::move(imgs);
                scale[mi] = s;
            });
        }
        replaced = 0;
        for (const auto& r : repl) replaced += r.empty() ? 0 : 1;
        const auto pot = [](int v) { int p = 1; while (p < v) p <<= 1; return p; };
        std::vector<std::pair<int, int>> want(mats.size(), {0, 0});
        std::map<std::pair<int, int>, int> count;
        for (size_t mi = 0; mi < mats.size(); ++mi) {
            const Material& m = mats[mi];
            if (m.width <= 0 || m.height <= 0) continue;
            want[mi] = {pot(m.width * scale[mi]), pot(m.height * scale[mi])};
            count[want[mi]] += std::max(1, int(m.frame_pixels.size()));
        }
        std::map<std::pair<int, int>, std::pair<int, int>> into;  // size -> the layer size used
        for (const auto& c : count) into[c.first] = c.first;
        for (;;) {
            std::map<std::pair<int, int>, int> used;
            for (const auto& c : count) used[into[c.first]] += c.second;
            if (int(used.size()) <= kMaxGroups) break;
            // the two layer sizes whose layers together, in a size holding both, cost the
            // fewest extra bytes go into that size
            using Size = std::pair<int, int>;
            const auto bytes_of = [](const Size& s, int layers) { return int64_t(s.first) * s.second * layers; };
            Size from_a{0, 0}, from_b{0, 0}, to{0, 0};
            int64_t best = -1;
            for (auto a = used.begin(); a != used.end(); ++a)
                for (auto b = std::next(a); b != used.end(); ++b) {
                    const Size both{std::max(a->first.first, b->first.first), std::max(a->first.second, b->first.second)};
                    const int64_t extra = bytes_of(both, a->second + b->second) - bytes_of(a->first, a->second) - bytes_of(b->first, b->second);
                    if (best < 0 || extra < best) {
                        best = extra;
                        from_a = a->first;
                        from_b = b->first;
                        to = both;
                    }
                }
            for (auto& i : into)
                if (i.second == from_a || i.second == from_b) i.second = to;
        }
        std::map<std::pair<int, int>, int> group_of;
        // where every material goes (in order), then its pixels, on all CPU threads
        for (size_t mi = 0; mi < mats.size(); ++mi) {
            const Material& m = mats[mi];
            if (m.width <= 0 || m.height <= 0) continue;
            const std::pair<int, int> size = into[want[mi]];
            auto it = group_of.find(size);
            if (it == group_of.end()) {
                it = group_of.emplace(size, int(groups.size())).first;
                groups.push_back({0, size.first, size.second, 0});
            }
            Group& g = groups[size_t(it->second)];
            const int frames = std::max(1, int(m.frame_pixels.size()));
            entries[mi] = {it->second, g.layers, frames, float(g.w) / float(scale[mi]), float(g.h) / float(scale[mi])};
            g.layers += frames;
        }
        std::vector<std::vector<uint8_t>> pixels(groups.size());  // per group, all layers
        for (size_t gi = 0; gi < groups.size(); ++gi)
            pixels[gi].resize(size_t(groups[gi].w) * size_t(groups[gi].h) * size_t(groups[gi].layers) * 4);
        const auto make = [&](size_t mi) {
            const Material& m = mats[mi];
            const Entry& e = entries[mi];
            if (e.group < 0) return;
            const Group& g = groups[size_t(e.group)];
            const bool holes = m.type >= 3 && m.type <= 7;
            const bool translucent = (m.type >> 1) == 7;
            const size_t layer_bytes = size_t(g.w) * size_t(g.h) * 4;
            const int mw = m.width * scale[mi], mh = m.height * scale[mi];
            std::vector<uint8_t> one(size_t(mw) * size_t(mh) * 4);
            for (int f = 0; f < e.frames; ++f) {
                if (!repl[mi].empty()) {
                    one = repl[mi][size_t(f)].pixels;
                } else {
                    for (int v = 0; v < m.height; ++v)
                        for (int u = 0; u < m.width; ++u) {
                            const uint8_t idx = lib->texel(m, u, v, f);
                            uint8_t* o = one.data() + (size_t(v) * size_t(m.width) + size_t(u)) * 4;
                            o[0] = palette[idx].r;
                            o[1] = palette[idx].g;
                            o[2] = palette[idx].b;
                            o[3] = translucent ? idx : holes && idx == 0 ? 0 : 255;
                        }
                }
                if (holes) dilate(one.data(), mw, mh);
                // repeated across the layer, a row of the material at a time
                uint8_t* const out = pixels[size_t(e.group)].data() + size_t(e.layer + f) * layer_bytes;
                const size_t row = size_t(mw) * 4;
                for (int y = 0; y < g.h; ++y) {
                    uint8_t* const dst = out + size_t(y) * size_t(g.w) * 4;
                    const uint8_t* const src = one.data() + size_t(y % mh) * row;
                    for (int x = 0; x < g.w; x += mw) std::memcpy(dst + size_t(x) * 4, src, std::min(row, size_t(g.w - x) * 4));
                }
            }
        };
        parallel(mats.size(), make);
        const uint64_t t1 = SDL_GetTicksNS();
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            Group& g = groups[gi];
            glGenTextures(1, &g.tex);
            glBindTexture(GL_TEXTURE_2D_ARRAY, g.tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl::TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, g.w, g.h, g.layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels[gi].data());
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl::GenerateMipmap(GL_TEXTURE_2D_ARRAY);
        }
        mip_filter = false;
        size_t layers = 0;
        for (const Group& g : groups) layers += size_t(g.layers);
        size_t bytes = 0;
        for (const Group& g : groups) bytes += size_t(g.w) * size_t(g.h) * size_t(g.layers) * 4;
        glFinish();
        const uint64_t t2 = SDL_GetTicksNS();
        SDL_Log("hardware renderer: %zu materials (%zu replaced) in %zu texture arrays, %zu layers, %zu MB (%.0f ms to make, %.0f ms to upload)",
                mats.size(), replaced, groups.size(), layers, bytes >> 20, double(t1 - t0) / 1e6, double(t2 - t1) / 1e6);
    }

    // fn(0 .. n - 1) on all CPU threads
    template <class Fn>
    static void parallel(size_t n, const Fn& fn) {
        const unsigned k = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
        std::atomic<size_t> next{0};
        std::vector<std::thread> workers;
        for (unsigned i = 0; i < k; ++i)
            workers.emplace_back([&]() {
                for (size_t j; (j = next.fetch_add(1)) < n;) fn(j);
            });
        for (std::thread& worker : workers) worker.join();
    }

    // Texels that are holes get the colour of the texels beside them (a few passes), so that a
    // filtered or reduced texture does not darken towards its holes.
    static void dilate(uint8_t* px, int w, int h) {
        const size_t n = size_t(w) * size_t(h);
        std::vector<uint8_t> known(n), next_known(n);
        for (size_t i = 0; i < n; ++i) known[i] = px[i * 4 + 3] != 0;
        for (int pass = 0; pass < 4; ++pass) {
            next_known = known;
            bool any = false;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const size_t i = size_t(y) * size_t(w) + size_t(x);
                    if (known[i]) continue;
                    int r = 0, g = 0, b = 0, k = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const size_t j = size_t((y + dy + h) % h) * size_t(w) + size_t((x + dx + w) % w);
                            if (!known[j]) continue;
                            r += px[j * 4 + 0];
                            g += px[j * 4 + 1];
                            b += px[j * 4 + 2];
                            ++k;
                        }
                    if (!k) continue;
                    px[i * 4 + 0] = uint8_t(r / k);
                    px[i * 4 + 1] = uint8_t(g / k);
                    px[i * 4 + 2] = uint8_t(b / k);
                    next_known[i] = 1;
                    any = true;
                }
            known.swap(next_known);
            if (!any) break;
        }
    }

    // The arrays' filters for FILTER, ANISO and MIPMAPS.
    void set_sampling(bool mips) {
        const int filter = std::clamp(quality.filter, 0, 2);
        const int aniso = std::clamp(quality.aniso, 0, 4);
        if (mips == mip_filter && filter == set_filter && aniso == set_aniso) return;
        mip_filter = mips;
        set_filter = filter;
        set_aniso = aniso;
        const GLint mag = filter ? GL_LINEAR : GL_NEAREST;
        const GLint min = !mips ? mag : filter == 0 ? GL_NEAREST_MIPMAP_NEAREST : filter == 1 ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
        const float a = std::min(max_aniso, aniso ? float(1 << aniso) : 1.0f);
        for (const Group& g : groups) {
            glBindTexture(GL_TEXTURE_2D_ARRAY, g.tex);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, mag);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, min);
            if (max_aniso > 1.0f) glTexParameterf(GL_TEXTURE_2D_ARRAY, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, a);
        }
    }

    // the track's sky panorama (and its replacement, in the palette the track gives it)
    bool pano_pack_changed = false;
    void ensure_panorama(const Track& t) {
        const PcxImage* bck = t.background ? &*t.background : nullptr;
        if (bck == pano_of && (pano_tex || !bck) && !pano_pack_changed) return;
        pano_pack_changed = false;
        if (pano_tex) glDeleteTextures(1, &pano_tex);
        if (pano_rgb_tex) glDeleteTextures(1, &pano_rgb_tex);
        pano_tex = pano_rgb_tex = 0;
        pano_of = bck;
        if (!bck || bck->width <= 0 || bck->height <= 0) return;
        pano_tex = make_texture_2d(GL_R8, bck->width, bck->height, GL_RED, GL_UNSIGNED_BYTE, bck->pixels.data());
        if (!pack || pack->empty()) return;
        const uint64_t hash = texture_hash(*bck, t.palette);
        if (!pack->has(hash)) return;
        auto img = pack->load(hash);
        if (!img) return;
        pano_rgb_tex = make_texture_2d(GL_RGBA8, img->width, img->height, GL_RGBA, GL_UNSIGNED_BYTE, img->pixels.data());
        glBindTexture(GL_TEXTURE_2D, pano_rgb_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        SDL_Log("hardware renderer: sky replaced (%dx%d)", img->width, img->height);
    }

    // The sky's uniforms (for the program in use, by its locations).
    void sky_uniforms(const SceneFrame& f, const SkyLoc& s_) {
        const GLint s_sky = s_.sky, s_has_pano = s_.has_pano, s_fill = s_.fill, s_wide = s_.wide, s_a = s_.a, s_b = s_.b, s_rot = s_.rot;
        const Camera& cam = *f.cam;
        gl::Uniform1i(s_sky, f.sky);
        const PcxImage* bck = f.track->background ? &*f.track->background : nullptr;
        gl::Uniform1i(s_has_pano, pano_tex ? 1 : 0);
        gl::Uniform1i(s_.has_rgb, pano_rgb_tex ? 1 : 0);
        if (pano_tex && bck) {
            const float scale = float(f.height) / 200.0f;
            const float horizon = (cam.center_y + cam.focal * std::sin(cam.pitch * kAngle)) * scale;
            const float top = horizon - 234.0f * scale;
            float s2 = std::sin(cam.roll * kAngle), c2 = std::cos(cam.roll * kAngle);
            if (!f.sky_tilt || s2 == 0.0f) {
                s2 = 0.0f;
                c2 = 1.0f;
            }
            const float cx = float(f.width) * 0.5f, cy = cam.center_y * scale;
            float x_offset;
            if (cam.fine_sky) {
                const float yaw = std::fmod(std::fmod(cam.yaw, 2048.0f) + 2048.0f, 2048.0f);
                x_offset = std::fmod((2047.0f - yaw) * 0.5f + 512.0f, 512.0f);
            } else {
                const int yaw = int(std::lround(cam.yaw)) & 0x7ff;
                x_offset = float(((0x7ff - yaw) >> 1) & 0x1ff);
            }
            const float across = f.wide ? 1.0f / f.unit_x : 320.0f / float(f.width);
            const float down = f.wide ? 1.0f / scale : across;
            const float mid = f.wide ? 160.0f - cx * across : 0.0f;
            gl::Uniform1i(s_fill, bck->pixels[size_t(bck->height - 1) * size_t(bck->width)]);
            gl::Uniform1i(s_wide, f.wide ? 1 : 0);
            gl::Uniform4f(s_a, x_offset, mid, cx, cy);
            gl::Uniform4f(s_b, across, down, scale, top);
            gl::Uniform2f(s_rot, s2, c2);
        }
    }

    void draw_sky(const SceneFrame& f) {
        gl::UseProgram(sky_prog);
        gl::ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, palette_tex);
        gl::ActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, pano_tex);
        gl::ActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, pano_rgb_tex);
        gl::ActiveTexture(GL_TEXTURE0);
        sky_uniforms(f, sky_loc);
        gl::BindVertexArray(sky_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    void add_tri(const detail::Tri& t, const detail::Fill& fl) {
        Vertex base{};
        int16_t group = -1, layer = 0;
        const int mi = fl.mat && lib ? int(fl.mat - lib->materials().data()) : -1;
        if (mi >= 0 && size_t(mi) < entries.size() && entries[size_t(mi)].group >= 0) {
            const Entry& e = entries[size_t(mi)];
            group = int16_t(e.group);
            layer = int16_t(e.layer + ((fl.frame % e.frames) + e.frames) % e.frames);
        }
        int mode = fl.mode;
        int16_t flags = 0;
        if (fl.clamp_uv) flags |= kClampUv;
        if (fl.seal_behind) flags |= kSealBehind;
        if (fl.use_texels && group >= 0) flags |= kTexels;
        if (group < 0 && (mode == int(detail::FillMode::Textured) || mode == int(detail::FillMode::Transparent) ||
                          mode == int(detail::FillMode::Translucent))) {
            if (mode == int(detail::FillMode::Translucent)) return;
            mode = int(detail::FillMode::Flat);
        }
        base.group = std::max<int16_t>(group, 0);
        base.layer = layer;
        base.mode = int16_t(mode);
        base.flags = flags;
        base.tw = float(std::max(1, fl.mat ? fl.mat->width : 1));
        base.th = float(std::max(1, fl.mat ? fl.mat->height : 1));
        base.bias = fl.depth_bias;
        base.flat = float(fl.flat);
        base.lw = group >= 0 ? entries[size_t(mi)].lw : 1.0f;
        base.lh = group >= 0 ? entries[size_t(mi)].lh : 1.0f;
        base.x0 = float(t.x0);
        base.y0 = float(t.y0);
        base.x1 = float(t.x1);
        base.y1 = float(t.y1);
        // the edges moved out (sealing): the corners of the larger triangle, in the weights of
        // the triangle's own (an edge i moved out to weight e[i] < 0)
        float e[3] = {0, 0, 0};
        const detail::Scr& a = t.a;
        const detail::Scr& b = t.b;
        const detail::Scr& c = t.c;
        if (fl.edge_slack > 0) {
            const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            detail::tri_edge_slack(t, fl.edge_slack, area, e);
        }
        const float ws[3][3] = {{1 - e[1] - e[2], e[1], e[2]}, {e[0], 1 - e[0] - e[2], e[2]}, {e[0], e[1], 1 - e[0] - e[1]}};
        for (const auto& wt : ws) {
            Vertex v = base;
            v.x = wt[0] * a.x + wt[1] * b.x + wt[2] * c.x;
            v.y = wt[0] * a.y + wt[1] * b.y + wt[2] * c.y;
            v.iz = wt[0] * a.iz + wt[1] * b.iz + wt[2] * c.iz;
            v.uz = wt[0] * a.uz + wt[1] * b.uz + wt[2] * c.uz;
            v.vz = wt[0] * a.vz + wt[1] * b.vz + wt[2] * c.vz;
            v.w0 = wt[0];
            v.w1 = wt[1];
            v.w2 = wt[2];
            verts.push_back(v);
        }
    }
};

GlScene::GlScene() : d_(std::make_unique<Impl>()) {}
GlScene::~GlScene() = default;

bool GlScene::init() {
    Impl& d = *d_;
    const std::string version = "#version 330 core\n";
    d.scene_prog = gl::make_program("scene", kSceneVs, (version + kSkyFunc + kSceneFs).c_str());
    d.sky_prog = gl::make_program("sky", kSkyVs, (version + kSkyFunc + kSkyFs).c_str());
    if (!d.scene_prog || !d.sky_prog) return false;
    gl::UseProgram(d.scene_prog);
    d.u_size = gl::GetUniformLocation(d.scene_prog, "u_size");
    d.u_darken_mul = gl::GetUniformLocation(d.scene_prog, "u_darken_mul");
    d.u_darken_add = gl::GetUniformLocation(d.scene_prog, "u_darken_add");
    d.u_fade = gl::GetUniformLocation(d.scene_prog, "u_fade");
    d.u_true_depth = gl::GetUniformLocation(d.scene_prog, "u_true_depth");
    d.u_bias_add = gl::GetUniformLocation(d.scene_prog, "u_bias_add");
    d.u_grad_scale = gl::GetUniformLocation(d.scene_prog, "u_grad_scale");
    d.u_linear = gl::GetUniformLocation(d.scene_prog, "u_linear");
    d.u_fog = gl::GetUniformLocation(d.scene_prog, "u_fog");
    d.u_a2c = gl::GetUniformLocation(d.scene_prog, "u_a2c");
    d.scene_sky_loc.find(d.scene_prog);
    gl::Uniform1i(gl::GetUniformLocation(d.scene_prog, "u_pano"), 3);
    gl::Uniform1i(gl::GetUniformLocation(d.scene_prog, "u_palette"), 0);
    gl::Uniform1i(gl::GetUniformLocation(d.scene_prog, "u_shade"), 1);
    for (int g = 0; g < kMaxGroups; ++g) {
        const std::string name = "u_tex" + std::to_string(g);
        gl::Uniform1i(gl::GetUniformLocation(d.scene_prog, name.c_str()), kFirstGroupUnit + g);
    }
    gl::UseProgram(d.sky_prog);
    gl::Uniform1i(gl::GetUniformLocation(d.sky_prog, "u_palette"), 0);
    gl::Uniform1i(gl::GetUniformLocation(d.sky_prog, "u_pano"), 3);
    d.sky_loc.find(d.sky_prog);
    {
        GLfloat a = 1.0f;
        glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &a);
        if (glGetError() == GL_NO_ERROR) d.max_aniso = std::max(1.0f, a);
        GLint s = 1;
        glGetIntegerv(0x8D57 /* GL_MAX_SAMPLES */, &s);
        if (glGetError() == GL_NO_ERROR) d.max_samples = std::max(1, s);
    }

    gl::GenVertexArrays(1, &d.sky_vao);
    gl::GenVertexArrays(1, &d.vao);
    gl::GenBuffers(1, &d.vbo);
    gl::BindVertexArray(d.vao);
    gl::BindBuffer(GL_ARRAY_BUFFER, d.vbo);
    const auto at = [](size_t o) { return reinterpret_cast<const void*>(o); };
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), at(offsetof(Vertex, x)));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), at(offsetof(Vertex, iz)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), at(offsetof(Vertex, w0)));
    gl::EnableVertexAttribArray(3);
    gl::VertexAttribIPointer(3, 4, GL_SHORT, sizeof(Vertex), at(offsetof(Vertex, group)));
    gl::EnableVertexAttribArray(4);
    gl::VertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), at(offsetof(Vertex, tw)));
    gl::EnableVertexAttribArray(5);
    gl::VertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), at(offsetof(Vertex, lw)));
    gl::EnableVertexAttribArray(6);
    gl::VertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), at(offsetof(Vertex, x0)));
    gl::BindVertexArray(0);
    return glGetError() == GL_NO_ERROR;
}

void GlScene::prepare(const Track& track) {
    d_->ensure_textures(track);
    d_->ensure_panorama(track);
}

void GlScene::set_textures(std::shared_ptr<const TexturePack> pack) {
    if (pack == d_->pack) return;
    d_->pack = std::move(pack);
    d_->pack_changed = d_->pano_pack_changed = true;
}

void GlScene::set_quality(const Quality& q) { d_->quality = q; }

GLuint GlScene::texture() const { return d_->drawn ? d_->color : 0; }
int GlScene::width() const { return d_->w; }
int GlScene::height() const { return d_->h; }

void GlScene::draw_scene(const SceneFrame& f) {
    Impl& d = *d_;
    const int ns = d.quality.msaa > 0 ? 1 << std::clamp(d.quality.msaa, 1, 3) : 0;
    if (!f.track || !f.cam || f.width <= 0 || f.height <= 0 || !d.ensure_target(f.width, f.height, ns)) return;
    d.ensure_textures(*f.track);
    d.ensure_panorama(*f.track);
    d.set_sampling(f.mipmaps);

    // the vertices of every batch, one after another
    d.verts.clear();
    std::vector<std::pair<size_t, size_t>> ranges;  // first vertex, count, per batch
    struct PartRun {
        size_t first, count;
        uint8_t part;
    };
    std::vector<std::vector<PartRun>> part_runs(f.batches.size());  // (cars) the vertices of each part
    for (size_t bi = 0; bi < f.batches.size(); ++bi) {
        const SceneBatch& b = f.batches[bi];
        const size_t first = d.verts.size();
        for (size_t i = 0; i < b.tri_count; ++i) {
            const detail::Fill& fl = b.fills[b.tris[i].fill];
            const size_t at = d.verts.size();
            d.add_tri(b.tris[i], fl);
            if (b.kind != SceneBatch::Painter || d.verts.size() == at) continue;
            auto& runs = part_runs[bi];
            if (runs.empty() || runs.back().part != fl.part) runs.push_back({at, 0, fl.part});
            runs.back().count += d.verts.size() - at;
        }
        ranges.emplace_back(first, d.verts.size() - first);
    }

    gl::BindFramebuffer(GL_FRAMEBUFFER, d.samples ? d.ms_fbo : d.fbo);
    glViewport(0, 0, f.width, f.height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    glClearDepth(0.0);
    glClearStencil(0);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    d.draw_sky(f);

    gl::UseProgram(d.scene_prog);
    gl::Uniform2f(d.u_size, float(f.width), float(f.height));
    gl::Uniform3f(d.u_darken_mul, d.darken_mul[0], d.darken_mul[1], d.darken_mul[2]);
    gl::Uniform3f(d.u_darken_add, d.darken_add[0], d.darken_add[1], d.darken_add[2]);
    gl::Uniform4f(d.u_fade, f.fade ? 1.0f : 0.0f, f.fade_iz, f.fade_start, f.fade_range);
    gl::Uniform1i(d.u_true_depth, 0);
    // the software filler's level: from 1 / sharpness texels a pixel; GL's nearest level
    // is rounded, the filler's rounded down (filtered: the levels as GL picks them)
    const bool linear = d.quality.filter > 0;
    gl::Uniform1f(d.u_grad_scale, linear ? 1.0f : std::exp2(std::log2(std::max(0.01f, f.mip_sharpness)) - 0.5f));
    gl::Uniform1i(d.u_linear, linear ? 1 : 0);
    gl::Uniform1i(d.u_fog, d.quality.fog ? 1 : 0);
    gl::Uniform1i(d.u_a2c, d.samples ? 1 : 0);
    d.sky_uniforms(f, d.scene_sky_loc);
    gl::ActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, d.pano_tex);
    gl::ActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, d.pano_rgb_tex);
    gl::ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d.palette_tex);
    gl::ActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, d.shade_tex);
    for (int g = 0; g < kMaxGroups; ++g) {
        gl::ActiveTexture(GLenum(GL_TEXTURE0 + kFirstGroupUnit + g));
        glBindTexture(GL_TEXTURE_2D_ARRAY, size_t(g) < d.groups.size() ? d.groups[size_t(g)].tex : 0);
    }
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindVertexArray(d.vao);
    gl::BindBuffer(GL_ARRAY_BUFFER, d.vbo);
    gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(d.verts.size() * sizeof(Vertex)), d.verts.data(), GL_STREAM_DRAW);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC1_COLOR);
    if (d.samples) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    // consecutive Normal batches in one draw
    for (size_t bi = 0; bi < f.batches.size();) {
        const SceneBatch::Kind kind = f.batches[bi].kind;
        const size_t first = ranges[bi].first;
        size_t count = ranges[bi].second;
        size_t next = bi + 1;
        if (kind == SceneBatch::Normal)
            while (next < f.batches.size() && f.batches[next].kind == SceneBatch::Normal) count += ranges[next++].second;
        bi = next;
        if (!count) continue;
        const auto draw = [&]() { glDrawArrays(GL_TRIANGLES, GLint(first), GLsizei(count)); };
        switch (kind) {
        case SceneBatch::Normal:
            glDepthMask(GL_TRUE);
            draw();
            break;
        case SceneBatch::Painter: {
            // A car, part by part as the software renderer draws it (Renderer::set_car_slack):
            // a part's faces in their order over each other; over the car's earlier parts by
            // depth with the slack, or whatever the depth where either is a wheel; over the
            // rest of the scene by depth. The stencil holds 0x80 for the car's pixels, 0x40
            // for a wheel's, 0x01 for the part being drawn; the part's depths are written
            // last (the last face's, as the filler leaves them).
            glEnable(GL_STENCIL_TEST);
            glClearStencil(0);
            glStencilMask(0xff);
            glClear(GL_STENCIL_BUFFER_BIT);
            for (const PartRun& r : part_runs[bi - 1]) {
                const bool wheel = (r.part & 32) != 0;
                const bool card = (r.part & 16) != 0;  // (stencil 0x20: a card's pixel)
                const auto part_draw = [&]() { glDrawArrays(GL_TRIANGLES, GLint(r.first), GLsizei(r.count)); };
                glDepthMask(GL_FALSE);
                glStencilMask(0x01);
                glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                // over the car's wheels, or anywhere on the car for a wheel: always
                glDepthFunc(GL_ALWAYS);
                glStencilFunc(GL_EQUAL, wheel ? 0x81 : 0xc1, wheel ? 0x80 : 0xc0);
                part_draw();
                // over its other parts: by depth, as if car_slack nearer (a card, or over one:
                // by depth alone)
                glDepthFunc(GL_GEQUAL);
                if (!wheel && card) {
                    glStencilFunc(GL_EQUAL, 0x81, 0xc0);
                    part_draw();
                } else if (!wheel) {
                    glStencilFunc(GL_EQUAL, 0xa1, 0xe0);
                    part_draw();
                    glStencilFunc(GL_EQUAL, 0x81, 0xe0);
                    gl::Uniform1f(d.u_bias_add, f.car_slack);
                    part_draw();
                    gl::Uniform1f(d.u_bias_add, 0.0f);
                }
                // over the rest of the scene: by depth
                glStencilFunc(GL_EQUAL, 0x01, 0x80);
                part_draw();
                // the part's depths where it was drawn, and those pixels the car's
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glDepthMask(GL_TRUE);
                glDepthFunc(GL_ALWAYS);
                glStencilMask(0xe1);
                glStencilFunc(GL_NOTEQUAL, 0x80 | (wheel ? 0x40 : 0) | (card ? 0x20 : 0), 0x01);  // (bit 0 set; written with it clear)
                part_draw();
                glDepthFunc(GL_GEQUAL);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            }
            glStencilMask(0xff);
            glClear(GL_STENCIL_BUFFER_BIT);
            glDisable(GL_STENCIL_TEST);
            glDepthMask(GL_TRUE);
            break;
        }
        case SceneBatch::BiasTest:
            // drawn where the biased depth passes (marked in the stencil), then the real depth
            // written there
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, 1, 0xff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glDepthMask(GL_FALSE);
            draw();
            glStencilFunc(GL_EQUAL, 1, 0xff);
            glStencilOp(GL_KEEP, GL_ZERO, GL_ZERO);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_ALWAYS);
            gl::Uniform1i(d.u_true_depth, 1);
            draw();
            gl::Uniform1i(d.u_true_depth, 0);
            glDepthFunc(GL_GEQUAL);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDisable(GL_STENCIL_TEST);
            break;
        case SceneBatch::Deferred:
            glDepthMask(GL_FALSE);
            draw();
            glDepthMask(GL_TRUE);
            break;
        }
    }
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDepthMask(GL_TRUE);
    gl::BindVertexArray(0);
    if (d.samples) {
        // the samples averaged into the picture
        gl::BindFramebuffer(GL_READ_FRAMEBUFFER, d.ms_fbo);
        gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, d.fbo);
        gl::BlitFramebuffer(0, 0, f.width, f.height, 0, 0, f.width, f.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    d.drawn = true;
}

} // namespace scr::video
