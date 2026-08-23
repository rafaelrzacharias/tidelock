// det_math.cpp - the kernels of det_math.h (docs/FX-PALETTE.md §10.3). Header-first stub: the
// signatures are published so dependent lanes compile; the bodies land in the next fx commit.
#include "foundation/det_math.h"

namespace fx {

u32 isqrt32(u32 x) { (void)x; TL_FATAL("det_math: isqrt32 unimplemented (header-first stub)"); }
u64 isqrt64(u64 x) { (void)x; TL_FATAL("det_math: isqrt64 unimplemented (header-first stub)"); }
void sincos(angle_t a, q_t* s, q_t* c) { (void)a; (void)s; (void)c; TL_FATAL("det_math: sincos unimplemented (header-first stub)"); }
q_t sin(angle_t a) { (void)a; TL_FATAL("det_math: sin unimplemented (header-first stub)"); }
q_t cos(angle_t a) { (void)a; TL_FATAL("det_math: cos unimplemented (header-first stub)"); }
angle_t atan2(pos_t y, pos_t x) { (void)y; (void)x; TL_FATAL("det_math: atan2 unimplemented (header-first stub)"); }
angle_t atan2q(q_t y, q_t x) { (void)y; (void)x; TL_FATAL("det_math: atan2q unimplemented (header-first stub)"); }

}  // namespace fx
