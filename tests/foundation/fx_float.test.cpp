// fx_float.test.cpp - the float bridge (docs/FX-PALETTE.md §6, §10.4). Test code may use floats:
// tests/ is not a sim TU and fx_float.h is the one header built for them.
// Spec: docs/FX-PALETTE.md §10.4. Rubric: docs/TESTING.md §7.
#include "fx_test_util.h"
#include "foundation/fx_float.h"

using namespace fx;

TL_TEST(fx_float_to_f32_f64_exact_cases, "foundation,fx,fast") {
    TL_EXPECT_TRUE(to_f32(fx_int<pos_t>(1)) == 1.0f);
    TL_EXPECT_TRUE(to_f32(fx_int<pos_t>(-3)) == -3.0f);
    TL_EXPECT_TRUE(to_f32(TEXEL) == 0.0625f);
    TL_EXPECT_TRUE(to_f32(fx_raw<pos_t>(1)) == 1.0f / 262144.0f);
    TL_EXPECT_TRUE(to_f32(fx_raw<q_t>(1 << 29)) == 0.5f);
    TL_EXPECT_TRUE(to_f64(fx_raw<q_t>(1)) == 1.0 / 1073741824.0);
    TL_EXPECT_TRUE(to_f64(fx_raw<pos_t>(INT32_MAX)) == 2147483647.0 / 262144.0);   // exact in f64
    TL_EXPECT_TRUE(to_f64(fx_raw<pos2_wide_t>((i64)1 << 36)) == 1.0);
    TL_EXPECT_TRUE(to_f32(fx_raw<pos_t>(0)) == 0.0f);
    // f32 loses nothing below 2^24 raw (docs/FX-PALETTE.md §6)
    u32 bad = 0;
    for (i32 r = -(1 << 24); r <= (1 << 24); r += 997) {
        if (to_f32(fx_raw<pos_t>(r)) * 262144.0f != (f32)r) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
}

TL_TEST(fx_float_quantize_rne_and_clamp, "foundation,fx,fast") {
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(0.0f).v, 0);
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(1.0f).v, 1 << 18);
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(-1.0f).v, -(1 << 18));
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(0.0625f).v, TEXEL.v);
    TL_EXPECT_EQ(from_f32_quantized<q_t>(0.5f).v, 1 << 29);
    TL_EXPECT_EQ(from_f32_quantized<q_t>(-0.5f).v, -(1 << 29));
    // ties to even at the row quantum: 2.5 quanta -> 2, 3.5 -> 4, -2.5 -> -2
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(2.5f / 262144.0f).v, 2);
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(3.5f / 262144.0f).v, 4);
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(-2.5f / 262144.0f).v, -2);
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(-3.5f / 262144.0f).v, -4);
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(2.4999f / 262144.0f).v, 2);
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(2.5001f / 262144.0f).v, 3);
    TL_EXPECT_EQ(from_f64_quantized<pos_t>(2.5 / 262144.0).v, 2);
    TL_EXPECT_EQ(from_f64_quantized<pos_t>(-3.5 / 262144.0).v, -4);
    TL_EXPECT_EQ(from_f64_quantized<q_t>(1.0 / 3.0).v, 357913941);
    // clamps at the row's range
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(1.0e9f).v, INT32_MAX);
    TL_EXPECT_EQ(from_f32_quantized<pos_t>(-1.0e9f).v, INT32_MIN);
    TL_EXPECT_EQ(from_f32_quantized<q_t>(2.0f).v, INT32_MAX);          // 2.0 * 2^30 = 2^31: clamped
    TL_EXPECT_EQ(from_f32_quantized<q_t>(-2.0f).v, INT32_MIN);         // exactly representable
    TL_EXPECT_EQ(from_f64_quantized<pos_t>(8192.0).v, INT32_MAX);
    TL_EXPECT_EQ(from_f64_quantized<pos_t>(-8192.0).v, INT32_MIN);
    // round trip: every seeded raw value survives f64 exactly and f32 within the f32 quantum
    FxRng rng = { 0x666c6f6174ull };
    u32 bad = 0;
    for (u32 i = 0; i < (1u << 18); ++i) {
        const i32 r = fx_rng_i32(&rng);
        if (from_f64_quantized<pos_t>(to_f64(fx_raw<pos_t>(r))).v != r) ++bad;
        if (from_f64_quantized<q_t>(to_f64(fx_raw<q_t>(r))).v != r) ++bad;
        const i32 back = from_f32_quantized<pos_t>(to_f32(fx_raw<pos_t>(r))).v;
        const i64 ar = r < 0 ? -(i64)r : (i64)r;
        i64 tol = 1;
        while ((ar >> 24) >= tol) tol *= 2;          // f32 has 24 bits: the quantum grows with |r|
        if (!fx_near_raw(back, r, tol)) ++bad;
    }
    TL_EXPECT_EQ(bad, 0u);
}
