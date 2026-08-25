// fx_review.test.cpp - the W1 fx adversarial review's probes (TODO.md "W1 fx - the adversarial
// review"): every case below was written to BREAK a claim the lane made, and stays as the
// regression for the defects it found. Cases that pass document a claim that survived.
// Spec: docs/FX-PALETTE.md §10.1, §10.3, §10.4. Rubric: docs/TESTING.md §7 (edge matrix).
#include "fx_test_util.h"
#include "foundation/det_math.h"
#include "foundation/fx_float.h"

using namespace fx;

TL_TEST(fx_review_rint_f32_above_2p22, "foundation,fx,fast") {
    // Claim (fx_float.h): "RNE uses the 1.5 * 2^23 add-subtract identity, exact under the default
    // rounding mode". The identity holds only for |s| <= 2^22: for |s| in [2^22, 2^23) the
    // intermediate s + 1.5*2^23 is >= 2^24, where a float's spacing is 2, so odd integers are
    // rounded to even ones. 4194305 is integral and must come back unchanged.
    TL_EXPECT_TRUE(fx_rint_f32(4194305.0f) == 4194305.0f);
    TL_EXPECT_TRUE(fx_rint_f32(-4194305.0f) == -4194305.0f);
    TL_EXPECT_TRUE(fx_rint_f32(8388607.0f) == 8388607.0f);
    TL_EXPECT_TRUE(fx_rint_f32(-8388607.0f) == -8388607.0f);
    TL_EXPECT_TRUE(fx_rint_f32(4194304.5f) == 4194304.0f);          // tie -> even
    TL_EXPECT_TRUE(fx_rint_f32(4194305.5f) == 4194306.0f);          // tie -> even
    TL_EXPECT_TRUE(fx_rint_f32(-4194305.5f) == -4194306.0f);
    // the same through the public entry: 16 m + 1 quantum is a pos_t a level editor writes
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(4194305.0f / 262144.0f).v, 4194305);
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(-4194305.0f / 262144.0f).v, -4194305);
    // every odd integer in [2^22, 2^23) must survive (the f32 identity's whole broken band)
    u32 bad = 0;
    for (i32 r = 1 << 22; r < (1 << 23); r += 2) {
        if (fx_rint_f32((f32)r) != (f32)r) ++bad;
        if (from_f32_quantized<pos_t>((f32)r / 262144.0f).v != r) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
    // f64: the same band is [2^51, 2^52) - unreachable through a 32-bit row, but the helper
    // is public and must be right on its own domain
    TL_EXPECT_TRUE(fx_rint_f64(2251799813685249.0) == 2251799813685249.0);     // 2^51 + 1
    TL_EXPECT_TRUE(fx_rint_f64(-2251799813685249.0) == -2251799813685249.0);
    TL_EXPECT_TRUE(fx_rint_f64(2251799813685248.5) == 2251799813685248.0);     // tie -> even
}

TL_TEST(fx_review_atan2_range_is_closed, "foundation,fx,det,fast") {
    // Claim (det_math.h, FX-PALETTE.md §10.3): the result is in (-HALF_TURN, HALF_TURN]. A ratio
    // below 2^-31 rounds to z = 0, the polynomial gives 0, and y < 0 negates HALF_TURN: the range
    // is CLOSED on both ends, and -HALF_TURN is the same angle as +HALF_TURN mod one turn.
    TL_EXPECT_EQ(atan2q(fx_raw<q_t>(-1), fx_raw<q_t>(INT32_MIN)).v, -HALF_TURN.v);
    TL_EXPECT_EQ(atan2(fx_raw<pos_t>(-1), fx_raw<pos_t>(-(1 << 30))).v, -HALF_TURN.v);
    TL_EXPECT_EQ(atan2q(fx_raw<q_t>(1), fx_raw<q_t>(INT32_MIN)).v, HALF_TURN.v);
    TL_EXPECT_EQ(((u32)atan2q(fx_raw<q_t>(-1), fx_raw<q_t>(INT32_MIN)).v - (u32)HALF_TURN.v) & ((1u << 30) - 1u), 0u);
    // the smallest ratio that still rounds away from the axis: 1 / 2^30 is 0.16 ulp of a turn
    // (-> 0), 2 / 2^24 is 2.5 ulp (-> nonzero), so the only -HALF_TURN inputs are |y|/|x| < ~2^-30
    TL_EXPECT_LT(atan2q(fx_raw<q_t>(-64), fx_raw<q_t>(-(1 << 30))).v, 0);
    TL_EXPECT_GT(atan2q(fx_raw<q_t>(-64), fx_raw<q_t>(-(1 << 30))).v, -HALF_TURN.v);
}

TL_TEST(fx_review_sat_mul_fx_clamps, "foundation,fx,fast") {
    // Claim (fx.h): sat_mul<R> "clamps to R's range instead of asserting". The property test only
    // compares it to mul where mul is in range, so the clamp itself had no test.
    TL_EXPECT_EQ(sat_mul<q_t>(fx_raw<q_t>(INT32_MAX), fx_raw<q_t>(INT32_MAX)).v, INT32_MAX);        // 4 - eps
    TL_EXPECT_EQ(sat_mul<q_t>(fx_raw<q_t>(INT32_MIN), fx_raw<q_t>(INT32_MAX)).v, INT32_MIN);        // -4 + eps
    TL_EXPECT_EQ(sat_mul<q_t>(fx_raw<q_t>(INT32_MIN), fx_raw<q_t>(INT32_MIN)).v, INT32_MAX);        // +4
    TL_EXPECT_EQ(sat_mul<scalar_t>(fx_int<scalar_t>(256), fx_int<scalar_t>(128)).v, INT32_MAX);     // 32768 = 2^15: just out
    TL_EXPECT_EQ(sat_mul<scalar_t>(fx_int<scalar_t>(-256), fx_int<scalar_t>(128)).v, INT32_MIN);    // -32768: exactly representable
    TL_EXPECT_EQ(sat_mul<scalar_t>(fx_int<scalar_t>(256), fx_int<scalar_t>(127)).v, 32512 << 16);   // in range: equals mul
    TL_EXPECT_EQ(sat_mul<pos_t>(fx_raw<invmass_t>(INT32_MAX), fx_raw<lambda_t>(1 << 16)).v, INT32_MAX);
}

TL_TEST(fx_review_edge_matrix_in_contract, "foundation,fx,det,fast") {
    // The INT32_MIN / 2^62 / 2^63 edges that stay INSIDE each helper's contract (the ones that
    // leave it trap in dev and are the runner lane's fatal-expected tests, TODO.md).
    // rne_div: d = -1 for every n but INT64_MIN; |d| just under 2^62
    TL_EXPECT_EQ(rne_div(5, -1), (i64)-5);
    TL_EXPECT_EQ(rne_div(-5, -1), (i64)5);
    TL_EXPECT_EQ(rne_div(INT64_MAX, -1), -INT64_MAX);
    TL_EXPECT_EQ(rne_div(INT64_MIN + 1, -1), INT64_MAX);
    TL_EXPECT_EQ(rne_div(((i64)1 << 62) - 1, ((i64)1 << 62) - 1), (i64)1);
    TL_EXPECT_EQ(rne_div(((i64)1 << 61), ((i64)1 << 62) - 1), (i64)1);          // 0.5000000000000000002 -> 1
    TL_EXPECT_EQ(rne_div(-((i64)1 << 61), ((i64)1 << 62) - 1), (i64)-1);
    // rne_shr at s = 62 with every sign
    TL_EXPECT_EQ(rne_shr(((i64)1 << 61), 62), (i64)0);                            // exact half -> even (0)
    TL_EXPECT_EQ(rne_shr(((i64)1 << 61) + 1, 62), (i64)1);
    TL_EXPECT_EQ(rne_shr(-((i64)1 << 61), 62), (i64)0);
    TL_EXPECT_EQ(rne_shr(-((i64)1 << 61) - 1, 62), (i64)-1);
    TL_EXPECT_EQ(rne_shr(INT64_MIN + 1, 62), (i64)-2);
    // mul_int at |k| = INT32_MAX, a small enough that the result fits
    TL_EXPECT_EQ(mul_int<omega_t>(fx_raw<angle_t>(1), INT32_MAX).v, 8388608);    // (2^31 - 1) / 2^8 = 8388607.996 -> 8388608 (rev 2: narrow 8)
    TL_EXPECT_EQ(mul_int<omega_t>(fx_raw<angle_t>(-1), INT32_MAX).v, -8388608);
    TL_EXPECT_EQ(mul_int<omega_t>(fx_raw<angle_t>(1), INT32_MIN).v, -8388608);
    TL_EXPECT_EQ(mul_int<omega_t>(fx_raw<angle_t>(255), INT32_MIN).v, (i32)ref_rne_shr((i64)255 * INT32_MIN, 8));   // 255/256 * -2^31 is the largest in-contract magnitude at narrow 8 (1023 fitted only at the old narrow 10)
    TL_EXPECT_EQ(mul_int<vel_t>(fx_raw<pos_t>(1 << 20), 511).v, (1 << 20) * 511 * 4);   // 2^29 * 4 = 2^31 - 4 * 2^20: fits
    TL_EXPECT_EQ(mul_int<vel_t>(fx_raw<pos_t>(INT32_MIN), 0).v, 0);
    // mul with INT32_MIN on both sides: 2^62 >> S
    TL_EXPECT_EQ(mul<q_t>(fx_raw<q_t>(INT32_MIN), fx_raw<q_t>(1 << 29)).v, -(1 << 30));     // -2 * 0.5 = -1
    TL_EXPECT_EQ(mul<pos_t>(fx_raw<vel_t>(INT32_MIN), H).v, (i32)ref_rne_shr((i64)INT32_MIN * H.v, 32));
    TL_EXPECT_EQ(mul<pos_t>(fx_raw<q_t>(INT32_MIN), fx_raw<pos_t>(1 << 29)).v, -(1 << 30));   // -2^31 * 2^29 >> 30 = -2^30
    // to<R> at the widen edge: the last value that fits and the identity on a 64-bit local
    TL_EXPECT_EQ(to<q_t>(fx_raw<pos_t>((1 << 19) - 1)).v, ((1 << 19) - 1) << 12);
    TL_EXPECT_EQ(to<q_t>(fx_raw<pos_t>(-(1 << 19))).v, INT32_MIN);               // -2^31 fits exactly
    TL_EXPECT_EQ(to<pos2_wide_t>(fx_raw<pos2_wide_t>(INT64_MIN)).v, INT64_MIN);
    TL_EXPECT_EQ(to<pos_t>(fx_raw<pos2_wide_t>(-((i64)INT32_MAX << 18) - ((i64)1 << 17))).v, INT32_MIN);   // -(2^31 - 0.5): tie between odd and even -> INT32_MIN
    // fx_int / fx_lit at the exact range limits
    TL_EXPECT_EQ(fx_int<q_t>(1).v, 1 << 30);
    TL_EXPECT_EQ(fx_int<q_t>(-1).v, -(1 << 30));
    TL_EXPECT_EQ(fx_lit<q_t>(-2, 1).v, INT32_MIN);                                // -2.0 is representable
    TL_EXPECT_EQ(fx_lit<q_t>(INT32_MAX, 1 << 30).v, INT32_MAX);
    TL_EXPECT_EQ(fx_lit<pos_t>(-8192, 1).v, INT32_MIN);
    TL_EXPECT_EQ(fx_lit<pos_t>(((i64)1 << 44) - 1, (i64)1 << 32).v, 1 << 30);    // 4096 m - 2^-32: rounds to 2^30 raw
    // dot / len2_wide just below the 2^63 edge: no false saturation
    vec2<pos_t> big = { fx_raw<pos_t>(INT32_MIN), fx_raw<pos_t>(INT32_MAX) };
    TL_EXPECT_EQ(dot<pos2_wide_t>(big, big).v, ((i64)1 << 62) + (i64)INT32_MAX * INT32_MAX);
    TL_EXPECT_EQ(len2_wide(big), ((i64)1 << 62) + (i64)INT32_MAX * INT32_MAX);
    TL_EXPECT_EQ(cross<pos2_wide_t>(big, big).v, (i64)0);
    vec2<pos_t> nb = { fx_raw<pos_t>(INT32_MIN), fx_raw<pos_t>(INT32_MIN) };
    vec2<pos_t> pb = { fx_raw<pos_t>(INT32_MAX), fx_raw<pos_t>(INT32_MIN) };
    TL_EXPECT_EQ(cross<pos2_wide_t>(nb, pb).v, ((i64)1 << 62) - (i64)INT32_MIN * INT32_MAX);   // 2^62 + 2^62 - 2^31: fits
    // sqrt: the largest n whose nearest root is still INT32_MAX (y + 1 would be 2^31)
    TL_EXPECT_EQ(sqrt<pos_t>(fx_raw<pos2_wide_t>((i64)INT32_MAX * INT32_MAX + INT32_MAX)).v, INT32_MAX);
    TL_EXPECT_EQ(sqrt<q_t>(fx_raw<q_t>(INT32_MAX)).v, 1518500250);               // sqrt(2 - eps) Q30
    TL_EXPECT_EQ(sqrt<pos_t>(fx_raw<pos_t>(INT32_MAX)).v, 23726566);             // sqrt(8192 m - eps) = 90.51 m
    // abs / sat_neg / sign on MIN for both reps
    TL_EXPECT_EQ(abs(fx_raw<pos2_wide_t>(INT64_MIN)).v, INT64_MAX);
    TL_EXPECT_EQ(sign(fx_raw<pos_t>(INT32_MIN)), -1);
    TL_EXPECT_EQ(sat_neg(fx_raw<pos2_wide_t>(INT64_MIN)).v, INT64_MAX);
    // atan2 with INT32_MIN on both axes and the q_t/pos_t overloads agreeing bit for bit
    TL_EXPECT_EQ(atan2q(fx_raw<q_t>(INT32_MIN), fx_raw<q_t>(INT32_MIN)).v, atan2(fx_raw<pos_t>(INT32_MIN), fx_raw<pos_t>(INT32_MIN)).v);
    TL_EXPECT_EQ(atan2q(fx_raw<q_t>(INT32_MIN), fx_raw<q_t>(1)).v, -QUARTER_TURN.v);
}

TL_TEST(fx_review_release_error_values, "foundation,fx,det,fast") {
    // Every documented "with asserts compiled out, returns X" value (fx.h, det_math.h,
    // fx_float.h). In dev these paths trap (the runner lane's fatal-expected tests, TODO.md);
    // netcode/ship run this body - before W1 fx review 3 none of them had a test anywhere.
    // (The generated test list registers every TL_TEST, so the #if is inside the body.)
#if TL_DEV
    TL_SKIP("dev is the trap tier: these calls fatal here, and fx_fatal.test.cpp is the "
            "TL_TEST_EXPECT_FATAL half of the same contract");
#else
    TL_EXPECT_EQ(div<q_t>(fx_raw<pos_t>(5), fx_raw<pos_t>(0)).v, INT32_MAX);           // sign(a) * INT32_MAX
    TL_EXPECT_EQ(div<q_t>(fx_raw<pos_t>(-5), fx_raw<pos_t>(0)).v, -INT32_MAX);
    TL_EXPECT_EQ(div<q_t>(fx_raw<pos_t>(0), fx_raw<pos_t>(0)).v, 0);
    TL_EXPECT_EQ(sqrt<pos_t>(fx_raw<pos_t>(-1)).v, 0);                                  // x < 0 -> 0
    TL_EXPECT_EQ(sqrt<pos_t>(fx_raw<pos2_wide_t>(INT64_MIN)).v, 0);
    TL_EXPECT_EQ(rsqrt<q_t>(fx_raw<q_t>(0)).v, INT32_MAX);                              // div<R>(1, 0) saturated
    TL_EXPECT_EQ(atan2q(fx_raw<q_t>(0), fx_raw<q_t>(0)).v, 0);
    TL_EXPECT_EQ(atan2(fx_raw<pos_t>(0), fx_raw<pos_t>(0)).v, 0);
    TL_EXPECT_TRUE(normalize(vec2<pos_t>{ fx_raw<pos_t>(0), fx_raw<pos_t>(0) }) == (vec2<q_t>{ fx_raw<q_t>(0), fx_raw<q_t>(0) }));
    volatile f32 z32 = 0.0f;
    volatile f64 z64 = 0.0;
    const f32 nan32 = z32 / z32, inf32 = 1.0f / z32;
    const f64 nan64 = z64 / z64, inf64 = 1.0 / z64;
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(nan32).v, 0);                                // NaN -> 0
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(inf32).v, INT32_MAX);                        // +inf -> clamp
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(-inf32).v, INT32_MIN);
    TL_EXPECT_EQ(from_f64_quantized<q_t>(nan64).v, 0);
    TL_EXPECT_EQ(from_f64_quantized<q_t>(inf64).v, INT32_MAX);
    TL_EXPECT_EQ(from_f64_quantized<q_t>(-inf64).v, INT32_MIN);
    // to<R> out of range: the sixth row of fx_fatal.test.cpp's dev-tier asserts, which had no
    // release-value counterpart anywhere until now (TODO.md). fx.h documents the value: the
    // intermediate is converted to R::rep by C++20's well-defined MODULAR conversion, so it
    // wraps rather than saturating. The intermediate here is exactly 2^31, one quantum past the
    // last value to<q_t> can hold (the edge-matrix test pins -(1 << 19) as the last that fits),
    // and it comes back as INT32_MIN - a positive value arriving as the most negative one, which
    // is precisely why fx.h now tells callers on this tier to range-check.
    TL_EXPECT_EQ(to<q_t>(fx_raw<pos_t>(1 << 19)).v, INT32_MIN);
    // The negative side of the same wrap (RR-16, wrap RATIFIED 2026-08-25): the intermediate is
    // -(2^31) - 4096, one quantum past the most negative value to<q_t> can hold, and modular
    // conversion brings it back as INT32_MAX - 4095 - a negative value arriving as a large
    // positive one. Pinned so the wrap contract is covered from both directions.
    TL_EXPECT_EQ(to<q_t>(fx_raw<pos_t>(-(1 << 19) - 1)).v, INT32_MAX - 4095);
    // clamp with lo > hi is documented as asserted only: it returns lo for x < lo, else hi for
    // x > hi, else x - stated here so a change shows up
    TL_EXPECT_EQ(clamp(fx_int<pos_t>(0), fx_int<pos_t>(5), fx_int<pos_t>(-5)).v, fx_int<pos_t>(5).v);
#endif
}
