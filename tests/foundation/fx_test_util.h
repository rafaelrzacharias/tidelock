#pragma once
// fx_test_util.h - shared helpers for the fx/det_math tests (tests/foundation/fx_*.test.cpp,
// det_*.test.cpp). Test code: obeys the subset (docs/TESTING.md §8 R-2) but is not sim code, so
// a 128-bit oracle and a local seeded generator are fine here.
// Spec: docs/FX-PALETTE.md §10.5; docs/TESTING.md §1 (property tests: seeded, never wall-clock).
#include "runner/tl_test.h"
#include "foundation/fx_palette.h"
#include "foundation/rng.h"

// splitmix64 - the test-side seeded generator, built on rng.h's mix64 (the same finalizer, so
// this must draw the identical stream it always has: replacing the inline copy with a call to
// the shared one must NOT move any pinned trace hash - docs/TODO.md "same mix").
struct FxRng { u64 s; };
inline u64 fx_rng_next(FxRng* r) { return mix64(r->s += 0x9e3779b97f4a7c15ull); }
inline i32 fx_rng_i32(FxRng* r) { return (i32)(u32)fx_rng_next(r); }
inline i64 fx_rng_i64(FxRng* r) { return (i64)fx_rng_next(r); }
// Uniform in [lo, hi] (inclusive), hi - lo < 2^63.
inline i64 fx_rng_range(FxRng* r, i64 lo, i64 hi) {
    return lo + (i64)(fx_rng_next(r) % ((u64)(hi - lo) + 1u));
}

// Reference RNE of the exact rational n/d written in sign-magnitude style - a different
// derivation from fx::rne_div's two's-complement one, so the two can check each other.
// Precondition d != 0, |n| and |d| < 2^62.
inline i64 ref_rne_div(i64 n, i64 d) {
    const bool neg = (n < 0) != (d < 0);
    const u64 an = n < 0 ? (u64)0 - (u64)n : (u64)n;
    const u64 ad = d < 0 ? (u64)0 - (u64)d : (u64)d;
    u64 q = an / ad;
    const u64 r = an % ad;
    // nearest: compare 2r with |d|; a tie goes to even |q|
    if (2u * r > ad || (2u * r == ad && (q & 1u))) q += 1u;
    return neg ? -(i64)q : (i64)q;
}
// Reference RNE of x / 2^s, s in [0, 62], via the rational reference.
inline i64 ref_rne_shr(i64 x, int s) { return s == 0 ? x : ref_rne_div(x, (i64)1 << s); }

// Compare two raw representations within tol ulps; prints nothing, returns bool for EXPECT.
inline bool fx_near_raw(i64 a, i64 b, i64 tol) {
    const i64 d = a > b ? a - b : b - a;
    return d <= tol;
}
