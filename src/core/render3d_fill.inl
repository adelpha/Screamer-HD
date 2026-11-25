// The pixel filler, built once per instruction set: the including file defines
// SCR_FILL_AVX2 (0: SSE2, four pixels at a time; 1: AVX2, eight) and SCR_FILL_NAME(x) (the
// suffix of the public names). See render3d_fill.h.
//
// The two builds must give the same picture, so:
// - every pixel's values come from the same operations in the same order (lane j of a group
//   starting at x is pixel x + j: start + step * (x - x0 + j)), whatever the group size;
// - no multiply and add may be fused into one operation (fp_contract off);
// - comparisons, min and max keep the operand order of the SSE2 code.

#include "core/render3d_fill.h"

#include <immintrin.h>

#ifdef _MSC_VER
#pragma fp_contract(off)
#define SCR_INLINE __forceinline
#else
#pragma STDC FP_CONTRACT OFF
#define SCR_INLINE inline __attribute__((always_inline))
#endif

namespace scr::detail {
namespace {

#if SCR_FILL_AVX2
constexpr int kLanes = 8;
using VF = __m256;
using VI = __m256i;
SCR_INLINE VF vset1(float v) { return _mm256_set1_ps(v); }
SCR_INLINE VF vlane_numbers() { return _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f); }
SCR_INLINE VF vadd(VF a, VF b) { return _mm256_add_ps(a, b); }
SCR_INLINE VF vsub(VF a, VF b) { return _mm256_sub_ps(a, b); }
SCR_INLINE VF vmul(VF a, VF b) { return _mm256_mul_ps(a, b); }
SCR_INLINE VF vdiv(VF a, VF b) { return _mm256_div_ps(a, b); }
SCR_INLINE VF vmax(VF a, VF b) { return _mm256_max_ps(a, b); }
SCR_INLINE VF vand(VF a, VF b) { return _mm256_and_ps(a, b); }
SCR_INLINE VF vor(VF a, VF b) { return _mm256_or_ps(a, b); }
SCR_INLINE VF vnot(VF a) { return _mm256_xor_ps(a, _mm256_castsi256_ps(_mm256_set1_epi32(-1))); }
SCR_INLINE VF vcmpge(VF a, VF b) { return _mm256_cmp_ps(a, b, _CMP_GE_OS); }
SCR_INLINE VF vcmplt(VF a, VF b) { return _mm256_cmp_ps(a, b, _CMP_LT_OS); }
SCR_INLINE VF vcmpnlt(VF a, VF b) { return _mm256_cmp_ps(a, b, _CMP_NLT_US); }
SCR_INLINE int vbits(VF a) { return _mm256_movemask_ps(a); }
SCR_INLINE VF vloadu(const float* p) { return _mm256_loadu_ps(p); }
SCR_INLINE void vstore(float* p, VF a) { _mm256_store_ps(p, a); }
SCR_INLINE void vistore(int32_t* p, VI a) { _mm256_store_si256(reinterpret_cast<__m256i*>(p), a); }
SCR_INLINE VI vcvtt(VF a) { return _mm256_cvttps_epi32(a); }
SCR_INLINE VF vcvt(VI a) { return _mm256_cvtepi32_ps(a); }
SCR_INLINE VI vcast(VF a) { return _mm256_castps_si256(a); }
SCR_INLINE VF vcastf(VI a) { return _mm256_castsi256_ps(a); }
SCR_INLINE VI viset1(int v) { return _mm256_set1_epi32(v); }
SCR_INLINE VI viadd(VI a, VI b) { return _mm256_add_epi32(a, b); }
SCR_INLINE VI visub(VI a, VI b) { return _mm256_sub_epi32(a, b); }
SCR_INLINE VI viand(VI a, VI b) { return _mm256_and_si256(a, b); }
SCR_INLINE VI viandnot(VI a, VI b) { return _mm256_andnot_si256(a, b); }  // ~a & b
SCR_INLINE VI vigt(VI a, VI b) { return _mm256_cmpgt_epi32(a, b); }
SCR_INLINE VI vieq(VI a, VI b) { return _mm256_cmpeq_epi32(a, b); }
SCR_INLINE VI visign(VI a) { return _mm256_srai_epi32(a, 31); }
SCR_INLINE VI vishl8(VI a) { return _mm256_slli_epi32(a, 8); }
SCR_INLINE VI vishr16(VI a) { return _mm256_srli_epi32(a, 16); }
// a * b for a in -32768 .. 32767 and b in 0 .. 32767
SCR_INLINE VI vimul_small(VI a, VI b) { return _mm256_mullo_epi32(a, b); }
#else
constexpr int kLanes = 4;
using VF = __m128;
using VI = __m128i;
SCR_INLINE VF vset1(float v) { return _mm_set1_ps(v); }
SCR_INLINE VF vlane_numbers() { return _mm_setr_ps(0.0f, 1.0f, 2.0f, 3.0f); }
SCR_INLINE VF vadd(VF a, VF b) { return _mm_add_ps(a, b); }
SCR_INLINE VF vsub(VF a, VF b) { return _mm_sub_ps(a, b); }
SCR_INLINE VF vmul(VF a, VF b) { return _mm_mul_ps(a, b); }
SCR_INLINE VF vdiv(VF a, VF b) { return _mm_div_ps(a, b); }
SCR_INLINE VF vmax(VF a, VF b) { return _mm_max_ps(a, b); }
SCR_INLINE VF vand(VF a, VF b) { return _mm_and_ps(a, b); }
SCR_INLINE VF vor(VF a, VF b) { return _mm_or_ps(a, b); }
SCR_INLINE VF vnot(VF a) { return _mm_xor_ps(a, _mm_castsi128_ps(_mm_set1_epi32(-1))); }
SCR_INLINE VF vcmpge(VF a, VF b) { return _mm_cmpge_ps(a, b); }
SCR_INLINE VF vcmplt(VF a, VF b) { return _mm_cmplt_ps(a, b); }
SCR_INLINE VF vcmpnlt(VF a, VF b) { return _mm_cmpnlt_ps(a, b); }
SCR_INLINE int vbits(VF a) { return _mm_movemask_ps(a); }
SCR_INLINE VF vloadu(const float* p) { return _mm_loadu_ps(p); }
SCR_INLINE void vstore(float* p, VF a) { _mm_store_ps(p, a); }
SCR_INLINE void vistore(int32_t* p, VI a) { _mm_store_si128(reinterpret_cast<__m128i*>(p), a); }
SCR_INLINE VI vcvtt(VF a) { return _mm_cvttps_epi32(a); }
SCR_INLINE VF vcvt(VI a) { return _mm_cvtepi32_ps(a); }
SCR_INLINE VI vcast(VF a) { return _mm_castps_si128(a); }
SCR_INLINE VF vcastf(VI a) { return _mm_castsi128_ps(a); }
SCR_INLINE VI viset1(int v) { return _mm_set1_epi32(v); }
SCR_INLINE VI viadd(VI a, VI b) { return _mm_add_epi32(a, b); }
SCR_INLINE VI visub(VI a, VI b) { return _mm_sub_epi32(a, b); }
SCR_INLINE VI viand(VI a, VI b) { return _mm_and_si128(a, b); }
SCR_INLINE VI viandnot(VI a, VI b) { return _mm_andnot_si128(a, b); }
SCR_INLINE VI vigt(VI a, VI b) { return _mm_cmpgt_epi32(a, b); }
SCR_INLINE VI vieq(VI a, VI b) { return _mm_cmpeq_epi32(a, b); }
SCR_INLINE VI visign(VI a) { return _mm_srai_epi32(a, 31); }
SCR_INLINE VI vishl8(VI a) { return _mm_slli_epi32(a, 8); }
SCR_INLINE VI vishr16(VI a) { return _mm_srli_epi32(a, 16); }
// a * b for a in -32768 .. 32767 and b in 0 .. 32767: the low words multiplied (signed);
// b's high word is zero, so the high words add nothing
SCR_INLINE VI vimul_small(VI a, VI b) { return _mm_madd_epi16(a, b); }
#endif

enum class Kind { Darken, Flat, Library, Opaque, Transparent, Translucent };

// 4x4 ordered-dither thresholds (Bayer), (n + 0.5) / 16, each row three times so a group's
// lanes can be read from any column.
alignas(32) const float kBayer[4][16] = {
    {0.03125f, 0.53125f, 0.15625f, 0.65625f, 0.03125f, 0.53125f, 0.15625f, 0.65625f, 0.03125f, 0.53125f, 0.15625f, 0.65625f, 0, 0, 0, 0},
    {0.78125f, 0.28125f, 0.90625f, 0.40625f, 0.78125f, 0.28125f, 0.90625f, 0.40625f, 0.78125f, 0.28125f, 0.90625f, 0.40625f, 0, 0, 0, 0},
    {0.21875f, 0.71875f, 0.09375f, 0.59375f, 0.21875f, 0.71875f, 0.09375f, 0.59375f, 0.21875f, 0.71875f, 0.09375f, 0.59375f, 0, 0, 0, 0},
    {0.96875f, 0.46875f, 0.84375f, 0.34375f, 0.96875f, 0.46875f, 0.84375f, 0.34375f, 0.96875f, 0.46875f, 0.84375f, 0.34375f, 0, 0, 0, 0},
};

// std::min, std::max, std::floor and std::ceil as the row setup used them
SCR_INLINE float fmax2(float a, float b) { return a < b ? b : a; }
SCR_INLINE float fmin2(float a, float b) { return b < a ? b : a; }
SCR_INLINE int imax2(int a, int b) { return a < b ? b : a; }
SCR_INLINE int imin2(int a, int b) { return b < a ? b : a; }
SCR_INLINE int to_int(float v) { return _mm_cvttss_si32(_mm_set_ss(v)); }
SCR_INLINE float floor_f(float v) {
    if (!(v > -8388608.0f && v < 8388608.0f)) return v;  // whole already (or not a number)
    const float t = float(to_int(v));
    return t > v ? t - 1.0f : t;
}
SCR_INLINE float ceil_f(float v) {
    if (!(v > -8388608.0f && v < 8388608.0f)) return v;
    const float t = float(to_int(v));
    return t < v ? t + 1.0f : t;
}
SCR_INLINE int wrapping_add(int a, int b) { return int(uint32_t(a) + uint32_t(b)); }

// i mod size (0 .. size - 1) for every lane with i in -32768 .. 32767: the quotient from a
// single-precision multiply is right or one off, and the remainder is put right after.
SCR_INLINE VI wrap_mod(VI i, VI size, VF rsize) {
    const VI q = vcvtt(vmul(vcvt(i), rsize));
    VI r = visub(i, vimul_small(q, size));
    r = viadd(r, viand(size, visign(r)));           // r < 0: + size
    r = visub(r, viandnot(vigt(size, r), size));    // r >= size: - size
    return r;
}
// Lanes (as bits) whose i is in -32768 .. 32767.
SCR_INLINE int small_lanes(VI i) {
    return vbits(vcastf(vieq(vishr16(viadd(i, viset1(0x8000))), viset1(0))));
}
SCR_INLINE int wrap_scalar(int i, int size, int mask) {
    if (mask >= 0) return i & mask;
    if ((i %= size) < 0) i += size;
    return i;
}

// One row of a triangle: the barycentric weights and the interpolated 1/z, u/z, v/z at x0
// and their steps per pixel.
struct Span {
    float w0, w1, w2, iz, uz, vz;
    float k0, k1, k2, diz, duz, dvz;
    float e0, e1, e2;
    uint8_t* row;
    float* drow;
    int x0, x1, y;
};

// A pixel on its own, from its unwrapped texel coordinates (the general case: any material,
// any coordinates).
template <Kind K>
SCR_INLINE void pixel_general(const FillContext& c, const Fill& fl, uint8_t* row, float* drow, int x, float tz, int iu, int iv) {
    uint8_t col;
    if constexpr (K == Kind::Library) {
        col = library_texel(c.lib, fl.mat, iu, iv, fl.frame);
    } else {
        iu = wrap_scalar(iu, fl.width, fl.wmask);
        iv = wrap_scalar(iv, fl.height, fl.hmask);
        col = fl.texels[size_t(iv) * 256 + size_t(iu)];
    }
    const FillMode mode = FillMode(fl.mode);
    if (mode == FillMode::Transparent && col == 0) return;
    if (mode == FillMode::Translucent) {
        row[x] = c.shade[size_t(col & 63) * 256 + row[x]];
        return;
    }
    if (fl.writes_depth) drow[x] = tz;
    row[x] = col;
}

#if SCR_FILL_AVX2
// Lane masks from bits.
SCR_INLINE __m256i lanes_from_bits(int bits) {
    const __m256i bit = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
    return _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_set1_epi32(bits), bit), bit);
}
// Writes the low byte of each lane of col to p[lane] where the lane's mask is set.
SCR_INLINE void store_pixels(uint8_t* p, __m256i col, __m256i mask) {
    // byte 0 of every lane the colour, byte 1 the mask; gathered to 8 + 8 bytes
    const __m256i both = _mm256_or_si256(_mm256_and_si256(col, _mm256_set1_epi32(0xff)), _mm256_and_si256(mask, _mm256_set1_epi32(0xff00)));
    const __m256i pick = _mm256_setr_epi8(0, 4, 8, 12, 1, 5, 9, 13, -1, -1, -1, -1, -1, -1, -1, -1,
                                          0, 4, 8, 12, 1, 5, 9, 13, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m256i order = _mm256_setr_epi32(0, 4, 1, 5, 2, 2, 2, 2);
    const __m128i packed = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(_mm256_shuffle_epi8(both, pick), order));
    const __m128i old = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(p), _mm_blendv_epi8(old, packed, _mm_unpackhi_epi64(packed, packed)));
}
#endif

template <Kind K>
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void span(const FillContext& c, const Span& s, const Fill& fl, bool bias) {
    // Everything the pixels need, in locals: the frame's bytes alias everything else, so
    // the compiler would otherwise reload these after every pixel written.
    uint8_t* const row = s.row;
    float* const drow = s.drow;
    const int x0 = s.x0, x1 = s.x1, width = c.width;
    const VF lane = vlane_numbers();
    const VF sw0 = vset1(s.w0), sw1 = vset1(s.w1), sw2 = vset1(s.w2);
    const VF k0 = vset1(s.k0), k1 = vset1(s.k1), k2 = vset1(s.k2);
    const VF e0 = vset1(s.e0), e1 = vset1(s.e1), e2 = vset1(s.e2);
    const VF siz = vset1(s.iz), diz = vset1(s.diz);
    const VF tie = vset1(1.0005f), one = vset1(1.0f), small = vset1(1e-3f);
    const VF vbias = vset1(fl.depth_bias);
    const bool fade = c.fade;
    const VF fade_iz = vset1(c.fade_iz);
    for (int x = x0; x <= x1; x += kLanes) {
        // lane j is pixel x + j: start + step * (x - x0 + j)
        const VF n = vadd(vset1(float(x - x0)), lane);
        const VF w0 = vadd(sw0, vmul(k0, n));
        const VF w1 = vadd(sw1, vmul(k1, n));
        const VF w2 = vadd(sw2, vmul(k2, n));
        int in = vbits(vand(vand(vcmpge(w0, e0), vcmpge(w1, e1)), vcmpge(w2, e2)));
        const int left = x1 - x + 1;
        if (left < kLanes) in &= (1 << left) - 1;
        if (!in) continue;
        const VF iz = vadd(siz, vmul(diz, n));
        if (fade) {
            // the view distance: past fade_depth, fewer pixels the further, none past far_depth
            const VF beyond = vcmplt(iz, fade_iz);
            if (in & vbits(beyond)) {
                const VF depth = vsub(vdiv(one, iz), vset1(128.0f));
                const VF frac = vdiv(vsub(depth, vset1(c.fade_start)), vset1(c.fade_range));
                const VF thr = vloadu(&kBayer[s.y & 3][x & 3]);
                in &= vbits(vor(vcmplt(frac, thr), vnot(beyond)));
                if (!in) continue;
            }
        }
        // Coplanar surfaces (road decals on the road, shadows) are resolved like the
        // original's painter's order: the later polygon wins near-ties.
        const VF tz = bias ? vdiv(iz, vmax(small, vsub(one, vmul(vbias, iz)))) : iz;
        // (the last group of a row at the right edge of the picture must not touch the next
        // row, which may be another thread's)
        const bool whole = x + kLanes <= width;
        VF dz;
        if (whole) {
            dz = vloadu(drow + x);
        } else {
            alignas(32) float d[kLanes] = {};
            for (int j = 0; j < left; ++j) d[j] = drow[x + j];
            dz = vloadu(d);
        }
        const int pass = in & vbits(vcmpnlt(vmul(tz, tie), dz));
        if (!pass) continue;

        if constexpr (K == Kind::Darken) {
            for (int j = 0; j < kLanes; ++j)
                if (pass & (1 << j)) row[x + j] = c.darken[row[x + j]];
        } else if constexpr (K == Kind::Flat) {
#if SCR_FILL_AVX2
            if (whole) {
                const __m256i m = lanes_from_bits(pass);
                if (fl.writes_depth) _mm256_storeu_ps(drow + x, _mm256_blendv_ps(dz, tz, _mm256_castsi256_ps(m)));
                store_pixels(row + x, _mm256_set1_epi32(fl.flat), m);
                continue;
            }
#endif
            alignas(32) float tzs[kLanes];
            vstore(tzs, tz);
            for (int j = 0; j < kLanes; ++j)
                if (pass & (1 << j)) {
                    if (fl.writes_depth) drow[x + j] = tzs[j];
                    row[x + j] = fl.flat;
                }
        } else {
            // the texel under each pixel: (u/z, v/z) / (1/z), rounded down
            const VF rz = vdiv(one, iz);
            const VF fu = vmul(vadd(vset1(s.uz), vmul(vset1(s.duz), n)), rz);
            const VF fv = vmul(vadd(vset1(s.vz), vmul(vset1(s.dvz), n)), rz);
            // floor: truncate, then one down where that went up (below zero)
            VI iu = vcvtt(fu), iv = vcvtt(fv);
            iu = viadd(iu, vcast(vcmplt(fu, vcvt(iu))));
            iv = viadd(iv, vcast(vcmplt(fv, vcvt(iv))));
            alignas(32) float tzs[kLanes];
            alignas(32) int32_t ius[kLanes], ivs[kLanes];
            if constexpr (K == Kind::Library) {
                vstore(tzs, tz);
                vistore(ius, iu);
                vistore(ivs, iv);
                for (int j = 0; j < kLanes; ++j)
                    if (pass & (1 << j)) pixel_general<K>(c, fl, row, drow, x + j, tzs[j], ius[j], ivs[j]);
            } else {
                // The quick way wraps u and v to the texture in the vector registers; it needs
                // coordinates of ordinary size.
                int ok = pass;
                if (fl.wmask < 0) ok &= small_lanes(iu);
                if (fl.hmask < 0) ok &= small_lanes(iv);
                if (ok != pass) {
                    vstore(tzs, tz);
                    vistore(ius, iu);
                    vistore(ivs, iv);
                    for (int j = 0; j < kLanes; ++j)
                        if (pass & (1 << j)) pixel_general<K>(c, fl, row, drow, x + j, tzs[j], ius[j], ivs[j]);
                    continue;
                }
                iu = fl.wmask >= 0 ? viand(iu, viset1(fl.wmask)) : wrap_mod(iu, viset1(fl.width), vset1(fl.rwidth));
                iv = fl.hmask >= 0 ? viand(iv, viset1(fl.hmask)) : wrap_mod(iv, viset1(fl.height), vset1(fl.rheight));
                const VI at = viadd(vishl8(iv), iu);  // the texture's rows are 256 apart
                const uint8_t* const texels = fl.texels;
#if SCR_FILL_AVX2
                if constexpr (K != Kind::Translucent) {
                    if (whole && fl.wide_reads) {
                        __m256i m = lanes_from_bits(pass);
                        // (lanes that are not drawn may hold any index: they are not read)
                        const __m256i col =
                            _mm256_mask_i32gather_epi32(_mm256_setzero_si256(), reinterpret_cast<const int*>(texels), at, m, 1);
                        if constexpr (K == Kind::Transparent) {
                            m = _mm256_andnot_si256(
                                _mm256_cmpeq_epi32(_mm256_and_si256(col, _mm256_set1_epi32(0xff)), _mm256_setzero_si256()), m);
                            if (_mm256_testz_si256(m, m)) continue;
                        }
                        if (fl.writes_depth) _mm256_storeu_ps(drow + x, _mm256_blendv_ps(dz, tz, _mm256_castsi256_ps(m)));
                        store_pixels(row + x, col, m);
                        continue;
                    }
                }
#endif
                vstore(tzs, tz);
                vistore(ius, at);
                for (int j = 0; j < kLanes; ++j) {
                    if (!(pass & (1 << j))) continue;
                    const uint8_t col = texels[size_t(uint32_t(ius[j]))];
                    uint8_t& dst = row[x + j];
                    if constexpr (K == Kind::Translucent) {
                        dst = c.shade[size_t(col & 63) * 256 + dst];
                    } else {
                        if constexpr (K == Kind::Transparent) {
                            if (col == 0) continue;
                        }
                        if (fl.writes_depth) drow[x + j] = tzs[j];
                        dst = col;
                    }
                }
            }
        }
    }
}

} // namespace

void SCR_FILL_NAME(fill_rows)(const FillContext& ctx, int band0, int band1, const TriRef* tris, size_t count) {
    const size_t width = size_t(ctx.width);
    for (size_t i = 0; i < count; ++i) {
        const Tri& t = *tris[i].tri;
        if (t.y1 < band0 || t.y0 >= band1) continue;
        const Fill& fl = *tris[i].fill;
        const FillMode mode = FillMode(fl.mode);
        const Scr& a = t.a;
        const Scr& b = t.b;
        const Scr& c = t.c;
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        const float inv = 1.0f / area;
        // Along a row w0, w1 and w2 are linear in x (w = k x + m): where all three can be
        // positive bounds the span; the exact test in span() still decides each pixel.
        const float k0 = (b.y - c.y) * inv, k1 = (c.y - a.y) * inv, k2 = -k0 - k1;
        // Road polygons reach a little past their edges (Pose::seal_edges).
        float e[3] = {0, 0, 0};
        if (fl.edge_slack > 0) tri_edge_slack(t, fl.edge_slack, area, e);
        const float e0 = e[0], e1 = e[1], e2 = e[2];
        const float diz = k0 * a.iz + k1 * b.iz + k2 * c.iz;
        const float duz = k0 * a.uz + k1 * b.uz + k2 * c.uz;
        const float dvz = k0 * a.vz + k1 * b.vz + k2 * c.vz;
        // the inner loop specialised for the fill (no per-pixel decisions)
        const bool textured = mode != FillMode::Flat && fl.use_texels;
        const bool bias = fl.depth_bias != 0;
        const Kind kind = mode == FillMode::Darken ? Kind::Darken
                          : !textured ? Kind::Flat
                          : !fl.texels ? Kind::Library
                          : mode == FillMode::Translucent ? Kind::Translucent
                          : mode == FillMode::Transparent ? Kind::Transparent
                          : Kind::Opaque;
        const int ya = imax2(t.y0, band0), yb = imin2(t.y1, band1 - 1);
        for (int y = ya; y <= yb; ++y) {
            const float py = float(y) + 0.5f;
            int x0 = t.x0, x1 = t.x1;
            {
                const float m0 = (b.x * (c.y - py) - (b.y - py) * c.x) * inv;
                const float m1 = (c.x * (a.y - py) - (c.y - py) * a.x) * inv;
                const float m2 = 1.0f - m0 - m1;
                float lo = -1e30f, hi = 1e30f;
                bool empty = false;
                const float ks[3] = {k0, k1, k2};
                const float ms[3] = {m0 - e0, m1 - e1, m2 - e2};
                for (int j = 0; j < 3; ++j) {
                    const float k = ks[j], m = ms[j];
                    if (k > 1e-9f) lo = fmax2(lo, -m / k);
                    else if (k < -1e-9f) hi = fmin2(hi, -m / k);
                    else if (m < -1e-3f) empty = true;
                }
                if (empty) continue;
                // px = x + 0.5, with a pixel to spare either side
                if (lo > -1e29f) x0 = imax2(x0, wrapping_add(to_int(floor_f(fmin2(lo, 1e8f))), -2));
                if (hi < 1e29f) x1 = imin2(x1, wrapping_add(to_int(ceil_f(fmax2(hi, -1e8f))), 1));
            }
            if (x0 > x1) continue;
            // The barycentric weights and the interpolated 1/z, u/z, v/z are linear along the
            // row: they start at x0 and step one pixel at a time.
            const float px0 = float(x0) + 0.5f;
            Span sp;
            sp.w0 = ((b.x - px0) * (c.y - py) - (b.y - py) * (c.x - px0)) * inv;
            sp.w1 = ((c.x - px0) * (a.y - py) - (c.y - py) * (a.x - px0)) * inv;
            sp.w2 = 1.0f - sp.w0 - sp.w1;
            sp.iz = sp.w0 * a.iz + sp.w1 * b.iz + sp.w2 * c.iz;
            sp.uz = sp.w0 * a.uz + sp.w1 * b.uz + sp.w2 * c.uz;
            sp.vz = sp.w0 * a.vz + sp.w1 * b.vz + sp.w2 * c.vz;
            sp.k0 = k0; sp.k1 = k1; sp.k2 = k2;
            sp.diz = diz; sp.duz = duz; sp.dvz = dvz;
            sp.e0 = e0; sp.e1 = e1; sp.e2 = e2;
            sp.row = ctx.pixels + size_t(y) * width;
            sp.drow = ctx.depth + size_t(y) * width;
            sp.x0 = x0; sp.x1 = x1; sp.y = y;
            switch (kind) {
            case Kind::Darken: span<Kind::Darken>(ctx, sp, fl, bias); break;
            case Kind::Flat: span<Kind::Flat>(ctx, sp, fl, bias); break;
            case Kind::Library: span<Kind::Library>(ctx, sp, fl, bias); break;
            case Kind::Opaque: span<Kind::Opaque>(ctx, sp, fl, bias); break;
            case Kind::Transparent: span<Kind::Transparent>(ctx, sp, fl, bias); break;
            case Kind::Translucent: span<Kind::Translucent>(ctx, sp, fl, bias); break;
            }
        }
    }
#if SCR_FILL_AVX2
    _mm256_zeroupper();
#endif
}

void SCR_FILL_NAME(wrap_coords)(const int32_t* in, int32_t* out, size_t count, int size) {
    const float rsize = 1.0f / float(size);
    for (size_t i = 0; i < count; i += kLanes) {
        alignas(32) int32_t v[kLanes] = {}, r[kLanes];
        for (size_t j = 0; j < size_t(kLanes) && i + j < count; ++j) v[j] = in[i + j];
#if SCR_FILL_AVX2
        const VI vi = _mm256_load_si256(reinterpret_cast<const __m256i*>(v));
#else
        const VI vi = _mm_load_si128(reinterpret_cast<const __m128i*>(v));
#endif
        const int ok = small_lanes(vi);
        vistore(r, wrap_mod(vi, viset1(size), vset1(rsize)));
        for (size_t j = 0; j < size_t(kLanes) && i + j < count; ++j)
            out[i + j] = (ok >> j) & 1 ? r[j] : wrap_scalar(v[j], size, -1);
    }
#if SCR_FILL_AVX2
    _mm256_zeroupper();
#endif
}

} // namespace scr::detail
