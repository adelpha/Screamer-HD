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
SCR_INLINE VF vmin(VF a, VF b) { return _mm256_min_ps(a, b); }
SCR_INLINE VF vneg(VF a) { return _mm256_xor_ps(a, _mm256_set1_ps(-0.0f)); }
SCR_INLINE VF vabs(VF a) { return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), a); }
SCR_INLINE VI vishr23(VI a) { return _mm256_srli_epi32(a, 23); }
SCR_INLINE VI vimadd(VI a, VI b) { return _mm256_madd_epi16(a, b); }
SCR_INLINE VF vcmpgt(VF a, VF b) { return _mm256_cmp_ps(a, b, _CMP_GT_OS); }
SCR_INLINE VI vior(VI a, VI b) { return _mm256_or_si256(a, b); }
SCR_INLINE VI vixor(VI a, VI b) { return _mm256_xor_si256(a, b); }
SCR_INLINE VI vilane_numbers() { return _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7); }
SCR_INLINE VF vand(VF a, VF b) { return _mm256_and_ps(a, b); }
SCR_INLINE VF vor(VF a, VF b) { return _mm256_or_ps(a, b); }
SCR_INLINE VF vandnot(VF a, VF b) { return _mm256_andnot_ps(a, b); }  // ~a & b
SCR_INLINE VF vnot(VF a) { return _mm256_xor_ps(a, _mm256_castsi256_ps(_mm256_set1_epi32(-1))); }
SCR_INLINE VF vcmpge(VF a, VF b) { return _mm256_cmp_ps(a, b, _CMP_GE_OS); }
SCR_INLINE VF vcmplt(VF a, VF b) { return _mm256_cmp_ps(a, b, _CMP_LT_OS); }
SCR_INLINE VF vcmpnlt(VF a, VF b) { return _mm256_cmp_ps(a, b, _CMP_NLT_US); }
SCR_INLINE int vbits(VF a) { return _mm256_movemask_ps(a); }
SCR_INLINE VF vloadu(const float* p) { return _mm256_loadu_ps(p); }
SCR_INLINE VI viload(const int32_t* p) { return _mm256_load_si256(reinterpret_cast<const __m256i*>(p)); }
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
SCR_INLINE VI vishr15(VI a) { return _mm256_srli_epi32(a, 15); }
// the high and the low word of the products of the lanes' low words (unsigned)
SCR_INLINE VI vimulhi16(VI a, VI b) { return _mm256_mulhi_epu16(a, b); }
SCR_INLINE VI vimullo16(VI a, VI b) { return _mm256_mullo_epi16(a, b); }
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
SCR_INLINE VF vmin(VF a, VF b) { return _mm_min_ps(a, b); }
SCR_INLINE VF vneg(VF a) { return _mm_xor_ps(a, _mm_set1_ps(-0.0f)); }
SCR_INLINE VF vabs(VF a) { return _mm_andnot_ps(_mm_set1_ps(-0.0f), a); }
SCR_INLINE VI vishr23(VI a) { return _mm_srli_epi32(a, 23); }
SCR_INLINE VI vimadd(VI a, VI b) { return _mm_madd_epi16(a, b); }
SCR_INLINE VF vcmpgt(VF a, VF b) { return _mm_cmpgt_ps(a, b); }
SCR_INLINE VI vior(VI a, VI b) { return _mm_or_si128(a, b); }
SCR_INLINE VI vixor(VI a, VI b) { return _mm_xor_si128(a, b); }
SCR_INLINE VI vilane_numbers() { return _mm_setr_epi32(0, 1, 2, 3); }
SCR_INLINE VF vand(VF a, VF b) { return _mm_and_ps(a, b); }
SCR_INLINE VF vor(VF a, VF b) { return _mm_or_ps(a, b); }
SCR_INLINE VF vandnot(VF a, VF b) { return _mm_andnot_ps(a, b); }
SCR_INLINE VF vnot(VF a) { return _mm_xor_ps(a, _mm_castsi128_ps(_mm_set1_epi32(-1))); }
SCR_INLINE VF vcmpge(VF a, VF b) { return _mm_cmpge_ps(a, b); }
SCR_INLINE VF vcmplt(VF a, VF b) { return _mm_cmplt_ps(a, b); }
SCR_INLINE VF vcmpnlt(VF a, VF b) { return _mm_cmpnlt_ps(a, b); }
SCR_INLINE int vbits(VF a) { return _mm_movemask_ps(a); }
SCR_INLINE VF vloadu(const float* p) { return _mm_loadu_ps(p); }
SCR_INLINE VI viload(const int32_t* p) { return _mm_load_si128(reinterpret_cast<const __m128i*>(p)); }
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
SCR_INLINE VI vishr15(VI a) { return _mm_srli_epi32(a, 15); }
SCR_INLINE VI vimulhi16(VI a, VI b) { return _mm_mulhi_epu16(a, b); }
SCR_INLINE VI vimullo16(VI a, VI b) { return _mm_mullo_epi16(a, b); }
#endif

enum class Kind { Darken, Flat, Library, Opaque, Transparent, Translucent, OpaqueMip, TransparentMip };

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

// i mod size (0 .. size - 1) for every lane with i in -16384 .. 16383, for a size of 2 .. 32767:
// with bias a multiple of size from 16384 up, i + bias is 0 .. 65534 and has the same
// remainder; its quotient by size comes from the high word of a 16-bit multiply by
// magic = 65536 / size (rounded down), which gives the quotient or one less, and the
// remainder is put right after. All in whole numbers: nothing to round.
SCR_INLINE VI wrap_mod(VI i, VI size, VI bias, VI magic) {
    const VI j = viadd(i, bias);
    const VI r = visub(j, vimullo16(vimulhi16(j, magic), size));  // 0 .. 2 size - 1
    return visub(r, viandnot(vigt(size, r), size));               // r >= size: - size
}
// Lanes (as bits) of i with every bit above the low 15 clear; for i = u + 16384 (or that of
// u and of v together), the lanes whose coordinates are in -16384 .. 16383.
SCR_INLINE int small_lanes(VI i) { return vbits(vcastf(vieq(vishr15(i), viset1(0)))); }
SCR_INLINE int wrap_scalar(int i, int size, int mask) {
    if (mask >= 0) return i & mask;
    if ((i %= size) < 0) i += size;
    return i;
}

// A pixel on its own, from its unwrapped texel coordinates (the general case: any material,
// any coordinates).
template <Kind K>
SCR_INLINE void pixel_general(const FillContext& c, const Fill& fl, uint8_t* row, float* drow, int x, float tz, int iu, int iv) {
    if (fl.clamp_uv) {
        iu = imin2(imax2(iu, 0), fl.width - 1);
        iv = imin2(imax2(iv, 0), fl.height - 1);
    }
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
// Writes the low byte of each lane of col to p[lane] where the lane's mask is set (p[0..7]
// are read and written).
SCR_INLINE void store_pixels(uint8_t* p, __m256i col, __m256i mask) {
    const __m256i old = _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p)));
    const __m256i merged = _mm256_blendv_epi8(old, col, mask);
    const __m256i pick = _mm256_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                          0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m256i order = _mm256_setr_epi32(0, 4, 1, 1, 1, 1, 1, 1);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(p),
                     _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(_mm256_shuffle_epi8(merged, pick), order)));
}
#endif

// a < b ? b : a and b < a ? b : a, lane by lane (signed)
SCR_INLINE VI vimax(VI a, VI b) { return vixor(a, viand(vixor(a, b), vigt(b, a))); }
SCR_INLINE VI vimin(VI a, VI b) { return vixor(a, viand(vixor(a, b), vigt(a, b))); }

// The group's texels (at: their places in texels) to its pixels px[lane] and depths dp[lane],
// for the lanes of pass; with Holes, texel 0 leaves the pixel as it is. vectors: the whole
// group lies in the row and four bytes can be read at every texel.
template <bool Holes>
SCR_INLINE void put_texels(const uint8_t* texels, VI at, int pass, [[maybe_unused]] bool vectors, [[maybe_unused]] VF dz, VF tz,
                           bool writes_depth, uint8_t* px, float* dp) {
#if SCR_FILL_AVX2
    if (vectors) {
        __m256i m = lanes_from_bits(pass);
        // (lanes that are not drawn may hold any index: they are not read)
        const __m256i col = _mm256_mask_i32gather_epi32(_mm256_setzero_si256(), reinterpret_cast<const int*>(texels), at, m, 1);
        if constexpr (Holes) {
            m = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(col, _mm256_set1_epi32(0xff)), _mm256_setzero_si256()), m);
            if (_mm256_testz_si256(m, m)) return;
        }
        if (writes_depth) _mm256_storeu_ps(dp, _mm256_blendv_ps(dz, tz, _mm256_castsi256_ps(m)));
        store_pixels(px, col, m);
        return;
    }
#endif
    alignas(32) float tzs[kLanes];
    alignas(32) int32_t ats[kLanes];
    vstore(tzs, tz);
    vistore(ats, at);
    for (int j = 0; j < kLanes; ++j) {
        if (!(pass & (1 << j))) continue;
        const uint8_t col = texels[size_t(uint32_t(ats[j]))];
        if constexpr (Holes) {
            if (col == 0) continue;
        }
        if (writes_depth) dp[j] = tzs[j];
        px[j] = col;
    }
}

// floor(v) as the vector code does it: truncated, then one down where that went up
SCR_INLINE int floor_int(float v) {
    const int t = to_int(v);
    return v < float(t) ? wrapping_add(t, -1) : t;
}
SCR_INLINE int lowest_bit(int bits) {
    int j = 0;
    while (!(bits & (1 << j))) ++j;
    return j;
}

// One triangle's rows inside a band. The rows are set up several at a time (lane j is row
// y + j), then each row's pixels are filled in groups (lane j is pixel x + j).
template <Kind K>
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void fill_tri(const FillContext& c, const Tri& t, const Fill& fl, int band0, int band1) {
    const Scr& pa = t.a;
    const Scr& pb = t.b;
    const Scr& pc = t.c;
    const float area = (pb.x - pa.x) * (pc.y - pa.y) - (pb.y - pa.y) * (pc.x - pa.x);
    const float inv = 1.0f / area;
    // Along a row w0, w1 and w2 are linear in x (w = k x + m): where all three can be
    // positive bounds the span; the exact test still decides each pixel.
    const float k0 = (pb.y - pc.y) * inv, k1 = (pc.y - pa.y) * inv, k2 = -k0 - k1;
    // Road polygons reach a little past their edges (Pose::seal_edges).
    float e[3] = {0, 0, 0};
    if (fl.edge_slack > 0) tri_edge_slack(t, fl.edge_slack, area, e);
    const float diz = k0 * pa.iz + k1 * pb.iz + k2 * pc.iz;
    const float duz = k0 * pa.uz + k1 * pb.uz + k2 * pc.uz;
    const float dvz = k0 * pa.vz + k1 * pb.vz + k2 * pc.vz;
    const bool bias = fl.depth_bias != 0;
    const int ya = imax2(t.y0, band0), yb = imin2(t.y1, band1 - 1);
    const int width = c.width;
    const size_t stride = size_t(width);

    // Everything the pixels need, in locals: the frame's bytes alias everything else, so
    // the compiler would otherwise reload these after every pixel written.
    const VF lane = vlane_numbers();
    const VF group = vset1(float(kLanes));
    const VF vk0 = vset1(k0), vk1 = vset1(k1), vk2 = vset1(k2);
    const VF ve0 = vset1(e[0]), ve1 = vset1(e[1]), ve2 = vset1(e[2]);
    const VF vdiz = vset1(diz);
    const VF tie = vset1(1.0005f), one = vset1(1.0f), small = vset1(1e-3f);
    const VF vbias = vset1(fl.depth_bias);
    const bool fade = c.fade;
    const VF fade_iz = vset1(c.fade_iz), fade_start = vset1(c.fade_start), fade_range = vset1(c.fade_range);
    const bool writes_depth = fl.writes_depth;
    const bool behind = fl.seal_behind && (e[0] != 0 || e[1] != 0 || e[2] != 0);
    const VF further = vset1(0.995f), none = vset1(0.0f);
    [[maybe_unused]] const VF vduz = vset1(duz), vdvz = vset1(dvz);
    [[maybe_unused]] const int wmask = fl.wmask, hmask = fl.hmask;
    [[maybe_unused]] const VI vwmask = viset1(fl.wmask), vhmask = viset1(fl.hmask);
    [[maybe_unused]] const VI vwidth = viset1(fl.width), vheight = viset1(fl.height);
    [[maybe_unused]] const VI ubias = viset1(fl.ubias), vbias2 = viset1(fl.vbias);
    [[maybe_unused]] const VI umagic = viset1(fl.umagic), vmagic = viset1(fl.vmagic);
    [[maybe_unused]] const VI range = viset1(0x4000);
    [[maybe_unused]] const bool clamp_uv = fl.clamp_uv;
    [[maybe_unused]] const VI last_u = viset1(fl.width - 1), last_v = viset1(fl.height - 1);
    // (the mip kinds) how u/z, v/z and 1/z change from one row to the next
    [[maybe_unused]] const MipTexture* const mip = fl.mip;
    const float k0y = (pc.x - pb.x) * inv, k1y = (pa.x - pc.x) * inv, k2y = -k0y - k1y;
    [[maybe_unused]] const VF vdizy = vset1(k0y * pa.iz + k1y * pb.iz + k2y * pc.iz);
    [[maybe_unused]] const VF vduzy = vset1(k0y * pa.uz + k1y * pb.uz + k2y * pc.uz);
    [[maybe_unused]] const VF vdvzy = vset1(k0y * pa.vz + k1y * pb.vz + k2y * pc.vz);
    [[maybe_unused]] const VF sharp = vset1(c.mip_sharpness);
    [[maybe_unused]] const VI top_u = viset1(mip ? mip->levels_u - 1 : 0), top_v = viset1(mip ? mip->levels_v - 1 : 0);
    [[maybe_unused]] const uint8_t* const texels = fl.texels;
    [[maybe_unused]] const bool wide_reads = fl.wide_reads;
    [[maybe_unused]] const uint8_t flat = fl.flat;
    [[maybe_unused]] const uint8_t* const shade = c.shade;
    [[maybe_unused]] const uint8_t* const darken = c.darken;

    // the rows' setup: which edges bound the span from the left, which from the right
    const VF ax = vset1(pa.x), ay = vset1(pa.y), bx = vset1(pb.x), by = vset1(pb.y), cx = vset1(pc.x), cy = vset1(pc.y);
    const VF vinv = vset1(inv);
    const VF aiz = vset1(pa.iz), biz = vset1(pb.iz), ciz = vset1(pc.iz);
    const VF auz = vset1(pa.uz), buz = vset1(pb.uz), cuz = vset1(pc.uz);
    const VF avz = vset1(pa.vz), bvz = vset1(pb.vz), cvz = vset1(pc.vz);
    const float ks[3] = {k0, k1, k2};
    int edge_class[3];  // 1: bounds the left, 2: the right, 0: along the rows
    for (int j = 0; j < 3; ++j) edge_class[j] = ks[j] > 1e-9f ? 1 : ks[j] < -1e-9f ? 2 : 0;
    const VF half = vset1(0.5f);
    const VF whole_below = vset1(8388608.0f), whole_above = vset1(-8388608.0f);

    for (int y = ya; y <= yb; y += kLanes) {
        alignas(32) int32_t rx0[kLanes], rx1[kLanes];
        alignas(32) float rw0[kLanes], rw1[kLanes], rw2[kLanes], riz[kLanes], ruz[kLanes], rvz[kLanes];
        int rows;  // (as bits) the rows of this batch with pixels to fill
        {
            const VF py = vadd(vadd(vset1(float(y)), lane), half);
            const VF m0 = vmul(vsub(vmul(bx, vsub(cy, py)), vmul(vsub(by, py), cx)), vinv);
            const VF m1 = vmul(vsub(vmul(cx, vsub(ay, py)), vmul(vsub(cy, py), ax)), vinv);
            const VF m2 = vsub(vsub(one, m0), m1);
            const VF ms[3] = {vsub(m0, ve0), vsub(m1, ve1), vsub(m2, ve2)};
            const VF vks[3] = {vk0, vk1, vk2};
            VF lo = vset1(-1e30f), hi = vset1(1e30f);
            VF empty = vcastf(viset1(0));
            for (int j = 0; j < 3; ++j) {
                if (edge_class[j] == 1) lo = vmax(vdiv(vneg(ms[j]), vks[j]), lo);
                else if (edge_class[j] == 2) hi = vmin(vdiv(vneg(ms[j]), vks[j]), hi);
                else empty = vor(empty, vcmplt(ms[j], vset1(-1e-3f)));
            }
            // px = x + 0.5, with a pixel to spare either side
            VI x0 = viset1(t.x0), x1 = viset1(t.x1);
            {
                const VF v = vmin(vset1(1e8f), lo);  // floor
                const VI ti = vcvtt(v);
                const VF down = vand(vcmpgt(vcvt(ti), v), vand(vcmpgt(v, whole_above), vcmplt(v, whole_below)));
                const VI cand = viadd(viadd(ti, vcast(down)), viset1(-2));
                const VI use = vcast(vcmpgt(lo, vset1(-1e29f)));
                x0 = vixor(x0, viand(vixor(x0, vimax(x0, cand)), use));
            }
            {
                const VF v = vmax(vset1(-1e8f), hi);  // ceil
                const VI ti = vcvtt(v);
                const VF up = vand(vcmplt(vcvt(ti), v), vand(vcmpgt(v, whole_above), vcmplt(v, whole_below)));
                const VI cand = viadd(visub(ti, vcast(up)), viset1(1));
                const VI use = vcast(vcmplt(hi, vset1(1e29f)));
                x1 = vixor(x1, viand(vixor(x1, vimin(x1, cand)), use));
            }
            rows = ~vbits(vor(empty, vcastf(vigt(x0, x1)))) & ((1 << kLanes) - 1);
            const int count = yb - y + 1;
            if (count < kLanes) rows &= (1 << count) - 1;
            if (!rows) continue;
            // The barycentric weights and the interpolated 1/z, u/z, v/z are linear along the
            // row: they start at x0 and step one pixel at a time.
            const VF px0 = vadd(vcvt(x0), half);
            const VF w0 = vmul(vsub(vmul(vsub(bx, px0), vsub(cy, py)), vmul(vsub(by, py), vsub(cx, px0))), vinv);
            const VF w1 = vmul(vsub(vmul(vsub(cx, px0), vsub(ay, py)), vmul(vsub(cy, py), vsub(ax, px0))), vinv);
            const VF w2 = vsub(vsub(one, w0), w1);
            vistore(rx0, x0);
            vistore(rx1, x1);
            vstore(rw0, w0);
            vstore(rw1, w1);
            vstore(rw2, w2);
            vstore(riz, vadd(vadd(vmul(w0, aiz), vmul(w1, biz)), vmul(w2, ciz)));
            vstore(ruz, vadd(vadd(vmul(w0, auz), vmul(w1, buz)), vmul(w2, cuz)));
            vstore(rvz, vadd(vadd(vmul(w0, avz), vmul(w1, bvz)), vmul(w2, cvz)));
        }
        for (int r = 0; r < kLanes; ++r) {
            if (!(rows & (1 << r))) continue;
            const int yr = y + r;
            uint8_t* const row = c.pixels + size_t(yr) * stride;
            float* const drow = c.depth + size_t(yr) * stride;
            const int x0 = rx0[r], x1 = rx1[r];
            const VF sw0 = vset1(rw0[r]), sw1 = vset1(rw1[r]), sw2 = vset1(rw2[r]);
            const VF siz = vset1(riz[r]);
            [[maybe_unused]] const VF suz = vset1(ruz[r]), svz = vset1(rvz[r]);
            const float* const bayer = kBayer[yr & 3];
            VF n = lane;  // lane j is pixel x + j: start + step * (x - x0 + j)
            for (int x = x0; x <= x1; x += kLanes, n = vadd(n, group)) {
                const VF w0 = vadd(sw0, vmul(vk0, n));
                const VF w1 = vadd(sw1, vmul(vk1, n));
                const VF w2 = vadd(sw2, vmul(vk2, n));
                int in = vbits(vand(vand(vcmpge(w0, ve0), vcmpge(w1, ve1)), vcmpge(w2, ve2)));
                const int left = x1 - x + 1;
                if (left < kLanes) in &= (1 << left) - 1;
                if (!in) continue;
                const VF iz = vadd(siz, vmul(vdiz, n));
                if (fade) {
                    // the view distance: past fade_depth, fewer pixels the further, none past far_depth
                    const VF beyond = vcmplt(iz, fade_iz);
                    if (in & vbits(beyond)) {
                        const VF depth = vsub(vdiv(one, iz), vset1(128.0f));
                        const VF frac = vdiv(vsub(depth, fade_start), fade_range);
                        const VF thr = vloadu(bayer + (x & 3));
                        in &= vbits(vor(vcmplt(frac, thr), vnot(beyond)));
                        if (!in) continue;
                    }
                }
                // Coplanar surfaces (road decals on the road, shadows) are resolved like the
                // original's painter's order: the later polygon wins near-ties.
                VF tz = bias ? vdiv(iz, vmax(small, vsub(one, vmul(vbias, iz)))) : iz;
                if (behind) {
                    // (past the polygon's own edges: half a percent further away)
                    const VF inner = vand(vand(vcmpge(w0, none), vcmpge(w1, none)), vcmpge(w2, none));
                    tz = vor(vand(inner, tz), vandnot(inner, vmul(tz, further)));
                }
                // (the last group of a row at the right edge of the picture must not touch the
                // next row, which may be another thread's)
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
                        if (pass & (1 << j)) row[x + j] = darken[row[x + j]];
                } else if constexpr (K == Kind::Flat) {
#if SCR_FILL_AVX2
                    if (whole) {
                        const __m256i m = lanes_from_bits(pass);
                        if (writes_depth) _mm256_storeu_ps(drow + x, _mm256_blendv_ps(dz, tz, _mm256_castsi256_ps(m)));
                        store_pixels(row + x, _mm256_set1_epi32(flat), m);
                        continue;
                    }
#endif
                    alignas(32) float tzs[kLanes];
                    vstore(tzs, tz);
                    for (int j = 0; j < kLanes; ++j)
                        if (pass & (1 << j)) {
                            if (writes_depth) drow[x + j] = tzs[j];
                            row[x + j] = flat;
                        }
                } else {
                    // the texel under each pixel: (u/z, v/z) / (1/z), rounded down
                    const VF rz = vdiv(one, iz);
                    const VF fu = vmul(vadd(suz, vmul(vduz, n)), rz);
                    const VF fv = vmul(vadd(svz, vmul(vdvz, n)), rz);
                    // floor: truncate, then one down where that went up (below zero)
                    VI iu = vcvtt(fu), iv = vcvtt(fv);
                    iu = viadd(iu, vcast(vcmplt(fu, vcvt(iu))));
                    iv = viadd(iv, vcast(vcmplt(fv, vcvt(iv))));
                    alignas(32) float tzs[kLanes];
                    alignas(32) int32_t ius[kLanes], ivs[kLanes];
                    if constexpr (K == Kind::OpaqueMip || K == Kind::TransparentMip) {
                        // How many texels a pixel covers along u and along v (the larger of
                        // its step to the next pixel and to the next row), and from that the
                        // level for each: the whole power of two in it.
                        const VF gu = vmax(vabs(vmul(vsub(vduz, vmul(fu, vdiz)), rz)), vabs(vmul(vsub(vduzy, vmul(fu, vdizy)), rz)));
                        const VF gv = vmax(vabs(vmul(vsub(vdvz, vmul(fv, vdiz)), rz)), vabs(vmul(vsub(vdvzy, vmul(fv, vdizy)), rz)));
                        const VI zero = viset1(0), exp0 = viset1(127);
                        const VI lu = vimin(vimax(visub(vishr23(vcast(vmul(gu, sharp))), exp0), zero), top_u);
                        const VI lv = vimin(vimax(visub(vishr23(vcast(vmul(gv, sharp))), exp0), zero), top_v);
                        alignas(32) int32_t lus[kLanes], lvs[kLanes];
                        vistore(lus, lu);
                        vistore(lvs, lv);
                        const int first = lowest_bit(pass);
                        const MipAxis& au = mip->u[lus[first]];
                        const MipAxis& av = mip->v[lvs[first]];
                        // usually the whole group is at one level, and is done as one
                        bool together = (pass & vbits(vcastf(viand(vieq(lu, viset1(lus[first])), vieq(lv, viset1(lvs[first])))))) == pass;
                        if (together) {
                            const VF su = vmul(fu, vset1(au.scale)), sv = vmul(fv, vset1(av.scale));
                            VI ju = vcvtt(su), jv = vcvtt(sv);
                            ju = viadd(ju, vcast(vcmplt(su, vcvt(ju))));
                            jv = viadd(jv, vcast(vcmplt(sv, vcvt(jv))));
                            VI big = zero;
                            if (clamp_uv) {
                                ju = vimin(vimax(ju, zero), viset1(au.size - 1));
                                jv = vimin(vimax(jv, zero), viset1(av.size - 1));
                            } else {
                                if (au.mask < 0) big = viadd(ju, range);
                                if (av.mask < 0) big = vior(big, viadd(jv, range));
                            }
                            together = (pass & small_lanes(big)) == pass;
                            if (together) {
                                if (!clamp_uv) {
                                    ju = au.mask >= 0 ? viand(ju, viset1(au.mask)) : wrap_mod(ju, viset1(au.size), viset1(au.bias), viset1(au.magic));
                                    jv = av.mask >= 0 ? viand(jv, viset1(av.mask)) : wrap_mod(jv, viset1(av.size), viset1(av.bias), viset1(av.magic));
                                }
                                put_texels<K == Kind::TransparentMip>(mip->texels[lvs[first]][lus[first]], viadd(vimadd(jv, viset1(au.size)), ju),
                                                                      pass, whole, dz, tz, writes_depth, row + x, drow + x);
                            }
                        }
                        if (!together) {
                            // pixel by pixel: each at its own level
                            alignas(32) float fus[kLanes], fvs[kLanes];
                            vstore(tzs, tz);
                            vstore(fus, fu);
                            vstore(fvs, fv);
                            for (int j = 0; j < kLanes; ++j) {
                                if (!(pass & (1 << j))) continue;
                                const MipAxis& bu = mip->u[lus[j]];
                                const MipAxis& bv = mip->v[lvs[j]];
                                const int ru = floor_int(fus[j] * bu.scale), rv = floor_int(fvs[j] * bv.scale);
                                const int tu = clamp_uv ? imin2(imax2(ru, 0), bu.size - 1) : wrap_scalar(ru, bu.size, bu.mask);
                                const int tv = clamp_uv ? imin2(imax2(rv, 0), bv.size - 1) : wrap_scalar(rv, bv.size, bv.mask);
                                const uint8_t col = mip->texels[lvs[j]][lus[j]][size_t(tv) * size_t(bu.size) + size_t(tu)];
                                if constexpr (K == Kind::TransparentMip) {
                                    if (col == 0) continue;
                                }
                                if (writes_depth) drow[x + j] = tzs[j];
                                row[x + j] = col;
                            }
                        }
                    } else if constexpr (K == Kind::Library) {
                        vstore(tzs, tz);
                        vistore(ius, iu);
                        vistore(ivs, iv);
                        for (int j = 0; j < kLanes; ++j)
                            if (pass & (1 << j)) pixel_general<K>(c, fl, row, drow, x + j, tzs[j], ius[j], ivs[j]);
                    } else {
                        // The quick way wraps u and v to the texture in the vector registers;
                        // it needs coordinates of ordinary size.
                        VI big = viset1(0);
                        if (clamp_uv) {
                            iu = vimin(vimax(iu, big), last_u);
                            iv = vimin(vimax(iv, big), last_v);
                        } else {
                            if (wmask < 0) big = viadd(iu, range);
                            if (hmask < 0) big = vior(big, viadd(iv, range));
                        }
                        if ((pass & small_lanes(big)) != pass) {
                            vstore(tzs, tz);
                            vistore(ius, iu);
                            vistore(ivs, iv);
                            for (int j = 0; j < kLanes; ++j)
                                if (pass & (1 << j)) pixel_general<K>(c, fl, row, drow, x + j, tzs[j], ius[j], ivs[j]);
                            continue;
                        }
                        if (!clamp_uv) {
                            iu = wmask >= 0 ? viand(iu, vwmask) : wrap_mod(iu, vwidth, ubias, umagic);
                            iv = hmask >= 0 ? viand(iv, vhmask) : wrap_mod(iv, vheight, vbias2, vmagic);
                        }
                        const VI at = viadd(vishl8(iv), iu);  // the texture's rows are 256 apart
                        if constexpr (K == Kind::Translucent) {
                            vistore(ius, at);
                            for (int j = 0; j < kLanes; ++j)
                                if (pass & (1 << j)) {
                                    uint8_t& dst = row[x + j];
                                    dst = shade[size_t(texels[size_t(uint32_t(ius[j]))] & 63) * 256 + dst];
                                }
                        } else {
                            put_texels<K == Kind::Transparent>(texels, at, pass, whole && wide_reads, dz, tz, writes_depth, row + x, drow + x);
                        }
                    }
                }
            }
        }
    }
}

} // namespace

void SCR_FILL_NAME(fill_rows)(const FillContext& ctx, int band0, int band1, const TriRef* tris, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const Tri& t = *tris[i].tri;
        if (t.y1 < band0 || t.y0 >= band1) continue;
        const Fill& fl = *tris[i].fill;
        // the inner loop specialised for the fill (no per-pixel decisions)
        const FillMode mode = FillMode(fl.mode);
        if (mode == FillMode::Darken) fill_tri<Kind::Darken>(ctx, t, fl, band0, band1);
        else if (!(mode != FillMode::Flat && fl.use_texels)) fill_tri<Kind::Flat>(ctx, t, fl, band0, band1);
        else if (!fl.texels) fill_tri<Kind::Library>(ctx, t, fl, band0, band1);
        else if (fl.mip && mode != FillMode::Translucent)
            mode == FillMode::Transparent ? fill_tri<Kind::TransparentMip>(ctx, t, fl, band0, band1)
                                          : fill_tri<Kind::OpaqueMip>(ctx, t, fl, band0, band1);
        else if (mode == FillMode::Translucent) fill_tri<Kind::Translucent>(ctx, t, fl, band0, band1);
        else if (mode == FillMode::Transparent) fill_tri<Kind::Transparent>(ctx, t, fl, band0, band1);
        else fill_tri<Kind::Opaque>(ctx, t, fl, band0, band1);
    }
#if SCR_FILL_AVX2
    _mm256_zeroupper();
#endif
}

void SCR_FILL_NAME(wrap_coords)(const int32_t* in, int32_t* out, size_t count, int size) {
    const int bias = (16384 + size - 1) / size * size, magic = 65536 / size;
    for (size_t i = 0; i < count; i += kLanes) {
        alignas(32) int32_t v[kLanes] = {}, r[kLanes];
        for (size_t j = 0; j < size_t(kLanes) && i + j < count; ++j) v[j] = in[i + j];
        const VI vi = viload(v);
        const int ok = size >= 2 ? small_lanes(viadd(vi, viset1(0x4000))) : 0;
        vistore(r, wrap_mod(vi, viset1(size), viset1(bias), viset1(magic)));
        for (size_t j = 0; j < size_t(kLanes) && i + j < count; ++j)
            out[i + j] = (ok >> j) & 1 ? r[j] : wrap_scalar(v[j], size, -1);
    }
#if SCR_FILL_AVX2
    _mm256_zeroupper();
#endif
}

} // namespace scr::detail
