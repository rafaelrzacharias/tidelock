// fx_rne.test.cpp - the two rounding primitives, rne_shr and rne_div (docs/FX-PALETTE.md §10.1,
// §10.5 "fx_rne"). Everything that narrows in the sim goes through one of these two; the
// exhaustive part is the whole 16-bit domain for every shift, the property part covers i64.
// Spec: docs/FX-PALETTE.md §10.1. Rubric: docs/TESTING.md §7 (edge matrix: 0/1/many, min/max, ties).
#include "fx_test_util.h"

using namespace fx;

TL_TEST(fx_rne_shr_tie_table, "foundation,fx,fast") {
    // For every s in 1..62: +-half rounds to the even neighbour, just over/under half does not.
    for (int s = 1; s <= 62; ++s) {
        const i64 unit = (i64)1 << s;
        const i64 half = unit >> 1;
        // 0 + half -> 0 (even), 1*unit + half -> 2 (even), 2*unit + half -> 2, 3*unit + half -> 4
        TL_EXPECT_EQ(rne_shr(half, s), (i64)0);
        if (s < 62) {
            TL_EXPECT_EQ(rne_shr(unit + half, s), (i64)2);
            TL_EXPECT_EQ(rne_shr(2 * unit + half, s), (i64)2);
            TL_EXPECT_EQ(rne_shr(3 * unit + half, s), (i64)4);
            TL_EXPECT_EQ(rne_shr(unit + half + 1, s), (i64)2);
            TL_EXPECT_EQ(rne_shr(unit + half - 1, s), (i64)1);
            // negative side: -half -> 0, -(unit + half) -> -2, -(2 unit + half) -> -2
            TL_EXPECT_EQ(rne_shr(-half, s), (i64)0);
            TL_EXPECT_EQ(rne_shr(-(unit + half), s), (i64)-2);
            TL_EXPECT_EQ(rne_shr(-(2 * unit + half), s), (i64)-2);
            TL_EXPECT_EQ(rne_shr(-(unit + half) + 1, s), (i64)-1);
            TL_EXPECT_EQ(rne_shr(-(unit + half) - 1, s), (i64)-2);
        }
        // exact multiples are unchanged
        TL_EXPECT_EQ(rne_shr(unit, s), (i64)1);
        TL_EXPECT_EQ(rne_shr(-unit, s), (i64)-1);
        TL_EXPECT_EQ(rne_shr(0, s), (i64)0);
    }
    TL_EXPECT_EQ(rne_shr(12345, 0), (i64)12345);
    TL_EXPECT_EQ(rne_shr(-12345, 0), (i64)-12345);
    // extremes: INT64_MAX >> 62 = 1 rem 2^62-1 > half -> 2; INT64_MIN >> 62 = -2 exactly
    TL_EXPECT_EQ(rne_shr(INT64_MAX, 62), (i64)2);
    TL_EXPECT_EQ(rne_shr(INT64_MIN, 62), (i64)-2);
    TL_EXPECT_EQ(rne_shr(INT64_MAX, 1), ((i64)1 << 62));    // 2^62 - 1 + tie(odd) -> 2^62
}

TL_TEST(fx_rne_shr_exhaustive_16bit, "foundation,fx,fast") {
    // Every 16-bit input, every shift 1..16, against the sign-magnitude reference.
    u32 mismatches = 0;
    for (int s = 1; s <= 16; ++s) {
        for (i32 x = -32768; x <= 32767; ++x) {
            if (rne_shr(x, s) != ref_rne_shr(x, s)) ++mismatches;
        }
    }
    TL_EXPECT_EQ(mismatches, 0u);
}

TL_TEST(fx_rne_shr_property_i64, "foundation,fx,fast") {
    FxRng rng = { 0x6678'7265'5f73'6872ull };    // "fx_rne_shr"
    u32 mismatches = 0;
    for (u32 i = 0; i < 1000000u; ++i) {
        const i64 x = fx_rng_i64(&rng);
        const int s = (int)(fx_rng_next(&rng) % 62u) + 1;
        if (rne_shr(x, s) != ref_rne_shr(x, s)) ++mismatches;
    }
    TL_EXPECT_EQ(mismatches, 0u);
}

TL_TEST(fx_rne_div_exhaustive_small, "foundation,fx,fast") {
    // Every (n, d) in [-256, 255] x [-256, 255] \ {d = 0} against the reference.
    u32 mismatches = 0;
    for (i32 n = -256; n <= 255; ++n) {
        for (i32 d = -256; d <= 255; ++d) {
            if (d == 0) continue;
            if (rne_div(n, d) != ref_rne_div(n, d)) ++mismatches;
        }
    }
    TL_EXPECT_EQ(mismatches, 0u);
    // the tie table in rational form: +-1/2 -> 0, +-3/2 -> +-2, +-5/2 -> +-2, +-7/2 -> +-4
    TL_EXPECT_EQ(rne_div(1, 2), (i64)0);
    TL_EXPECT_EQ(rne_div(-1, 2), (i64)0);
    TL_EXPECT_EQ(rne_div(3, 2), (i64)2);
    TL_EXPECT_EQ(rne_div(-3, 2), (i64)-2);
    TL_EXPECT_EQ(rne_div(5, 2), (i64)2);
    TL_EXPECT_EQ(rne_div(7, 2), (i64)4);
    TL_EXPECT_EQ(rne_div(3, -2), (i64)-2);
    TL_EXPECT_EQ(rne_div(-3, -2), (i64)2);
    // exact quotients and zero numerator
    TL_EXPECT_EQ(rne_div(0, 7), (i64)0);
    TL_EXPECT_EQ(rne_div(0, -7), (i64)0);
    TL_EXPECT_EQ(rne_div(21, 7), (i64)3);
    TL_EXPECT_EQ(rne_div(-21, 7), (i64)-3);
}

TL_TEST(fx_rne_div_property_i64, "foundation,fx,fast") {
    FxRng rng = { 0x6678'5f72'6e65'5f64ull };    // "fx_rne_d"
    u32 mismatches = 0;
    for (u32 i = 0; i < 1000000u; ++i) {
        // |n| < 2^62 and |d| < 2^31 keeps both implementations inside their preconditions
        const i64 n = fx_rng_i64(&rng) >> 2;
        i64 d = (i64)fx_rng_i32(&rng);
        if (d == 0) d = 1;
        if (rne_div(n, d) != ref_rne_div(n, d)) ++mismatches;
    }
    TL_EXPECT_EQ(mismatches, 0u);
    // the documented extremes
    TL_EXPECT_EQ(rne_div(INT64_MAX, 1), INT64_MAX);
    TL_EXPECT_EQ(rne_div(INT64_MIN, 1), INT64_MIN);
    TL_EXPECT_EQ(rne_div(INT64_MIN + 1, -1), INT64_MAX);
}
