// det_trig.test.cpp - sincos/sin/cos in turns (docs/FX-PALETTE.md §10.3, §10.5 "det_trig").
// Fast half: the exact points, every symmetry over 2^20 seeded angles (bit-exact, needs no
// reference), sin^2 + cos^2, monotonicity inside each quadrant, and the correctly rounded
// mpmath table at 4096 turn fractions (tools/fxcheck/oracle.py emit-tables). Slow half: the
// identities over all 2^30 turn fractions. The true-error sweep over all 2^30 inputs against a
// long double reference is tools/fxcheck (nightly); its measured bounds are the constants in
// det_math.h that this file asserts against.
// Spec: docs/FX-PALETTE.md §10.3. Rubric: docs/TESTING.md §7.
#include "fx_test_util.h"
#include "foundation/det_math.h"
#include "det_ref_tables.inc"

using namespace fx;

static const i32 QT = 1 << 28;          // quarter turn raw
static const i32 HT = 1 << 29;          // half turn raw
static const i32 ONE_TURN = 1 << 30;

TL_TEST(det_trig_exact_points, "foundation,fx,det,smoke,fast") {
    q_t s, c;
    sincos(fx_raw<angle_t>(0), &s, &c);
    TL_EXPECT_EQ(s.v, 0);
    TL_EXPECT_EQ(c.v, q_t::ONE);
    sincos(QUARTER_TURN, &s, &c);
    TL_EXPECT_EQ(s.v, q_t::ONE);
    TL_EXPECT_EQ(c.v, 0);
    sincos(HALF_TURN, &s, &c);
    TL_EXPECT_EQ(s.v, 0);
    TL_EXPECT_EQ(c.v, -q_t::ONE);
    sincos(fx_raw<angle_t>(3 * QT), &s, &c);
    TL_EXPECT_EQ(s.v, -q_t::ONE);
    TL_EXPECT_EQ(c.v, 0);
    // a whole turn, two turns, negative turns: the mask makes them all zero
    TL_EXPECT_EQ(sin(TURN).v, 0);
    TL_EXPECT_EQ(sin(TURN + TURN).v, 0);
    TL_EXPECT_EQ(sin(-TURN).v, 0);
    TL_EXPECT_EQ(cos(-TURN).v, q_t::ONE);
    TL_EXPECT_EQ(sin(-QUARTER_TURN).v, -q_t::ONE);
    TL_EXPECT_EQ(cos(-QUARTER_TURN).v, 0);
    // +-2 turns wrap (INT32_MIN raw is exactly -2 turns = 0)
    TL_EXPECT_EQ(sin(fx_raw<angle_t>(INT32_MIN)).v, 0);
    TL_EXPECT_EQ(cos(fx_raw<angle_t>(INT32_MIN)).v, q_t::ONE);
    // 1/8 turn: sin = cos = 1/sqrt 2 within the kernel bound
    sincos(fx_raw<angle_t>(1 << 27), &s, &c);
    TL_EXPECT_TRUE(fx_near_raw(s.v, 759250125, FX_SIN_MAX_ERR_ULP));
    TL_EXPECT_TRUE(fx_near_raw(c.v, 759250125, FX_SIN_MAX_ERR_ULP));
    // 1/12 turn (30 degrees): sin = 1/2 exactly in the reference; within bound here
    TL_EXPECT_TRUE(fx_near_raw(sin(fx_lit<angle_t>(1, 12)).v, 1 << 29, FX_SIN_MAX_ERR_ULP));
    // sin and cos never leave [-1, 1]
    TL_EXPECT_LE(sin(fx_raw<angle_t>(QT + 1)).v, q_t::ONE);
    TL_EXPECT_LE(sin(fx_raw<angle_t>(QT - 1)).v, q_t::ONE);
    TL_EXPECT_GE(sin(fx_raw<angle_t>(3 * QT + 1)).v, -q_t::ONE);
    TL_EXPECT_GE(sin(fx_raw<angle_t>(3 * QT - 1)).v, -q_t::ONE);
}

// The identities every angle must satisfy bit-exactly, plus the norm bound. Returns failures.
static u32 check_identities(i32 raw, u64* worst_norm) {
    u32 bad = 0;
    const angle_t a = fx_raw<angle_t>(raw);
    q_t s, c;
    sincos(a, &s, &c);
    if (sin(a).v != s.v || cos(a).v != c.v) ++bad;                                 // sincos == (sin, cos)
    if (sin(-a).v != -s.v) ++bad;                                                  // odd
    if (cos(-a).v != c.v) ++bad;                                                   // even
    if (sin(HALF_TURN - a).v != s.v) ++bad;                                        // sin(1/2 - a) = sin a
    if (sin(a + HALF_TURN).v != -s.v) ++bad;                                       // sin(a + 1/2) = -sin a
    if (cos(a + HALF_TURN).v != -c.v) ++bad;
    if (sin(a + QUARTER_TURN).v != c.v) ++bad;                                     // sin(a + 1/4) = cos a
    if (cos(a - QUARTER_TURN).v != s.v) ++bad;                                     // cos(a - 1/4) = sin a
    if (sin(a + TURN).v != s.v || sin(a - TURN).v != s.v) ++bad;                   // period
    if (s.v > q_t::ONE || s.v < -q_t::ONE || c.v > q_t::ONE || c.v < -q_t::ONE) ++bad;
    const i64 nrm = (i64)s.v * s.v + (i64)c.v * c.v;                               // Q60
    const i64 dev = nrm - ((i64)1 << 60);
    const u64 adev = (u64)(dev < 0 ? -dev : dev) >> 30;                             // in Q30 ulps
    if (adev > *worst_norm) *worst_norm = adev;
    return bad;
}

TL_TEST(det_trig_identities_seeded, "foundation,fx,det,fast") {
    FxRng rng = { 0x74726967ull };
    u32 bad = 0;
    u64 worst_norm = 0;
    for (u32 i = 0; i < (1u << 20); ++i) bad += check_identities(fx_rng_i32(&rng), &worst_norm);
    // and the neighbourhood of every quadrant boundary
    for (i32 q = 0; q < 4; ++q) {
        for (i32 d = -64; d <= 64; ++d) bad += check_identities(q * QT + d, &worst_norm);
    }
    TL_EXPECT_EQ(bad, 0u);
    TL_EXPECT_LE(worst_norm, (u64)FX_SIN2COS2_MAX_ERR_ULP);
}

TL_TEST(det_trig_identities_exhaustive, "foundation,fx,det,slow") {
    u32 bad = 0;
    u64 worst_norm = 0;
    for (i64 raw = 0; raw < ONE_TURN; ++raw) bad += check_identities((i32)raw, &worst_norm);
    TL_EXPECT_EQ(bad, 0u);
    TL_EXPECT_LE(worst_norm, (u64)FX_SIN2COS2_MAX_ERR_ULP);
}

TL_TEST(det_trig_monotone_in_quadrant, "foundation,fx,det,fast") {
    // sin is non-decreasing on [0, 1/4] and non-increasing on [1/4, 1/2]: checked on a 2^-16
    // turn grid plus a dense band at each end (the polynomial's flattest region).
    u32 bad = 0;
    i32 prev = -1;
    for (i32 k = 0; k <= QT; k += 1 << 12) {
        const i32 v = sin(fx_raw<angle_t>(k)).v;
        if (v < prev) ++bad;
        prev = v;
    }
    prev = q_t::ONE + 1;
    for (i32 k = QT; k <= HT; k += 1 << 12) {
        const i32 v = sin(fx_raw<angle_t>(k)).v;
        if (v > prev) ++bad;
        prev = v;
    }
    prev = -1;
    for (i32 k = 0; k <= 4096; ++k) { const i32 v = sin(fx_raw<angle_t>(k)).v; if (v < prev) ++bad; prev = v; }
    prev = q_t::ONE + 1;
    for (i32 k = QT; k <= QT + 4096; ++k) { const i32 v = sin(fx_raw<angle_t>(k)).v; if (v > prev) ++bad; prev = v; }
    TL_EXPECT_EQ(bad, 0u);
    // sin is strictly positive on (0, 1/2) and strictly negative on (1/2, 1) - no sign glitches
    TL_EXPECT_GT(sin(fx_raw<angle_t>(1)).v, 0);
    TL_EXPECT_GT(sin(fx_raw<angle_t>(HT - 1)).v, 0);
    TL_EXPECT_LT(sin(fx_raw<angle_t>(HT + 1)).v, 0);
    TL_EXPECT_LT(sin(fx_raw<angle_t>(ONE_TURN - 1)).v, 0);
    // the slope at 0 is 2 pi turns^-1: sin(1 raw) = 2 pi / 2^30 * 2^30 = 6.28 -> 6
    TL_EXPECT_EQ(sin(fx_raw<angle_t>(1)).v, 6);
    TL_EXPECT_EQ(sin(fx_raw<angle_t>(-1)).v, -6);
}

TL_TEST(det_trig_vs_mpmath_table, "foundation,fx,det,fast") {
    // REF_SIN[k] = RNE(sin(2 pi k / 4096) * 2^30), k = 0..4096 - correctly rounded at 60 digits.
    u32 bad = 0;
    i64 worst = 0;
    for (i32 k = 0; k <= 4096; ++k) {
        const angle_t a = fx_raw<angle_t>(k << 18);                  // k/4096 turn
        q_t s, c;
        sincos(a, &s, &c);
        const i64 es = (i64)s.v - REF_SIN[k];
        const i64 ec = (i64)c.v - REF_SIN[(k + 1024) & 4095];        // cos(a) = sin(a + 1/4); table is periodic
        const i64 aes = es < 0 ? -es : es, aec = ec < 0 ? -ec : ec;
        if (aes > FX_SIN_MAX_ERR_ULP || aec > FX_SIN_MAX_ERR_ULP) ++bad;
        if (aes > worst) worst = aes;
        if (aec > worst) worst = aec;
    }
    TL_EXPECT_EQ(bad, 0u);
    TL_EXPECT_LE(worst, (i64)FX_SIN_MAX_ERR_ULP);
    TL_EXPECT_GT(worst, (i64)2);     // the bound is measured, not slack: the table reaches past 2 ulp
}
