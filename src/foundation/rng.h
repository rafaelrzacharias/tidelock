#pragma once
// ---------------------------------------------------------------------------------------------
// rng.h - stateless keyed RNG: the sim's ONE random-number mechanism.
//
// Spec: docs/DETERMINISM.md §3 (mechanism, exact formula), §9.1/§9.5 (this file);
//   docs/FX-PALETTE.md §5 (the fx-typed draws); docs/CANON.md "Ticks, hashes, fingerprints, RNG".
// Purpose: every sim draw is a pure function of (seed, tick, system_id, carrier_id, draw) - never
//   a sequential generator, so scheduling, worker count and iteration order cannot change a
//   result (docs/DETERMINISM.md §2 rule 10). `mix64` is the splitmix64 finalizer shared by
//   rng_for and (per docs/TODO.md) tests/foundation/fx_test_util.h's seeded generator - same
//   mix, so replacing the test's inline copy with a call to this one must not move its pinned
//   trace hashes.
// Invariants: `system_id` values come from the closed enum in rng_systems.h. `draw` is a
//   per-carrier counter the CALLER owns locally within the tick - never stored across ticks
//   (docs/DETERMINISM.md §3). `rng_below`'s Lemire step has no rejection loop: a ~n/2^64 bias is
//   accepted so draw count never depends on the value drawn.
// Determinism: pure integer functions, no state, no floats, no libm. `rng_below` reuses
//   fx::mulhi64u (fx.h) rather than a second widening-multiply implementation - one fact, one
//   home for "high 64 bits of an unsigned 128-bit product".
// Threading: none - pure functions over values; safe from any worker.
// Includes: foundation/fx_palette.h (q_t, mul<R>, mulhi64u).
// ---------------------------------------------------------------------------------------------
#include "foundation/fx_palette.h"

// The splitmix64 finalizer (Steele/Lemire/Vigna): the one mix rng_for chains four times, and the
// same mix tests/foundation/fx_test_util.h's seeded generator reuses.
constexpr u64 mix64(u64 x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

constexpr u64 RNG_K0 = 0x9e3779b97f4a7c15ull;

// The one entry point every doc uses. A pure function of the five key fields; `draw` defaults to
// 0 for a carrier that draws once per tick. Never fails, never allocates.
constexpr u64 rng_for(u64 seed, u64 tick, u32 system_id, u64 carrier_id, u32 draw = 0) {
    u64 r = mix64(seed ^ RNG_K0);
    r = mix64(r + tick);
    r = mix64(r + ((u64(system_id) << 32) | u64(draw)));
    r = mix64(r + carrier_id);
    return r;
}

// Uniform in [0, n) via Lemire's multiply-shift over a 64-bit source word; no rejection loop.
// Precondition n > 0 (asserted); n == 0 returns 0 in release.
constexpr u32 rng_below(u64 r, u32 n) {
    TL_ASSERT(n > 0);
    return u32(fx::mulhi64u(r, u64(n)));
}

// Top 30 bits of the mix as a q_t in [0, 1) (docs/FX-PALETTE.md §5). Never fails.
constexpr q_t rng_q(u64 r) {
    return fx::fx_raw<q_t>(i32(r >> 34));
}

// lo + a uniform fraction of (hi - lo), via rng_q and the closed mixed-op table - compiles only
// for an R the table lists a q_t product for (docs/FX-PALETTE.md §3.1: pos_t/invmass_t,
// vel_t/omega_t, q_t/stiff_t/angle_t/dt_t, scalar_t/lambda_t). Never through doubles.
template <typename R>
constexpr R rng_range(u64 r, R lo, R hi) {
    return lo + fx::mul<R>(rng_q(r), hi - lo);
}
