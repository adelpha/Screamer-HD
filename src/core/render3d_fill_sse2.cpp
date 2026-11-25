// The pixel filler for any x64 CPU (SSE2).
#define SCR_FILL_AVX2 0
#define SCR_FILL_NAME(x) x##_sse2
#include "core/render3d_fill.inl"
