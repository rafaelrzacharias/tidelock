#pragma once
// ---------------------------------------------------------------------------------------------
// fx_float.h - the float bridge: fx -> f32/f64 for render/editor/tools, f32/f64 -> fx for
//   INPUT capture and editor writes. The ONLY place a palette value meets a float.
//
// Spec: docs/FX-PALETTE.md §6 (the bridge and its rules), §10.4 (this header).
// Purpose: render extracts once per entity per frame (to_f32); the editor and the Live input
//   producer quantise user values to a row (from_f32_quantized) and hand them to the command
//   channel - never a direct poke into sim state.
// Invariants: UNREACHABLE FROM SIM TUs - tools/audit/includes.py rejects any sim TU that
//   includes it, and this file is the one foundation header exempt from the float token ban.
//   to_f32(pos_t) is exact for |raw| < 2^24 and loses below a texel at the far edge of the
//   world (docs/FX-PALETTE.md §6). Quantisation is round-to-nearest-even at the row's quantum,
//   computed WITHOUT libm: <math.h> is banned outside render/editor/platform
//   (docs/CPP-SUBSET.md §1), so RNE uses the 2^23 / 2^52 add-subtract identity on |s|, which is
//   exact under the default rounding mode (-ffast-math is banned everywhere, §7). NOT the
//   sign-free 1.5 * 2^23 variant: that one is exact only for |s| <= 2^22, because s + 1.5 * 2^23
//   is >= 2^24 above that, where a float's spacing is 2 (W1 fx review 1 measured it: every odd
//   integer in [2^22, 2^23) came back even).
// Determinism: none claimed - float in, float out; nothing here feeds sim state except through
//   a quantised row value in a command, which is then ordinary integer data.
// Threading: none - pure functions.
// Includes: foundation/fx_palette.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/fx_palette.h"

namespace fx {

// x as a float: x.v * 2^-FRAC (the reciprocal of a power of two is exact). Render only.
template <typename A>
constexpr f32 to_f32(A x) { return f32(x.v) * (1.0f / f32(A::ONE)); }

// TEXEL (fx_palette.h) as a compile-time f32 ratio, for render-side code that needs it without a
// to_f32 CALL SITE of its own (docs/RENDER2D.md §9.5's allowlist scopes to_f32/to_f64 call sites
// to render/extract.cpp, render/simview.cpp, editor/ - review round 2 N5 found a third render-side
// site, sprite.cpp, calling to_f32(TEXEL) directly). Same derivation to_f32<pos_t> uses, inlined
// rather than called, so this definition is not itself a call site; still derived from TEXEL.v,
// the one canonical fixed-point source, never a restated "1/16" literal.
constexpr f32 TEXEL_M = f32(TEXEL.v) * (1.0f / f32(pos_t::ONE));
static_assert(TEXEL_M == 1.0f / 16.0f, "TEXEL = 1/16 m (foundation/fx_palette.h)");

// x as a double: exact for every 32-bit row (53-bit mantissa). Editor/tools.
template <typename A>
constexpr f64 to_f64(A x) { return f64(x.v) * (1.0 / f64(A::ONE)); }

// RNE of a float to an integer-valued float, without libm. Exact under round-to-nearest: for
// 0 <= s < 2^23, s + 2^23 lies in [2^23, 2^24) where the spacing is exactly 1, so the add rounds
// to the nearest integer (ties to even) and the subtract is exact; negative s mirrors.
constexpr f32 fx_rint_f32(f32 s) {
    if (s >= 8388608.0f || s <= -8388608.0f) return s;      // |s| >= 2^23: already integral
    const f32 magic = 8388608.0f;                           // 2^23
    return s < 0.0f ? (s - magic) + magic : (s + magic) - magic;
}

// RNE of a double to an integer-valued double, without libm; the f32 argument at 2^52.
constexpr f64 fx_rint_f64(f64 s) {
    if (s >= 4503599627370496.0 || s <= -4503599627370496.0) return s;   // |s| >= 2^52
    const f64 magic = 4503599627370496.0;                   // 2^52
    return s < 0.0 ? (s - magic) + magic : (s + magic) - magic;
}

// f quantised to R: RNE at the row quantum, clamped to R's range. NaN/inf assert (debug/dev)
// and quantise as 0 / the clamp. INPUT capture and editor writes only.
template <typename R>
constexpr R from_f32_quantized(f32 f) {
    static_assert(sizeof(typename R::rep) == 4, "32-bit rows only");
    TL_ASSERT(f * 0.0f == 0.0f);                            // false for NaN and +-inf
    if (!(f * 0.0f == 0.0f)) return fx_raw<R>(f != f ? 0 : (f > 0.0f ? INT32_MAX : INT32_MIN));
    const f32 s = fx_rint_f32(f * f32(R::ONE));
    if (s >= 2147483648.0f) return fx_raw<R>(INT32_MAX);
    if (s <= -2147483648.0f) return fx_raw<R>(INT32_MIN);
    return fx_raw<R>(i32(s));
}

// The f64 form of from_f32_quantized (data-table compiler, editor fields).
template <typename R>
constexpr R from_f64_quantized(f64 f) {
    static_assert(sizeof(typename R::rep) == 4, "32-bit rows only");
    TL_ASSERT(f * 0.0 == 0.0);
    if (!(f * 0.0 == 0.0)) return fx_raw<R>(f != f ? 0 : (f > 0.0 ? INT32_MAX : INT32_MIN));
    const f64 s = fx_rint_f64(f * f64(R::ONE));
    if (s >= 2147483648.0) return fx_raw<R>(INT32_MAX);
    if (s <= -2147483648.0) return fx_raw<R>(INT32_MIN);
    return fx_raw<R>(i32(s));
}

}  // namespace fx

static_assert(fx::fx_rint_f32(2.5f) == 2.0f && fx::fx_rint_f32(3.5f) == 4.0f && fx::fx_rint_f32(-2.5f) == -2.0f, "ties to even");
static_assert(fx::fx_rint_f64(0.5) == 0.0 && fx::fx_rint_f64(1.5) == 2.0 && fx::fx_rint_f64(-0.5) == 0.0, "ties to even");
static_assert(fx::fx_rint_f32(4194305.0f) == 4194305.0f && fx::fx_rint_f32(-8388607.0f) == -8388607.0f, "odd integers in [2^22, 2^23) survive");
static_assert(fx::fx_rint_f64(2251799813685249.0) == 2251799813685249.0, "2^51 + 1 survives");
static_assert(fx::from_f32_quantized<pos_t>(1.0f).v == (1 << 18) && fx::from_f32_quantized<pos_t>(-0.5f).v == -(1 << 17), "");
