// det_sqrt.test.cpp - isqrt32/isqrt64, sqrt<R>, rsqrt<R> (docs/FX-PALETTE.md §10.3, §10.5
// "det_sqrt"). The floor property needs no reference: y*y <= x < (y+1)*(y+1) is the definition.
// The nearest property of sqrt<R> is an exact integer inequality too. Exhaustive isqrt32 over
// 2^32 inputs is the `slow` test (~1 min); the fast half is every perfect square and its
// neighbours plus 2^20 seeded inputs.
// Spec: docs/FX-PALETTE.md §10.3. Rubric: docs/TESTING.md §7.
#include "fx_test_util.h"
#include "foundation/det_math.h"

using namespace fx;

static bool floor_ok32(u32 x) {
    const u64 y = isqrt32(x);
    return y * y <= x && x < (y + 1) * (y + 1);
}

static bool floor_ok64(u64 x) {
    const u64 y = isqrt64(x);
    if (y > 0xffffffffull) return false;
    const bool lo = y * y <= x;                                     // y < 2^32: no overflow
    const bool hi = y == 0xffffffffull || x < (y + 1) * (y + 1);    // (2^32)^2 does not fit
    return lo && hi;
}

TL_TEST(det_isqrt_edges_and_squares, "foundation,fx,det,fast") {
    TL_EXPECT_EQ(isqrt32(0u), 0u);
    TL_EXPECT_EQ(isqrt32(1u), 1u);
    TL_EXPECT_EQ(isqrt32(2u), 1u);
    TL_EXPECT_EQ(isqrt32(3u), 1u);
    TL_EXPECT_EQ(isqrt32(4u), 2u);
    TL_EXPECT_EQ(isqrt32(0xffffffffu), 65535u);
    TL_EXPECT_EQ(isqrt32(65535u * 65535u), 65535u);
    TL_EXPECT_EQ(isqrt32(65535u * 65535u - 1u), 65534u);
    TL_EXPECT_EQ(isqrt64(0u), (u64)0);
    TL_EXPECT_EQ(isqrt64(~(u64)0), (u64)0xffffffffu);
    TL_EXPECT_EQ(isqrt64((u64)1 << 62), (u64)1 << 31);
    TL_EXPECT_EQ(isqrt64(((u64)1 << 62) - 1), ((u64)1 << 31) - 1);
    // every perfect square up to 2^16 and its two neighbours, both widths
    u32 bad = 0;
    for (u64 y = 0; y <= 65535; ++y) {
        const u64 sq = y * y;
        if (isqrt32((u32)sq) != y) ++bad;
        if (isqrt64(sq) != y) ++bad;
        if (sq > 0 && isqrt32((u32)(sq - 1)) != y - 1) ++bad;
        if (sq + 2 * y <= 0xffffffffull && isqrt32((u32)(sq + 2 * y)) != y) ++bad;
        if (sq + 2 * y + 1 <= 0xffffffffull && isqrt32((u32)(sq + 2 * y + 1)) != y + 1) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
    // squares of 64-bit roots around the top of the range
    for (u64 y = 0xffffff00ull; y <= 0xffffffffull; ++y) {
        if (isqrt64(y * y) != y) ++bad;
        if (isqrt64(y * y - 1) != y - 1) ++bad;
        if (y < 0xffffffffull && isqrt64(y * y + 2 * y) != y) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
}

TL_TEST(det_isqrt_property_seeded, "foundation,fx,det,fast") {
    FxRng rng = { 0x6973717274ull };
    u32 bad = 0;
    for (u32 i = 0; i < (1u << 20); ++i) {
        const u64 r = fx_rng_next(&rng);
        const u32 shift = (u32)(fx_rng_next(&rng) & 63u);
        if (!floor_ok32((u32)r)) ++bad;
        if (!floor_ok64(r >> shift)) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
}

TL_TEST(det_isqrt32_exhaustive, "foundation,fx,det,slow") {
    u32 bad = 0;
    for (u64 x = 0; x <= 0xffffffffull; ++x) {
        if (!floor_ok32((u32)x)) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
}

// nearest: (y - 1/2)^2 <= n <= (y + 1/2)^2  <=>  4y^2 - 4y + 1 <= 4n <= 4y^2 + 4y + 1 (no ties:
// 4n is even, the bounds are odd). Exact in unsigned __int128 (test code, not sim).
static bool nearest_ok(u64 n, u64 y) {
    const unsigned __int128 n4 = (unsigned __int128)n * 4;
    const unsigned __int128 yy = (unsigned __int128)y * y;
    if (y == 0) return n4 <= 1;
    return (4 * yy - 4 * y + 1) <= n4 && n4 <= (4 * yy + 4 * y + 1);
}

TL_TEST(det_sqrt_nearest_property, "foundation,fx,det,fast") {
    // sqrt<pos_t>(pos2_wide_t): shift 0 - the squared-distance path
    FxRng rng = { 0x73717274706f73ull };
    u32 bad = 0;
    for (u32 i = 0; i < (1u << 20); ++i) {
        const u64 n = fx_rng_next(&rng) >> (3u + (fx_rng_next(&rng) & 31u));   // < 2^61, varied magnitude
        const pos_t y = sqrt<pos_t>(fx_raw<pos2_wide_t>((i64)n));
        if (y.v < 0 || !nearest_ok(n, (u64)y.v)) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
    // sqrt<pos_t>(pos_t): shift 18 - sqrt of a length in metres
    for (u32 i = 0; i < (1u << 20); ++i) {
        const i32 x = fx_rng_i32(&rng) & INT32_MAX;
        const pos_t y = sqrt<pos_t>(fx_raw<pos_t>(x));
        if (!nearest_ok((u64)x << 18, (u64)y.v)) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
    // sqrt<q_t>(q_t): shift 30 - sqrt of a unit-range scalar (kernel use)
    for (u32 i = 0; i < (1u << 20); ++i) {
        const i32 x = fx_rng_i32(&rng) & INT32_MAX;
        const q_t y = sqrt<q_t>(fx_raw<q_t>(x));
        if (!nearest_ok((u64)x << 30, (u64)y.v)) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
    // exact values
    TL_EXPECT_EQ(sqrt<pos_t>(fx_int<pos_t>(0)).v, 0);
    TL_EXPECT_EQ(sqrt<pos_t>(fx_int<pos_t>(1)).v, fx_int<pos_t>(1).v);
    TL_EXPECT_EQ(sqrt<pos_t>(fx_int<pos_t>(4)).v, fx_int<pos_t>(2).v);
    TL_EXPECT_EQ(sqrt<pos_t>(fx_int<pos_t>(8100)).v, fx_int<pos_t>(90).v);
    TL_EXPECT_EQ(sqrt<pos_t>(fx_lit<pos_t>(1, 4)).v, fx_lit<pos_t>(1, 2).v);
    TL_EXPECT_EQ(sqrt<q_t>(fx_int<q_t>(1)).v, q_t::ONE);
    TL_EXPECT_EQ(sqrt<q_t>(fx_lit<q_t>(1, 4)).v, fx_lit<q_t>(1, 2).v);
    TL_EXPECT_EQ(sqrt<q_t>(fx_lit<q_t>(1, 2)).v, 759250125);            // RNE(2^30 / sqrt 2) = 759250124.99
    TL_EXPECT_EQ(sqrt<pos_t>(fx_raw<pos2_wide_t>((i64)3 << 36)).v, 454047);   // sqrt(3) m: 454046.73 quanta
    TL_EXPECT_EQ(sqrt<pos_t>(fx_raw<pos2_wide_t>(2)).v, 1);             // sqrt(2 quanta^2) = 1.41 -> 1
    TL_EXPECT_EQ(sqrt<pos_t>(fx_raw<pos2_wide_t>(3)).v, 2);             // 1.73 -> 2
    TL_EXPECT_EQ(sqrt<pos_t>(fx_raw<pos_t>(1)).v, 512);                 // sqrt(2^-18) = 2^-9 exactly
    // the largest pos_t (8,192 m) squared still round-trips
    TL_EXPECT_EQ(sqrt<pos_t>(fx_raw<pos2_wide_t>(mul_wide(fx_raw<pos_t>(INT32_MAX), fx_raw<pos_t>(INT32_MAX)))).v, INT32_MAX);
}

TL_TEST(det_rsqrt_is_div_of_sqrt, "foundation,fx,det,fast") {
    // rsqrt<R>(x) == div<R>(1, sqrt<R>(x)) by definition; the result must fit R, so q_t inputs
    // are >= 1/4 (result <= 2) and pos_t inputs are >= 1/2^26 m (result <= 8,192)
    TL_EXPECT_EQ(rsqrt<q_t>(fx_int<q_t>(1)).v, q_t::ONE);
    TL_EXPECT_EQ(rsqrt<q_t>(fx_lit<q_t>(1, 2)).v, 1518500250);                // RNE(2^30 * sqrt 2) = 1518500249.9
    TL_EXPECT_EQ(rsqrt<pos_t>(fx_int<pos_t>(4)).v, fx_lit<pos_t>(1, 2).v);    // 1/sqrt(4 m) = 0.5
    TL_EXPECT_EQ(rsqrt<pos_t>(fx_lit<pos_t>(1, 4)).v, fx_int<pos_t>(2).v);    // 1/sqrt(1/4) = 2
    TL_EXPECT_EQ(rsqrt<pos_t>(fx_int<pos_t>(1)).v, fx_int<pos_t>(1).v);
    FxRng rng = { 0x7273717274ull };
    u32 bad = 0;
    for (u32 i = 0; i < (1u << 16); ++i) {
        const i32 x = (i32)(((u32)fx_rng_i32(&rng) & 0x3fffffffu) | 0x40000000u);   // q_t in [1, 2)
        const q_t r = rsqrt<q_t>(fx_raw<q_t>(x));
        if (r.v != div<q_t>(fx_int<q_t>(1), sqrt<q_t>(fx_raw<q_t>(x))).v) ++bad;
        if (r.v > q_t::ONE || r.v < 759250125 - 1) ++bad;                          // in [1/sqrt 2, 1]
    }
    TL_EXPECT_EQ(bad, 0u);
}
