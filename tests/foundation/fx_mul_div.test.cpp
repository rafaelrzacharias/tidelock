// fx_mul_div.test.cpp - mul<R>/div<R>/mul_int/mul_wide/to<R>, the constructors, the
// operators, and the quanta helpers (docs/FX-PALETTE.md §10.1, §10.5 "fx_mul_div").
// Property tests: 1M seeded pairs per op-table entry against the exact rational reference
// (ref_rne_div on the i64 product - no double anywhere: a double loses bits above 2^53, the
// exact integer reference does not). Exhaustive tests: every pair of 8-bit raw operands for
// every distinct shift the table uses, against the same reference.
// Spec: docs/FX-PALETTE.md §10.1, §3.1. Rubric: docs/TESTING.md §7.
#include "fx_test_util.h"

using namespace fx;

// --- exhaustive small-operand products through the real helper ------------------------------
// a in [-128, 127] x b in [-32768, 32767]: 16M pairs per entry; products reach 2^23, so every
// remainder pattern below the narrowing point is exercised for shifts <= 16 (the 16-bit rows:
// scalar x scalar, invmass x lambda). Larger shifts are covered by the property tests with
// operands scaled to the shift, and by the `to<R>` 14-bit narrowing sweep.
template <typename R, typename A, typename B>
static u32 exhaustive_mul(void) {
    constexpr int S = A::FRAC_BITS + B::FRAC_BITS - R::FRAC_BITS;
    u32 bad = 0;
    for (i32 a = -128; a <= 127; ++a) {
        for (i32 b = -32768; b <= 32767; ++b) {
            const i64 want = ref_rne_shr((i64)a * (i64)b, S);
            const R got = mul<R>(fx_raw<A>(a), fx_raw<B>(b));
            if ((i64)got.v != want) ++bad;
        }
    }
    return bad;
}

// a, b in [-256, 255], b != 0; pairs whose exact quotient does not fit R are skipped (they
// assert in dev) and counted so the test can prove it was not vacuous.
template <typename R, typename A, typename B>
static u32 exhaustive_div(u32* tested) {
    constexpr int S = R::FRAC_BITS + B::FRAC_BITS - A::FRAC_BITS;
    u32 bad = 0;
    for (i32 a = -256; a <= 255; ++a) {
        for (i32 b = -256; b <= 255; ++b) {
            if (b == 0) continue;
            const i64 want = ref_rne_div((i64)a * ((i64)1 << S), (i64)b);
            if (want < INT32_MIN || want > INT32_MAX) continue;
            ++*tested;
            const R got = div<R>(fx_raw<A>(a), fx_raw<B>(b));
            if ((i64)got.v != want) ++bad;
        }
    }
    return bad;
}

// --- property: seeded pairs; b is scaled to the shift so most products fit R, and pairs whose
// exact result does not fit are skipped (they assert in dev) and counted. ------------------
template <typename R, typename A, typename B>
static u32 property_mul(u64 seed, u32 n, u32* tested) {
    constexpr int S = A::FRAC_BITS + B::FRAC_BITS - R::FRAC_BITS;
    constexpr int SCALE = S >= 31 ? 0 : 31 - S;
    FxRng rng = { seed };
    u32 bad = 0;
    for (u32 i = 0; i < n; ++i) {
        const i32 a = fx_rng_i32(&rng);
        const i32 b = (i & 7u) == 0 ? fx_rng_i32(&rng) : (fx_rng_i32(&rng) >> SCALE);
        const i64 want = ref_rne_shr((i64)a * (i64)b, S);
        if (want < INT32_MIN || want > INT32_MAX) continue;
        ++*tested;
        const R got = mul<R>(fx_raw<A>(a), fx_raw<B>(b));
        if ((i64)got.v != want) ++bad;
        // sat_mul agrees wherever mul is in range
        const R sat = sat_mul<R>(fx_raw<A>(a), fx_raw<B>(b));
        if (sat.v != got.v) ++bad;
    }
    return bad;
}

template <typename R, typename A, typename B>
static u32 property_div(u64 seed, u32 n, u32* tested) {
    constexpr int S = R::FRAC_BITS + B::FRAC_BITS - A::FRAC_BITS;
    FxRng rng = { seed };
    u32 bad = 0;
    for (u32 i = 0; i < n; ++i) {
        // a scaled below b most of the time so the quotient fits a unit-range result
        const i32 b = fx_rng_i32(&rng);
        const i32 a = (i & 7u) == 0 ? fx_rng_i32(&rng) : (i32)(((i64)b * (i64)(fx_rng_i32(&rng) >> 1)) >> 31);
        if (b == 0) continue;
        const i64 want = ref_rne_div((i64)a * ((i64)1 << S), (i64)b);
        if (want < INT32_MIN || want > INT32_MAX) continue;
        ++*tested;
        const R got = div<R>(fx_raw<A>(a), fx_raw<B>(b));
        if ((i64)got.v != want) ++bad;
    }
    return bad;
}

TL_TEST(fx_mul_exhaustive_small_operands, "foundation,fx,fast") {
    TL_EXPECT_EQ((exhaustive_mul<scalar_t, scalar_t, scalar_t>()), 0u);     // shift 16
    TL_EXPECT_EQ((exhaustive_mul<pos_t, invmass_t, lambda_t>()), 0u);       // shift 16
    TL_EXPECT_EQ((exhaustive_mul<vel_t, scalar_t, vel_t>()), 0u);           // shift 16
    TL_EXPECT_EQ((exhaustive_mul<angle_t, omega_t, dt_t>()), 0u);           // shift 22 (rev 2)
    // to<R> narrowing q_t (30) -> scalar_t (16): a 14-bit RNE shift over every 16-bit input
    u32 bad = 0;
    for (i32 a = -32768; a <= 32767; ++a) {
        const i64 want = ref_rne_shr(a, 14);
        if (to<scalar_t>(fx_raw<q_t>(a)).v != (i32)want) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
}

TL_TEST(fx_div_exhaustive_small_operands, "foundation,fx,fast") {
    u32 tested = 0;
    TL_EXPECT_EQ((exhaustive_div<pos_t, pos_t, pos_t>(&tested)), 0u);            // shift 18: always fits
    TL_EXPECT_EQ(tested, 512u * 511u);
    tested = 0;
    TL_EXPECT_EQ((exhaustive_div<q_t, pos_t, pos_t>(&tested)), 0u);              // shift 30: |a| <= 2|b| fits
    TL_EXPECT_GT(tested, 100000u);
    tested = 0;
    TL_EXPECT_EQ((exhaustive_div<scalar_t, scalar_t, scalar_t>(&tested)), 0u);   // shift 16
    TL_EXPECT_EQ(tested, 512u * 511u);
}

// The skip rate is asserted PER ROW (an aggregate over 16 rows let one vacuous row hide behind
// fifteen full ones - W1 fx review 3). b is scaled to the shift, so 7 of every 8 pairs fit by
// construction; the floor is 3/4.
#define FX_PROPERTY_ROW(FN, R, A, B, SEED, N)                                      \
    do {                                                                            \
        u32 tested = 0;                                                             \
        TL_EXPECT_EQ((FN<R, A, B>(SEED, N, &tested)), 0u);                          \
        TL_EXPECT_GE(tested, (N) / 4u * 3u);                                        \
    } while (0)

TL_TEST(fx_mul_property_every_table_row, "foundation,fx,fast") {
    // One million seeded pairs per distinct (format) product of docs/FX-PALETTE.md §3.1.
    const u32 N = 1000000u;
    FX_PROPERTY_ROW(property_mul, pos_t, vel_t, dt_t, 1, N);
    FX_PROPERTY_ROW(property_mul, angle_t, omega_t, dt_t, 2, N);
    FX_PROPERTY_ROW(property_mul, pos_t, invmass_t, lambda_t, 3, N);
    FX_PROPERTY_ROW(property_mul, pos_t, lambda_t, invmass_t, 4, N);
    FX_PROPERTY_ROW(property_mul, pos_t, q_t, pos_t, 5, N);
    FX_PROPERTY_ROW(property_mul, pos_t, pos_t, q_t, 6, N);
    FX_PROPERTY_ROW(property_mul, vel_t, q_t, vel_t, 7, N);
    FX_PROPERTY_ROW(property_mul, vel_t, vel_t, q_t, 8, N);
    FX_PROPERTY_ROW(property_mul, q_t, q_t, q_t, 9, N);
    FX_PROPERTY_ROW(property_mul, scalar_t, q_t, scalar_t, 10, N);
    FX_PROPERTY_ROW(property_mul, scalar_t, scalar_t, q_t, 11, N);
    FX_PROPERTY_ROW(property_mul, vel_t, scalar_t, vel_t, 12, N);
    FX_PROPERTY_ROW(property_mul, vel_t, vel_t, scalar_t, 13, N);
    FX_PROPERTY_ROW(property_mul, q_t, scalar_t, q_t, 14, N);
    FX_PROPERTY_ROW(property_mul, q_t, q_t, scalar_t, 15, N);
    FX_PROPERTY_ROW(property_mul, scalar_t, scalar_t, scalar_t, 16, N);
    // rev 2: omega_t is its own format (docs/FX-PALETTE.md §9 R-8) - its triples are distinct rows
    FX_PROPERTY_ROW(property_mul, omega_t, q_t, omega_t, 17, N);
    FX_PROPERTY_ROW(property_mul, omega_t, omega_t, q_t, 18, N);
    FX_PROPERTY_ROW(property_mul, omega_t, scalar_t, omega_t, 19, N);
    FX_PROPERTY_ROW(property_mul, omega_t, omega_t, scalar_t, 20, N);
}

TL_TEST(fx_div_property_every_table_row, "foundation,fx,fast") {
    const u32 N = 1000000u;
    FX_PROPERTY_ROW(property_div, q_t, pos_t, pos_t, 21, N);
    FX_PROPERTY_ROW(property_div, q_t, vel_t, vel_t, 22, N);
    FX_PROPERTY_ROW(property_div, q_t, q_t, q_t, 23, N);
    FX_PROPERTY_ROW(property_div, q_t, scalar_t, scalar_t, 24, N);
    FX_PROPERTY_ROW(property_div, pos_t, pos_t, pos_t, 25, N);
    FX_PROPERTY_ROW(property_div, vel_t, vel_t, vel_t, 26, N);
    FX_PROPERTY_ROW(property_div, scalar_t, scalar_t, scalar_t, 27, N);
    FX_PROPERTY_ROW(property_div, q_t, omega_t, omega_t, 28, N);       // rev 2 omega quotient rows
    FX_PROPERTY_ROW(property_div, omega_t, omega_t, omega_t, 29, N);
    // div ties need a 2^31 divisor, i.e. INT32_MIN: 3*2^30/-2^31 = -1.5 -> -2 (even);
    // 5 -> -2.5 -> -2; -3 -> 1.5 -> 2; -1 -> 0.5 -> 0. Unit quotients are exact.
    TL_EXPECT_EQ(div<q_t>(fx_raw<pos_t>(3), fx_raw<pos_t>(INT32_MIN)).v, -2);
    TL_EXPECT_EQ(div<q_t>(fx_raw<pos_t>(5), fx_raw<pos_t>(INT32_MIN)).v, -2);
    TL_EXPECT_EQ(div<q_t>(fx_raw<pos_t>(-3), fx_raw<pos_t>(INT32_MIN)).v, 2);
    TL_EXPECT_EQ(div<q_t>(fx_raw<pos_t>(-1), fx_raw<pos_t>(INT32_MIN)).v, 0);
    TL_EXPECT_EQ(div<q_t>(fx_raw<pos_t>(INT32_MIN), fx_raw<pos_t>(INT32_MIN)).v, q_t::ONE);
    TL_EXPECT_EQ(div<q_t>(fx_raw<pos_t>(7), fx_raw<pos_t>(7)).v, q_t::ONE);
    TL_EXPECT_EQ(div<q_t>(fx_raw<pos_t>(-7), fx_raw<pos_t>(7)).v, -q_t::ONE);
    TL_EXPECT_EQ(div<q_t>(fx_raw<pos_t>(0), fx_raw<pos_t>(-7)).v, 0);
    TL_EXPECT_EQ(div<q_t>(fx_int<pos_t>(1), fx_int<pos_t>(3)).v, 357913941);   // RNE(2^30/3)
    TL_EXPECT_EQ(div<q_t>(fx_int<pos_t>(2), fx_int<pos_t>(3)).v, 715827883);   // RNE(2^31/3)
    TL_EXPECT_EQ(div<pos_t>(fx_int<pos_t>(1), fx_int<pos_t>(4)).v, 1 << 16);   // 0.25 m
}

TL_TEST(fx_mul_int_and_mul_wide, "foundation,fx,fast") {
    // v = (x - x_prev) * INV_H: pos (18) -> vel (20) is an exact 2-bit widening
    const pos_t dx = fx_raw<pos_t>(12345);
    TL_EXPECT_EQ(mul_int<vel_t>(dx, INV_H).v, 12345 * 480 * 4);
    TL_EXPECT_EQ(mul_int<vel_t>(-dx, INV_H).v, -12345 * 480 * 4);
    TL_EXPECT_EQ(mul_int<vel_t>(fx_raw<pos_t>(0), INV_H).v, 0);
    // one texel per substep is 30 m/s: (1/16 m) * 480 = 30 m/s exactly
    TL_EXPECT_EQ(mul_int<vel_t>(TEXEL, INV_H).v, fx_int<vel_t>(30).v);
    // omega = dtheta * INV_H: angle (30) -> omega (22) narrows by 8 with RNE (rev 2; was 10)
    const angle_t dth = fx_raw<angle_t>(1023);                 // 1023 * 480 = 491040; /256 = 1918.125 -> 1918
    TL_EXPECT_EQ(mul_int<omega_t>(dth, INV_H).v, 1918);
    TL_EXPECT_EQ(mul_int<omega_t>(fx_raw<angle_t>(1), INV_H).v, 2);          // 480/256 = 1.875 -> 2
    TL_EXPECT_EQ(mul_int<omega_t>(fx_raw<angle_t>(-1), INV_H).v, -2);
    TL_EXPECT_EQ(mul_int<omega_t>(fx_raw<angle_t>(2), INV_H).v, 4);          // 960/256 = 3.75 -> 4
    TL_EXPECT_EQ(mul_int<omega_t>(fx_raw<angle_t>(4), 32).v, 0);             // 128/256 = 0.5 -> 0 (even)
    TL_EXPECT_EQ(mul_int<omega_t>(fx_raw<angle_t>(12), 32).v, 2);            // 384/256 = 1.5 -> 2 (even)
    // a full turn per substep is 480 turn/s
    TL_EXPECT_EQ(mul_int<omega_t>(TURN, INV_H).v, fx_int<omega_t>(480).v);
    // mul_wide: exact product, no rounding, sign preserved
    TL_EXPECT_EQ(mul_wide(fx_raw<pos_t>(INT32_MAX), fx_raw<pos_t>(INT32_MAX)), (i64)INT32_MAX * INT32_MAX);
    TL_EXPECT_EQ(mul_wide(fx_raw<pos_t>(INT32_MIN), fx_raw<pos_t>(INT32_MIN)), (i64)INT32_MIN * INT32_MIN);
    TL_EXPECT_EQ(mul_wide(fx_raw<pos_t>(INT32_MIN), fx_raw<pos_t>(INT32_MAX)), (i64)INT32_MIN * INT32_MAX);
    TL_EXPECT_EQ(mul_wide(WORLD_HALF, WORLD_HALF), (i64)4096 * 4096 << 36);
}

TL_TEST(fx_constructors_and_conversions, "foundation,fx,fast") {
    TL_EXPECT_EQ(fx_raw<pos_t>(-5).v, -5);
    TL_EXPECT_EQ(fx_int<pos_t>(0).v, 0);
    TL_EXPECT_EQ(fx_int<pos_t>(1).v, 1 << 18);
    TL_EXPECT_EQ(fx_int<pos_t>(-1).v, -(1 << 18));
    TL_EXPECT_EQ(fx_int<pos_t>(8191).v, 8191 << 18);                  // the row's max integer
    TL_EXPECT_EQ(fx_int<pos_t>(-8191).v, -(8191 << 18));
    TL_EXPECT_EQ(fx_int<q_t>(1).v, q_t::ONE);
    TL_EXPECT_EQ(fx_int<scalar_t>(32767).v, 32767 << 16);
    // fx_lit: the documented constants and a few rationals
    TL_EXPECT_EQ(fx_lit<dt_t>(1, 480).v, 2236962);
    TL_EXPECT_EQ(fx_lit<q_t>(1, 2).v, 1 << 29);
    TL_EXPECT_EQ(fx_lit<q_t>(-1, 2).v, -(1 << 29));
    TL_EXPECT_EQ(fx_lit<q_t>(1, 3).v, 357913941);
    TL_EXPECT_EQ(fx_lit<q_t>(2, -3).v, -715827883);
    TL_EXPECT_EQ(fx_lit<pos_t>(1, 16).v, TEXEL.v);
    TL_EXPECT_EQ(fx_lit<scalar_t>(1, 131072).v, 0);                   // 2^16/2^17 = 0.5 -> 0 (even)
    TL_EXPECT_EQ(fx_lit<scalar_t>(3, 131072).v, 2);                   // 1.5 -> 2 (even)
    // floor to int
    TL_EXPECT_EQ(fx_to_int_floor(fx_int<pos_t>(7)), 7);
    TL_EXPECT_EQ(fx_to_int_floor(fx_raw<pos_t>((7 << 18) + 1)), 7);
    TL_EXPECT_EQ(fx_to_int_floor(fx_raw<pos_t>(-1)), -1);              // -epsilon floors to -1
    TL_EXPECT_EQ(fx_to_int_floor(fx_raw<pos_t>(-(1 << 18))), -1);
    TL_EXPECT_EQ(fx_to_int_floor(fx_raw<pos_t>(-(1 << 18) - 1)), -2);
    TL_EXPECT_EQ(fx_to_int_floor(fx_raw<pos2_wide_t>(-(i64)1)), (i64)-1);
    // to<R>: widen exact, narrow RNE, identity
    TL_EXPECT_EQ(to<vel_t>(fx_raw<pos_t>(3)).v, 12);
    TL_EXPECT_EQ(to<vel_t>(fx_raw<pos_t>(-3)).v, -12);
    TL_EXPECT_EQ(to<pos_t>(fx_raw<vel_t>(6)).v, 2);                   // 1.5 -> 2
    TL_EXPECT_EQ(to<pos_t>(fx_raw<vel_t>(2)).v, 0);                   // 0.5 -> 0
    TL_EXPECT_EQ(to<pos_t>(fx_raw<vel_t>(-6)).v, -2);
    TL_EXPECT_EQ(to<pos_t>(fx_raw<vel_t>(-2)).v, 0);
    TL_EXPECT_EQ(to<pos_t>(fx_raw<pos_t>(77)).v, 77);
    TL_EXPECT_EQ(to<scalar_t>(fx_int<invmass_t>(4096)).v, 4096 << 16);
    TL_EXPECT_EQ(to<q_t>(fx_int<scalar_t>(1)).v, q_t::ONE);
    TL_EXPECT_EQ(to<pos_t>(fx_raw<pos2_wide_t>(((i64)5 << 18) + ((i64)1 << 17))).v, 6);             // 5.5 -> 6 (even)
    TL_EXPECT_EQ(to<pos_t>(fx_raw<pos2_wide_t>(((i64)4 << 18) + ((i64)1 << 17))).v, 4);             // 4.5 -> 4
    TL_EXPECT_EQ(to<pos2_wide_t>(fx_raw<pos_t>(-9)).v, -(i64)9 << 18);
}

TL_TEST(fx_operators_and_per_format_helpers, "foundation,fx,fast") {
    const pos_t a = fx_int<pos_t>(3), b = fx_int<pos_t>(-5);
    TL_EXPECT_EQ((a + b).v, -(2 << 18));
    TL_EXPECT_EQ((a - b).v, 8 << 18);
    TL_EXPECT_EQ((-b).v, 5 << 18);
    TL_EXPECT_TRUE(b < a && a > b && b <= a && a >= b && a != b && a == a);
    pos_t c = a; c += b; TL_EXPECT_EQ(c.v, (a + b).v); c -= b; TL_EXPECT_EQ(c.v, a.v);
    // wrap policy on the plain operators (explicit, docs/FX-PALETTE.md §1)
    TL_EXPECT_EQ((fx_raw<pos_t>(INT32_MAX) + fx_raw<pos_t>(1)).v, INT32_MIN);
    TL_EXPECT_EQ((fx_raw<pos_t>(INT32_MIN) - fx_raw<pos_t>(1)).v, INT32_MAX);
    TL_EXPECT_EQ((-fx_raw<pos_t>(INT32_MIN)).v, INT32_MIN);
    // angle_t wraps at +-2 turns and the turn mask makes every value equivalent mod 1 turn
    TL_EXPECT_EQ(((TURN + TURN) + fx_raw<angle_t>(1)).v, INT32_MIN + 1);
    TL_EXPECT_EQ((u32)(TURN + TURN + TURN).v & (u32)(TURN.v - 1), 0u);
    // sat tier
    TL_EXPECT_EQ(sat_add(fx_raw<pos_t>(INT32_MAX), fx_raw<pos_t>(1)).v, INT32_MAX);
    TL_EXPECT_EQ(sat_sub(fx_raw<pos_t>(INT32_MIN), fx_raw<pos_t>(1)).v, INT32_MIN);
    TL_EXPECT_EQ(sat_neg(fx_raw<pos_t>(INT32_MIN)).v, INT32_MAX);
    TL_EXPECT_EQ(sat_add(a, b).v, (a + b).v);
    // abs/min/max/clamp/sign/is_zero
    TL_EXPECT_EQ(abs(b).v, 5 << 18);
    TL_EXPECT_EQ(abs(a).v, 3 << 18);
    TL_EXPECT_EQ(abs(fx_raw<pos_t>(INT32_MIN)).v, INT32_MAX);
    TL_EXPECT_EQ(min(a, b).v, b.v);
    TL_EXPECT_EQ(max(a, b).v, a.v);
    TL_EXPECT_EQ(clamp(fx_int<pos_t>(9), b, a).v, a.v);
    TL_EXPECT_EQ(clamp(fx_int<pos_t>(-9), b, a).v, b.v);
    TL_EXPECT_EQ(clamp(fx_int<pos_t>(0), b, a).v, 0);
    TL_EXPECT_EQ(clamp(a, a, a).v, a.v);
    TL_EXPECT_EQ(sign(a), 1);
    TL_EXPECT_EQ(sign(b), -1);
    TL_EXPECT_EQ(sign(fx_raw<pos_t>(0)), 0);
    TL_EXPECT_TRUE(is_zero(fx_raw<pos_t>(0)) && !is_zero(a));
    // i64 locals get the same operators
    const pos2_wide_t w = fx_raw<pos2_wide_t>((i64)1 << 40), z = fx_raw<pos2_wide_t>(-((i64)1 << 40));
    TL_EXPECT_EQ((w + z).v, (i64)0);
    TL_EXPECT_EQ((w - z).v, (i64)1 << 41);
    TL_EXPECT_TRUE(z < w);
    TL_EXPECT_EQ(abs(z).v, w.v);
    TL_EXPECT_EQ(sat_add(fx_raw<pos2_wide_t>(INT64_MAX), w).v, INT64_MAX);
    // zero-initialisation and trivial copy
    pos_t zero{};
    TL_EXPECT_EQ(zero.v, 0);
    TL_EXPECT_TRUE(__is_trivially_copyable(pos_t));
}

TL_TEST(fx_quanta_helpers_edges, "foundation,fx,fast") {
    // wrap_*
    TL_EXPECT_EQ(wrap_add(INT32_MAX, 1), INT32_MIN);
    TL_EXPECT_EQ(wrap_add(INT32_MIN, -1), INT32_MAX);
    TL_EXPECT_EQ(wrap_sub(INT32_MIN, 1), INT32_MAX);
    TL_EXPECT_EQ(wrap_mul(INT32_MAX, 2), -2);
    TL_EXPECT_EQ(wrap_mul(1 << 30, 2), INT32_MIN);          // the sanctioned left shift of a signed value
    TL_EXPECT_EQ(wrap_mul(-(1 << 30), 2), INT32_MIN);
    TL_EXPECT_EQ(wrap_neg(INT32_MIN), INT32_MIN);
    TL_EXPECT_EQ(wrap_add(INT64_MAX, (i64)1), INT64_MIN);
    TL_EXPECT_EQ(wrap_sub(INT64_MIN, (i64)1), INT64_MAX);
    TL_EXPECT_EQ(wrap_mul(INT64_MAX, (i64)2), (i64)-2);
    TL_EXPECT_EQ(wrap_neg(INT64_MIN), INT64_MIN);
    // sat_* i32
    TL_EXPECT_EQ(sat_add(INT32_MAX, 1), INT32_MAX);
    TL_EXPECT_EQ(sat_add(INT32_MIN, -1), INT32_MIN);
    TL_EXPECT_EQ(sat_add(INT32_MAX, INT32_MIN), -1);
    TL_EXPECT_EQ(sat_sub(INT32_MIN, 1), INT32_MIN);
    TL_EXPECT_EQ(sat_sub(INT32_MAX, -1), INT32_MAX);
    TL_EXPECT_EQ(sat_sub(0, INT32_MIN), INT32_MAX);
    TL_EXPECT_EQ(sat_mul(INT32_MAX, 2), INT32_MAX);
    TL_EXPECT_EQ(sat_mul(INT32_MIN, 2), INT32_MIN);
    TL_EXPECT_EQ(sat_mul(INT32_MIN, -1), INT32_MAX);
    TL_EXPECT_EQ(sat_mul(-46341, 46341), INT32_MIN);                        // -2147488281: overflow by one
    TL_EXPECT_EQ(sat_mul(46340, 46340), 2147395600);
    TL_EXPECT_EQ(sat_neg(INT32_MIN), INT32_MAX);
    TL_EXPECT_EQ(sat_neg(7), -7);
    // sat_* i64, including the exact boundaries
    TL_EXPECT_EQ(sat_add(INT64_MAX, (i64)1), INT64_MAX);
    TL_EXPECT_EQ(sat_add(INT64_MIN, (i64)-1), INT64_MIN);
    TL_EXPECT_EQ(sat_add(INT64_MAX, INT64_MIN), (i64)-1);
    TL_EXPECT_EQ(sat_add(INT64_MAX - 1, (i64)1), INT64_MAX);
    TL_EXPECT_EQ(sat_sub(INT64_MIN, (i64)1), INT64_MIN);
    TL_EXPECT_EQ(sat_sub(INT64_MAX, (i64)-1), INT64_MAX);
    TL_EXPECT_EQ(sat_sub((i64)0, INT64_MIN), INT64_MAX);
    TL_EXPECT_EQ(sat_sub((i64)-1, INT64_MAX), INT64_MIN);
    TL_EXPECT_EQ(sat_mul((i64)1 << 32, (i64)1 << 31), INT64_MAX);
    TL_EXPECT_EQ(sat_mul((i64)1 << 32, -((i64)1 << 31)), INT64_MIN);
    TL_EXPECT_EQ(sat_mul((i64)1 << 31, (i64)1 << 31), (i64)1 << 62);
    TL_EXPECT_EQ(sat_mul(-((i64)1 << 31), (i64)1 << 32), INT64_MIN);      // exactly -2^63: representable
    TL_EXPECT_EQ(sat_mul(INT64_MIN, (i64)-1), INT64_MAX);
    TL_EXPECT_EQ(sat_mul(INT64_MIN, (i64)1), INT64_MIN);
    TL_EXPECT_EQ(sat_mul((i64)-3, (i64)5), (i64)-15);
    TL_EXPECT_EQ(sat_neg(INT64_MIN), INT64_MAX);
    // mul_widen
    TL_EXPECT_EQ(mul_widen(INT32_MIN, INT32_MIN), (i64)1 << 62);
    TL_EXPECT_EQ(mul_widen(INT32_MIN, INT32_MAX), -((i64)1 << 62) + ((i64)1 << 31));
}

TL_TEST(fx_mulhi64_property_vs_128bit, "foundation,fx,fast") {
    // The 128-bit oracle is the compiler's; the helper must never need it on a sim path.
    FxRng rng = { 0x6d75'6c68'6936'3400ull };
    u32 bad = 0;
    for (u32 i = 0; i < 1000000u; ++i) {
        const i64 a = fx_rng_i64(&rng), b = fx_rng_i64(&rng);
        const __int128 p = (__int128)a * (__int128)b;
        const i64 hi = (i64)(p >> 64);
        const u64 uhi = (u64)(((unsigned __int128)(u64)a * (unsigned __int128)(u64)b) >> 64);
        if (mulhi64(a, b) != hi) ++bad;
        if (mulhi64u((u64)a, (u64)b) != uhi) ++bad;
        // sat_mul agrees with the 128-bit product clamped
        const i64 want = p > (__int128)INT64_MAX ? INT64_MAX : p < (__int128)INT64_MIN ? INT64_MIN : (i64)p;
        if (sat_mul(a, b) != want) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
    TL_EXPECT_EQ(mulhi64(INT64_MIN, INT64_MIN), (i64)1 << 62);
    TL_EXPECT_EQ(mulhi64(INT64_MIN, INT64_MAX), -((i64)1 << 62));
    TL_EXPECT_EQ(mulhi64u(UINT64_MAX, UINT64_MAX), UINT64_MAX - 1u);
}
