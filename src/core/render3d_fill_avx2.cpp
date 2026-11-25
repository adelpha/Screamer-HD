// The pixel filler for CPUs with AVX2 (this file alone is built for it; the Renderer uses
// it when the CPU can).
#define SCR_FILL_AVX2 1
#define SCR_FILL_NAME(x) x##_avx2
#include "core/render3d_fill.inl"
