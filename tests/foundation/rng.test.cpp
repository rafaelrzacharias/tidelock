// rng.h - the stateless keyed RNG (rng_for/rng_below/rng_q/rng_range, mix64).
// Spec: docs/DETERMINISM.md §3, §9.5. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/rng.h"
#include "foundation/rng_systems.h"
#include "foundation/hash.h"
#include "fx_test_util.h"

// Known-answer vectors for rng_for, cross-checked against tools/rapidhash_ref.py's independent
// implementation of docs/DETERMINISM.md §3's formula (`--check`), not just against the header
// they guard. If these move, the mix changed - fix the mix, never re-pin (docs/TODO.md).
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

TL_TEST(rng_edge_matrix, "foundation,smoke,fast") {
    // The corners the lane's tests stopped short of (W1 rng/hash review): the widest n rng_below
    // takes, the top of the source word, and the widest carrier_id.
    TL_EXPECT_EQ(rng_below(~u64(0), 0xffffffffu), (u32)0xfffffffe);   // r = max -> n - 1, never n
    TL_EXPECT_EQ(rng_below(u64(0), 0xffffffffu), (u32)0);
    TL_EXPECT_LT(rng_below(rng_for(7, 7, 7, 7), 0xffffffffu), 0xffffffffu);
    // carrier_id = 2^64-1 is a legal key (entity ids are u64) and must not alias carrier 0.
    TL_EXPECT_NE(rng_for(0, 0, 1, ~u64(0)), rng_for(0, 0, 1, 0));
    TL_EXPECT_EQ(rng_for(0, 0, 0, ~u64(0), 0), (u64)0x631753f755f459e8ull);
    // Every R the mixed-op table lists a q_t product for must instantiate rng_range
    // (docs/FX-PALETTE.md §3.1); this is a compile-time check with a cheap runtime assertion.
    TL_EXPECT_EQ(rng_range<q_t>(0, fx::fx_raw<q_t>(0), fx::fx_raw<q_t>(1024)).v, (i32)0);
    TL_EXPECT_EQ(rng_range<vel_t>(0, fx::fx_int<vel_t>(-1), fx::fx_int<vel_t>(1)).v,
                 fx::fx_int<vel_t>(-1).v);
    TL_EXPECT_EQ(rng_range<pos_t>(0, fx::fx_int<pos_t>(0), fx::fx_int<pos_t>(1)).v, (i32)0);
    TL_EXPECT_EQ(rng_range<scalar_t>(0, fx::fx_int<scalar_t>(0), fx::fx_int<scalar_t>(1)).v, (i32)0);
}

TL_TEST(rng_below_uniformity, "foundation,det,fast") {   // measured 287 ms for 2 x 2^24 draws
    // docs/DETERMINISM.md §9.5 asks for uniformity over 2^24 draws within 0.5%; the lane shipped
    // 2^16 within 10%, which is ~256x fewer draws at 20x the tolerance - a bias big enough to
    // matter would have passed it. Both an n that divides 2^64 exactly (8) and one that does not
    // (5, where Lemire's no-rejection bias actually lives) are measured, and the per-bucket count
    // is asserted per n, never as a sum (LESSONS.md: a vacuous row hides behind a full one).
    // Margin: at n = 8 the 0.5% band is 7.7 sigma of the binomial, at n = 5 it is 10 sigma, so
    // the tolerance is bounded by the claim and not by what happens to be green.
    const u32 total = 1u << 24;
    const u32 ns[2] = { 8u, 5u };
    for (u32 k = 0; k < 2; ++k) {
        const u32 n = ns[k];
        u32 counts[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        for (u32 i = 0; i < total; ++i) {
            counts[rng_below(rng_for(0x5eed, i, 2, k), n)] += 1;
        }
        const u32 expect = total / n;
        const u32 band = expect / 200;                     // 0.5%
        u32 checked = 0;
        for (u32 i = 0; i < n; ++i) {
            TL_EXPECT_IN_RANGE(counts[i], expect - band, expect + band);
            checked += counts[i];
        }
        TL_EXPECT_EQ(checked, total);                      // every draw landed in a bucket
    }
}

TL_TEST(rng_for_system_id_draw_packing_is_injective, "foundation,smoke,fast") {
    // (u64(system_id) << 32) | draw is the one place two key fields share a mixer input, so it is
    // the one place a (system, draw) pair could alias another - a silent cross-system collision
    // is exactly the bug the closed enum exists to prevent (docs/DETERMINISM.md §3). The packing
    // is injective by construction; this measures it, including at the field boundary where
    // draw = 2^32-1 sits one below system_id + 1, draw = 0.
    TL_EXPECT_NE(rng_for(3, 4, 0, 5, 0xffffffffu), rng_for(3, 4, 1, 5, 0));
    TL_EXPECT_NE(rng_for(3, 4, 1, 5, 0xffffffffu), rng_for(3, 4, 2, 5, 0));
    TL_EXPECT_NE(rng_for(3, 4, RNG_SYS_LUAU_BASE, 5, 0),
                 rng_for(3, 4, RNG_SYS_LUAU_BASE + 255u, 5, 0));
    // Distinctness over a dense block of the (system_id, draw) plane: 64 x 64 keys, no repeats.
    u64 seen[64 * 64];
    u32 m = 0;
    for (u32 s = 0; s < 64; ++s) {
        for (u32 d = 0; d < 64; ++d) { seen[m++] = rng_for(9, 9, s, 9, d); }
    }
    u32 collisions = 0;
    for (u32 i = 0; i < m; ++i) {
        for (u32 j = i + 1; j < m; ++j) { if (seen[i] == seen[j]) { collisions += 1; } }
    }
    TL_EXPECT_EQ(collisions, (u32)0);
    TL_EXPECT_EQ(m, (u32)(64 * 64));
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

TL_TEST(rng_range_closed_at_both_ends, "foundation,smoke,fast") {
    // The contract that was neither stated nor tested until the W1 rng/hash review: rng_q is
    // [0, 1) but rng_range is CLOSED - mul<R> rounds RNE, so the top draw of any span narrower
    // than 2^29 raw units lands exactly on hi (docs/DETERMINISM.md §3). Pinned so a future
    // "make it half-open" change cannot happen silently, and so a caller who reads [lo, hi) out
    // of rng_q's range is contradicted by a test instead of by a desync.
    const scalar_t lo = fx::fx_int<scalar_t>(-10);
    const scalar_t hi = fx::fx_int<scalar_t>(10);
    TL_EXPECT_EQ(rng_range<scalar_t>(~u64(0), lo, hi).v, hi.v);       // hi IS attainable
    TL_EXPECT_EQ(rng_range<scalar_t>(u64(0), lo, hi).v, lo.v);
    // Same for a pos_t span, and for the degenerate lo == hi range (the precondition's boundary).
    const pos_t plo = fx::fx_int<pos_t>(-2);
    const pos_t phi = fx::fx_int<pos_t>(2);
    TL_EXPECT_EQ(rng_range<pos_t>(~u64(0), plo, phi).v, phi.v);
    TL_EXPECT_EQ(rng_range<pos_t>(~u64(0), plo, plo).v, plo.v);
}

TL_TEST(rng_range_widest_legal_span, "foundation,smoke,fast") {
    // The span precondition from the legal side: INT32_MAX raw units is the widest rng_range
    // accepts. One raw unit more - a full-world pos_t span, 2^31 - wraps the subtraction to
    // INT32_MIN and returned values metres outside the world before the review added the assert;
    // it cannot be exercised here because it now traps in dev (docs/DETERMINISM.md §3).
    const scalar_t lo = fx::fx_raw<scalar_t>(-1);
    const scalar_t hi = fx::fx_raw<scalar_t>(INT32_MAX - 1);
    for (u32 i = 0; i < 1024; ++i) {
        const scalar_t v = rng_range<scalar_t>(rng_for(0x2468, i, 4, 1), lo, hi);
        TL_EXPECT_GE(v.v, lo.v);
        TL_EXPECT_LE(v.v, hi.v);
    }
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
