// rng.h - the stateless keyed RNG (rng_for/rng_below/rng_q/rng_range, mix64).
// Spec: docs/DETERMINISM.md §3, §9.5. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/rng.h"
#include "foundation/rng_systems.h"
#include "foundation/hash.h"
#include "fx_test_util.h"

// Known-answer vectors for rng_for, computed once from the pinned mix and committed as goldens
// (docs/DETERMINISM.md §9.5). If these move, the mix changed - fix the mix, never re-pin
// (docs/TODO.md).
TL_TEST(rng_for_known_answer_vectors, "foundation,smoke,fast") {
    TL_EXPECT_EQ(rng_for(0, 0, 0, 0, 0), (u64)0x1957a7604e215178ull);
    TL_EXPECT_EQ(rng_for(1, 0, 0, 0, 0), (u64)0x92a152cb66af0c17ull);
    TL_EXPECT_EQ(rng_for(0, 1, 0, 0, 0), (u64)0x7d2a6f022d441849ull);
    TL_EXPECT_EQ(rng_for(0, 0, 1, 0, 0), (u64)0x2eb6817e205351d5ull);
    TL_EXPECT_EQ(rng_for(0, 0, 0, 1, 0), (u64)0x005a85820a61e3beull);
    TL_EXPECT_EQ(rng_for(0, 0, 0, 0, 1), (u64)0x35be14fe33fff9acull);
    TL_EXPECT_EQ(rng_for(TL_HASH_SEED, 1000000, RNG_SYS_LUAU_BASE, 0xdeadbeefull, 7),
                 (u64)0xf2e5e37808d75849ull);
}

TL_TEST(rng_for_key_independence, "foundation,smoke,fast") {
    // Each of the five key fields perturbs the output on its own (docs/TESTING.md §9.5).
    const u64 base = rng_for(1, 2, 3, 4, 5);
    TL_EXPECT_NE(base, rng_for(9, 2, 3, 4, 5));
    TL_EXPECT_NE(base, rng_for(1, 9, 3, 4, 5));
    TL_EXPECT_NE(base, rng_for(1, 2, 9, 4, 5));
    TL_EXPECT_NE(base, rng_for(1, 2, 3, 9, 5));
    TL_EXPECT_NE(base, rng_for(1, 2, 3, 4, 9));
}

TL_TEST(rng_for_default_draw_is_zero, "foundation,smoke,fast") {
    TL_EXPECT_EQ(rng_for(7, 8, 9, 10), rng_for(7, 8, 9, 10, 0));
}

TL_TEST(rng_for_two_instance_determinism, "foundation,smoke,fast") {
    // Two independently-seeded call sequences over the same key stream agree bit for bit - the
    // dual-sim precondition (docs/DETERMINISM.md §6).
    FxRng gen_a = { 0x1234u };
    FxRng gen_b = { 0x1234u };
    for (u32 i = 0; i < 64; ++i) {
        const u64 key = fx_rng_next(&gen_a);
        const u64 ra = rng_for(key, i, i * 3u, key ^ i);
        const u64 rb = rng_for(key, i, i * 3u, key ^ i);
        TL_EXPECT_EQ(ra, rb);
        (void)fx_rng_next(&gen_b);
    }
}

TL_TEST(rng_below_exhaustive_small_n, "foundation,smoke,fast") {
    // Exhaustive over small n: every result is in [0, n), and n == 1 always returns 0.
    for (u32 n = 1; n <= 64; ++n) {
        for (u32 draw = 0; draw < 200; ++draw) {
            const u64 r = rng_for(0xabc, draw, 1, draw);
            const u32 v = rng_below(r, n);
            TL_EXPECT_LT(v, n);
        }
    }
    for (u32 draw = 0; draw < 32; ++draw) {
        TL_EXPECT_EQ(rng_below(rng_for(1, draw, 1, 0), 1u), (u32)0);
    }
}

TL_TEST(rng_below_uniformity_sanity, "foundation,smoke,fast") {
    // Coarse bucket-count sanity over a large draw set, not a strict statistical test (that is
    // docs/DETERMINISM.md §9.5's job for the runner lane's property harness). n = 4: each bucket
    // should land near 1/4 of 2^16 draws.
    const u32 n = 4;
    u32 counts[4] = { 0, 0, 0, 0 };
    const u32 total = 1u << 16;
    for (u32 i = 0; i < total; ++i) {
        const u64 r = rng_for(0x5eed, i, 2, 0);
        counts[rng_below(r, n)] += 1;
    }
    const u32 expect = total / n;
    for (u32 i = 0; i < n; ++i) {
        const u32 lo = expect - expect / 10;   // within 10%
        const u32 hi = expect + expect / 10;
        TL_EXPECT_IN_RANGE(counts[i], lo, hi);
    }
}

TL_TEST(rng_q_range, "foundation,smoke,fast") {
    // rng_q is in [0, 1) for every draw: raw bits in [0, q_t::ONE).
    for (u32 i = 0; i < 4096; ++i) {
        const u64 r = rng_for(0x9001, i, 3, 0);
        const q_t q = rng_q(r);
        TL_EXPECT_GE(q.v, (i32)0);
        TL_EXPECT_LT(q.v, q_t::ONE);
    }
    // Top bits all-1 -> q_t just under 1.0; all-0 -> exactly 0.
    TL_EXPECT_EQ(rng_q(0).v, (i32)0);
    TL_EXPECT_EQ(rng_q(~u64(0)).v, q_t::ONE - 1);
}

TL_TEST(rng_range_bounds, "foundation,smoke,fast") {
    const scalar_t lo = fx::fx_int<scalar_t>(-10);
    const scalar_t hi = fx::fx_int<scalar_t>(10);
    for (u32 i = 0; i < 2048; ++i) {
        const u64 r = rng_for(0x1357, i, 4, 0);
        const scalar_t v = rng_range<scalar_t>(r, lo, hi);
        TL_EXPECT_GE(v.v, lo.v);
        TL_EXPECT_LE(v.v, hi.v);
    }
    // r == 0 -> rng_q == 0 -> the range's low endpoint exactly.
    TL_EXPECT_EQ(rng_range<scalar_t>(0, lo, hi).v, lo.v);
}

TL_TEST(mix64_matches_fx_test_util_splitmix64, "foundation,smoke,fast") {
    // The shared finalizer: fx_test_util.h's seeded generator calls mix64 with the same running
    // state update as this reference does (docs/TODO.md "same mix").
    u64 s = 0x2468ace0u;
    s += 0x9e3779b97f4a7c15ull;
    const u64 expect = mix64(s);
    FxRng gen = { 0x2468ace0u };
    TL_EXPECT_EQ(fx_rng_next(&gen), expect);
}
