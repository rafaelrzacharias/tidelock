// det_vec.test.cpp - vec2, dot/cross, len2_wide/len, normalize, rotate, lerp (docs/FX-PALETTE.md
// §4.3, §10.3, §10.5 "det_vec").
// Spec: docs/FX-PALETTE.md §10.3. Rubric: docs/TESTING.md §7.
#include "fx_test_util.h"
#include "foundation/det_math.h"

using namespace fx;

static vec2<pos_t> vp(i32 x, i32 y) { return { fx_raw<pos_t>(x), fx_raw<pos_t>(y) }; }
static vec2<q_t> vq(i32 x, i32 y) { return { fx_raw<q_t>(x), fx_raw<q_t>(y) }; }

TL_TEST(det_vec_operators, "foundation,fx,det,fast") {
    const vec2<pos_t> a = vp(3, -4), b = vp(10, 20);
    TL_EXPECT_TRUE((a + b) == vp(13, 16));
    TL_EXPECT_TRUE((a - b) == vp(-7, -24));
    TL_EXPECT_TRUE((-a) == vp(-3, 4));
    TL_EXPECT_TRUE(a != b);
    TL_EXPECT_TRUE((vp(INT32_MAX, 0) + vp(1, 0)) == vp(INT32_MIN, 0));     // wraps like the scalar
}

TL_TEST(det_vec_dot_cross, "foundation,fx,det,fast") {
    // dot<pos2_wide_t>(pos, pos): shift 0, exact
    const vec2<pos_t> a = vp(3, -4), b = vp(10, 20);
    TL_EXPECT_EQ(dot<pos2_wide_t>(a, b).v, (i64)30 - 80);
    TL_EXPECT_EQ(dot<pos2_wide_t>(a, a).v, (i64)25);
    TL_EXPECT_EQ(len2_wide(a), (i64)25);
    TL_EXPECT_EQ(len2_wide(vp(INT32_MAX, 0)), (i64)INT32_MAX * INT32_MAX);
    TL_EXPECT_EQ(len2_wide(vp(INT32_MIN, 0)), (i64)1 << 62);
    // dot<q_t>(q, q): shift 30 with RNE - unit vectors
    TL_EXPECT_EQ(dot<q_t>(vq(q_t::ONE, 0), vq(q_t::ONE, 0)).v, q_t::ONE);
    TL_EXPECT_EQ(dot<q_t>(vq(q_t::ONE, 0), vq(0, q_t::ONE)).v, 0);
    TL_EXPECT_EQ(dot<q_t>(vq(1 << 29, 1 << 29), vq(1 << 29, 1 << 29)).v, 1 << 29);     // 1/4 + 1/4
    TL_EXPECT_EQ(dot<q_t>(vq(3, 0), vq(1 << 29, 0)).v, 2);                            // 1.5 -> 2 (even)
    TL_EXPECT_EQ(dot<q_t>(vq(1, 0), vq(1 << 29, 0)).v, 0);                            // 0.5 -> 0
    // cross<pos_t>(pos, q): the body angular term r x n, shift 30
    TL_EXPECT_EQ(cross<pos_t>(vp(1 << 18, 0), vq(0, q_t::ONE)).v, 1 << 18);           // x * 1 - 0
    TL_EXPECT_EQ(cross<pos_t>(vp(0, 1 << 18), vq(q_t::ONE, 0)).v, -(1 << 18));
    TL_EXPECT_EQ(cross<pos_t>(vp(1 << 18, 1 << 18), vq(1 << 29, 1 << 29)).v, 0);     // parallel
    // dot<pos_t>(pos, q): projection onto a unit normal, shift 30
    TL_EXPECT_EQ(dot<pos_t>(vp(1 << 18, 7), vq(q_t::ONE, 0)).v, 1 << 18);
    TL_EXPECT_EQ(dot<pos_t>(vp(1 << 18, 1 << 18), vq(1 << 29, 1 << 29)).v, 1 << 18);
    // seeded: dot == sum of two muls within 1 ulp (dot rounds once, two muls round twice)
    FxRng rng = { 0x646f74ull };
    u32 bad = 0;
    for (u32 i = 0; i < (1u << 18); ++i) {
        const vec2<pos_t> p = vp(fx_rng_i32(&rng) >> 8, fx_rng_i32(&rng) >> 8);
        const vec2<q_t> n = vq(fx_rng_i32(&rng) >> 1, fx_rng_i32(&rng) >> 1);
        const i64 exact = (i64)p.x.v * n.x.v + (i64)p.y.v * n.y.v;
        const i64 want = ref_rne_shr(exact, 30);
        if (dot<pos_t>(p, n).v != (i32)want) ++bad;
        const i64 cx = (i64)p.x.v * n.y.v - (i64)p.y.v * n.x.v;
        if (cross<pos_t>(p, n).v != (i32)ref_rne_shr(cx, 30)) ++bad;
        const i32 two = (mul<pos_t>(p.x, n.x) + mul<pos_t>(p.y, n.y)).v;
        if (!fx_near_raw(two, dot<pos_t>(p, n).v, 1)) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
}

TL_TEST(det_vec_len_and_normalize, "foundation,fx,det,fast") {
    // 3-4-5 in metres, exactly
    TL_EXPECT_EQ(len(vp(3 << 18, 4 << 18)).v, 5 << 18);
    TL_EXPECT_EQ(len(vp(-3 << 18, 4 << 18)).v, 5 << 18);
    TL_EXPECT_EQ(len(vp(0, 0)).v, 0);
    TL_EXPECT_EQ(len(vp(0, -7)).v, 7);
    TL_EXPECT_EQ(len(vp(1, 1)).v, 1);                                   // sqrt 2 quanta = 1.41 -> 1
    TL_EXPECT_EQ(len(vp(INT32_MAX, 0)).v, INT32_MAX);                    // the largest representable length
    // normalize: axis vectors exact, 3-4-5 to the nearest q_t
    TL_EXPECT_TRUE(normalize(vp(5 << 18, 0)) == vq(q_t::ONE, 0));
    TL_EXPECT_TRUE(normalize(vp(0, -5)) == vq(0, -q_t::ONE));
    const vec2<q_t> n = normalize(vp(3 << 18, 4 << 18));
    TL_EXPECT_EQ(n.x.v, fx_lit<q_t>(3, 5).v);
    TL_EXPECT_EQ(n.y.v, fx_lit<q_t>(4, 5).v);
    // the documented zero-vector value (the assert is a fatal-expected test, TODO.md)
#if !TL_DEV
    TL_EXPECT_TRUE(normalize(vp(0, 0)) == vq(0, 0));
#endif
    // seeded: the precision of normalize is bounded by the INPUT's quantisation - len is
    // correctly rounded (<= 1/2 quantum), so |u| is within 1/(2|d|) of 1 in relative terms,
    // i.e. |u|^2 - 1 is within 2^30/|d| + a few ulps of Q30; the direction error is the two
    // divisions' half-ulps only. Both bounds checked for magnitudes from 1 quantum to 8 km.
    FxRng rng = { 0x6e6f726dull };
    u32 bad = 0;
    i64 worst_long = 0;                                                // |d| >= 2^28 quanta (1 km)
    for (u32 i = 0; i < (1u << 18); ++i) {
        // components < 2^30 so |d|^2 < 2^61 and |d| < 2^31 (len's preconditions); 1 quantum..4 km
        const u32 sh = 1u + (u32)(fx_rng_next(&rng) % 30u);
        vec2<pos_t> d = vp(fx_rng_i32(&rng) >> sh, fx_rng_i32(&rng) >> sh);
        if (d.x.v == 0 && d.y.v == 0) d.x = fx_raw<pos_t>(1);
        const vec2<q_t> u = normalize(d);
        const i64 l = len(d).v;
        const i64 nrm = (i64)u.x.v * u.x.v + (i64)u.y.v * u.y.v;      // Q60
        const i64 dev = nrm - ((i64)1 << 60);
        const i64 adev = (dev < 0 ? -dev : dev) >> 30;                 // in Q30 ulps
        if (adev > ((i64)1 << 30) / l + 4) ++bad;
        if (l >= ((i64)1 << 28) && adev > worst_long) worst_long = adev;
        // direction: cross(d, u) = d x (u - d/l) <= |d| * (half an ulp per component)
        const i64 cr = (i64)d.x.v * u.y.v - (i64)d.y.v * u.x.v;       // Q48 raw
        const i64 acr = cr < 0 ? -cr : cr;
        if (acr > 2 * l + 2) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
    TL_EXPECT_LE(worst_long, (i64)8);                                  // at >= 1 km: 2^30/2^28 + 4
}

TL_TEST(det_vec_rotate, "foundation,fx,det,fast") {
    const vec2<pos_t> p = vp(3 << 18, 4 << 18);
    q_t s, c;
    sincos(fx_raw<angle_t>(0), &s, &c);
    TL_EXPECT_TRUE(rotate(p, s, c) == p);
    sincos(QUARTER_TURN, &s, &c);
    TL_EXPECT_TRUE(rotate(p, s, c) == vp(-4 << 18, 3 << 18));
    sincos(HALF_TURN, &s, &c);
    TL_EXPECT_TRUE(rotate(p, s, c) == vp(-3 << 18, -4 << 18));
    sincos(-QUARTER_TURN, &s, &c);
    TL_EXPECT_TRUE(rotate(p, s, c) == vp(4 << 18, -3 << 18));
    // rotating by a and then by -a returns within 1 quantum per component (two RNEs)
    FxRng rng = { 0x726f74ull };
    u32 bad = 0;
    for (u32 i = 0; i < (1u << 16); ++i) {
        const vec2<pos_t> q = vp(fx_rng_i32(&rng) >> 4, fx_rng_i32(&rng) >> 4);
        const angle_t a = fx_raw<angle_t>(fx_rng_i32(&rng));
        sincos(a, &s, &c);
        const vec2<pos_t> r = rotate(q, s, c);
        const vec2<pos_t> back = rotate(r, -s, c);
        // |r| == |q| within the kernel's error: 9 ulp of q_t over 2^27 quanta is ~1 quantum
        const i64 dl = (i64)len(r).v - (i64)len(q).v;
        if (dl > 3 || dl < -3) ++bad;
        if (!fx_near_raw(back.x.v, q.x.v, 3) || !fx_near_raw(back.y.v, q.y.v, 3)) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
}

TL_TEST(det_lerp, "foundation,fx,det,fast") {
    const pos_t a = fx_int<pos_t>(10), b = fx_int<pos_t>(20);
    TL_EXPECT_EQ(lerp<pos_t>(a, b, fx_raw<q_t>(0)).v, a.v);
    TL_EXPECT_EQ(lerp<pos_t>(a, b, fx_int<q_t>(1)).v, b.v);
    TL_EXPECT_EQ(lerp<pos_t>(a, b, fx_lit<q_t>(1, 2)).v, fx_int<pos_t>(15).v);
    TL_EXPECT_EQ(lerp<pos_t>(a, b, fx_lit<q_t>(1, 4)).v, fx_lit<pos_t>(25, 2).v);
    TL_EXPECT_EQ(lerp<pos_t>(b, a, fx_lit<q_t>(1, 4)).v, fx_lit<pos_t>(35, 2).v);
    TL_EXPECT_EQ(lerp<pos_t>(a, b, fx_int<q_t>(-1)).v, fx_int<pos_t>(0).v);          // extrapolation
    TL_EXPECT_EQ(lerp<q_t>(fx_raw<q_t>(0), fx_int<q_t>(1), fx_lit<q_t>(1, 3)).v, 357913941);
    TL_EXPECT_EQ(lerp<vel_t>(fx_int<vel_t>(-4), fx_int<vel_t>(4), fx_lit<q_t>(3, 4)).v, fx_int<vel_t>(2).v);
    TL_EXPECT_EQ(lerp<scalar_t>(fx_int<scalar_t>(1), fx_int<scalar_t>(1), fx_lit<q_t>(7, 9)).v, fx_int<scalar_t>(1).v);
    // one quantum apart: the DELTA rounds (0.5 quantum -> 0, even), so t == 1/2 stays at a;
    // t just above 1/2 moves
    TL_EXPECT_EQ(lerp<pos_t>(fx_raw<pos_t>(4), fx_raw<pos_t>(5), fx_lit<q_t>(1, 2)).v, 4);
    TL_EXPECT_EQ(lerp<pos_t>(fx_raw<pos_t>(5), fx_raw<pos_t>(6), fx_lit<q_t>(1, 2)).v, 5);
    TL_EXPECT_EQ(lerp<pos_t>(fx_raw<pos_t>(5), fx_raw<pos_t>(6), fx_raw<q_t>((1 << 29) + 1)).v, 6);
    TL_EXPECT_EQ(lerp<pos_t>(fx_raw<pos_t>(5), fx_raw<pos_t>(7), fx_lit<q_t>(1, 2)).v, 6);
}
