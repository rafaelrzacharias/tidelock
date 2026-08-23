// det_atan2.test.cpp - atan2 / atan2q in turns (docs/FX-PALETTE.md §10.3, §10.5 "det_atan2").
// Exact axes and diagonals, every octant boundary, symmetries that need no reference (bit-exact),
// the round trip atan2(sin a, cos a) == a within the measured bound over 2^16 sampled angles,
// and the mpmath table atan(k/4096) at 4096 ratios. The 2^24-sample true-error sweep is
// tools/fxcheck (nightly); its bound is FX_ATAN2_MAX_ERR_ULP in det_math.h.
// Spec: docs/FX-PALETTE.md §10.3. Rubric: docs/TESTING.md §7.
#include "fx_test_util.h"
#include "foundation/det_math.h"
#include "det_ref_tables.inc"

using namespace fx;

static const i32 QT = 1 << 28;
static const i32 HT = 1 << 29;
static const i32 ET = 1 << 27;          // eighth turn

TL_TEST(det_atan2_axes_and_diagonals_exact, "foundation,fx,det,smoke,fast") {
    static const i32 mags[] = { 1, 2, 3, 7, 1000, 1 << 14, 1 << 18, (1 << 30) - 1, 1 << 30, INT32_MAX };
    for (i32 m : mags) {
        // axes
        TL_EXPECT_EQ(atan2q(fx_raw<q_t>(0), fx_raw<q_t>(m)).v, 0);
        TL_EXPECT_EQ(atan2q(fx_raw<q_t>(0), fx_raw<q_t>(-m)).v, HT);           // +1/2, never -1/2
        TL_EXPECT_EQ(atan2q(fx_raw<q_t>(m), fx_raw<q_t>(0)).v, QT);
        TL_EXPECT_EQ(atan2q(fx_raw<q_t>(-m), fx_raw<q_t>(0)).v, -QT);
        // diagonals
        TL_EXPECT_EQ(atan2q(fx_raw<q_t>(m), fx_raw<q_t>(m)).v, ET);
        TL_EXPECT_EQ(atan2q(fx_raw<q_t>(m), fx_raw<q_t>(-m)).v, 3 * ET);
        TL_EXPECT_EQ(atan2q(fx_raw<q_t>(-m), fx_raw<q_t>(-m)).v, -3 * ET);
        TL_EXPECT_EQ(atan2q(fx_raw<q_t>(-m), fx_raw<q_t>(m)).v, -ET);
        // the pos_t overload is the same function
        TL_EXPECT_EQ(atan2(fx_raw<pos_t>(m), fx_raw<pos_t>(m)).v, ET);
        TL_EXPECT_EQ(atan2(fx_raw<pos_t>(0), fx_raw<pos_t>(-m)).v, HT);
    }
    // INT32_MIN magnitudes (the abs that would overflow an i32)
    TL_EXPECT_EQ(atan2q(fx_raw<q_t>(0), fx_raw<q_t>(INT32_MIN)).v, HT);
    TL_EXPECT_EQ(atan2q(fx_raw<q_t>(INT32_MIN), fx_raw<q_t>(0)).v, -QT);
    TL_EXPECT_EQ(atan2q(fx_raw<q_t>(INT32_MIN), fx_raw<q_t>(INT32_MIN)).v, -3 * ET);
    TL_EXPECT_TRUE(fx_near_raw(atan2q(fx_raw<q_t>(INT32_MIN), fx_raw<q_t>(INT32_MAX)).v, -ET, FX_ATAN2_MAX_ERR_ULP));
    // known values: atan(1/2) = 0.073791 turn, atan(2) = 0.176208 turn
    TL_EXPECT_TRUE(fx_near_raw(atan2q(fx_raw<q_t>(1), fx_raw<q_t>(2)).v, 79233351, FX_ATAN2_MAX_ERR_ULP));     // RNE(atan(0.5)/2pi * 2^30), mpmath
    TL_EXPECT_TRUE(fx_near_raw(atan2q(fx_raw<q_t>(2), fx_raw<q_t>(1)).v, 189202105, FX_ATAN2_MAX_ERR_ULP));    // 2^28 - 79233351
    // tiny ratios: atan(1/2^30) = 1/(2 pi 2^30) turn = 0.16 ulp -> 0
    TL_EXPECT_EQ(atan2q(fx_raw<q_t>(1), fx_raw<q_t>(1 << 30)).v, 0);
    TL_EXPECT_EQ(atan2q(fx_raw<q_t>(1), fx_raw<q_t>(-(1 << 30))).v, HT);
    TL_EXPECT_EQ(atan2q(fx_raw<q_t>(-1), fx_raw<q_t>(-(1 << 30))).v, -HT);       // the branch cut: just below the negative x axis
}

// Symmetries that must hold bit-exactly by construction of the octant unfold.
static u32 check_symmetries(i32 y, i32 x) {
    if (x == 0 && y == 0) return 0;
    if (x == INT32_MIN || y == INT32_MIN) return 0;      // negation not representable
    u32 bad = 0;
    const i32 r = atan2q(fx_raw<q_t>(y), fx_raw<q_t>(x)).v;
    if (y != 0 && atan2q(fx_raw<q_t>(-y), fx_raw<q_t>(x)).v != -r) ++bad;                  // mirror in x
    if (y != 0 || x > 0) {
        const i32 m = atan2q(fx_raw<q_t>(y), fx_raw<q_t>(-x)).v;                             // mirror in y: 1/2 - r (sign of y)
        const i32 want = y >= 0 ? HT - r : -HT - r;
        if (m != want) ++bad;
    }
    if (x != 0 && y != 0) {
        // swap: atan2(x, y) == 1/4 - atan2(y, x) modulo one turn, bit-exactly (the same
        // polynomial evaluation on the same ratio, unfolded the other way)
        const i32 t = atan2q(fx_raw<q_t>(x), fx_raw<q_t>(y)).v;
        if (((u32)t - (u32)(QT - r)) & ((1u << 30) - 1u)) ++bad;
    }
    if (r > HT || r <= -HT) ++bad;                                                           // range (-1/2, 1/2]
    return bad;
}

TL_TEST(det_atan2_symmetries_seeded, "foundation,fx,det,fast") {
    FxRng rng = { 0x6174616e32ull };
    u32 bad = 0;
    for (u32 i = 0; i < (1u << 20); ++i) bad += check_symmetries(fx_rng_i32(&rng), fx_rng_i32(&rng));
    // octant boundaries and their neighbourhoods at several magnitudes
    static const i32 mags[] = { 1, 5, 1 << 10, 1 << 20, 1 << 29, (1 << 30) - 1, 1 << 30, INT32_MAX - 1 };
    for (i32 m : mags) {
        for (i32 d = -3; d <= 3; ++d) {
            if (m + d <= 0) continue;
            bad += check_symmetries(m + d, m);
            bad += check_symmetries(m, m + d);
            bad += check_symmetries(d, m);
            bad += check_symmetries(m, d);
        }
    }
    TL_EXPECT_EQ(bad, 0u);
}

TL_TEST(det_atan2_roundtrip_sincos, "foundation,fx,det,fast") {
    // atan2(sin a, cos a) == a within the measured bound, for all 2^16 angles on the 2^-16 grid
    // plus every quadrant boundary neighbourhood; the comparison is mod one turn in (-1/2, 1/2].
    i64 worst = 0;
    u32 bad = 0;
    for (i64 k = 0; k < (1 << 30); k += 1 << 14) {
        const angle_t a = fx_raw<angle_t>((i32)k);
        q_t s, c;
        sincos(a, &s, &c);
        const i32 r = atan2q(s, c).v;
        i64 d = (((i64)r - k) % (1 << 30) + (1 << 30)) % (1 << 30);
        if (d > (1 << 29)) d -= (1 << 30);
        const i64 ad = d < 0 ? -d : d;
        if (ad > worst) worst = ad;
        if (ad > FX_ATAN2_ROUNDTRIP_MAX_ERR_ULP) ++bad;
    }
    for (i32 q = 0; q < 8; ++q) {
        for (i32 d = -16; d <= 16; ++d) {
            const i32 k = (q << 27) + d;
            q_t s, c;
            sincos(fx_raw<angle_t>(k), &s, &c);
            const i32 r = atan2q(s, c).v;
            i64 dd = (((i64)r - k) % (1 << 30) + (1 << 30)) % (1 << 30);
            if (dd > (1 << 29)) dd -= (1 << 30);
            const i64 ad = dd < 0 ? -dd : dd;
            if (ad > worst) worst = ad;
            if (ad > FX_ATAN2_ROUNDTRIP_MAX_ERR_ULP) ++bad;
        }
    }
    TL_EXPECT_EQ(bad, 0u);
    TL_EXPECT_LE(worst, (i64)FX_ATAN2_ROUNDTRIP_MAX_ERR_ULP);
}

TL_TEST(det_atan2_vs_mpmath_table, "foundation,fx,det,fast") {
    // REF_ATAN[k] = RNE(atan(k/4096) / 2pi * 2^30), k = 0..4096: atan2q(k, 4096) exercises the
    // polynomial on an exact ratio, and atan2q(k << 18, 1 << 30) the same ratio at full scale.
    u32 bad = 0;
    i64 worst = 0;
    for (i32 k = 0; k <= 4096; ++k) {
        const i32 r1 = atan2q(fx_raw<q_t>(k), fx_raw<q_t>(4096)).v;
        const i32 r2 = atan2q(fx_raw<q_t>(k << 18), fx_raw<q_t>(1 << 30)).v;
        const i64 e1 = (i64)r1 - REF_ATAN[k], e2 = (i64)r2 - REF_ATAN[k];
        const i64 a1 = e1 < 0 ? -e1 : e1, a2 = e2 < 0 ? -e2 : e2;
        if (r1 != r2) ++bad;                                   // the ratio is exact: same z, same result
        if (a1 > FX_ATAN2_MAX_ERR_ULP) ++bad;
        if (a1 > worst) worst = a1;
        if (a2 > worst) worst = a2;
        // the complementary angle through the swap path
        if (k > 0) {
            const i32 rc = atan2q(fx_raw<q_t>(4096), fx_raw<q_t>(k)).v;
            const i64 ec = (i64)rc - (QT - REF_ATAN[k]);
            const i64 ac = ec < 0 ? -ec : ec;
            if (ac > FX_ATAN2_MAX_ERR_ULP) ++bad;
            if (ac > worst) worst = ac;
        }
    }
    TL_EXPECT_EQ(bad, 0u);
    TL_EXPECT_LE(worst, (i64)FX_ATAN2_MAX_ERR_ULP);
}
