// fx_decimal.test.cpp - fx_parse_decimal_raw/fx_parse_decimal (RR-38, docs/FX-PALETTE.md §10.6):
// the integer-only decimal-to-fixed-point quantizer. Rubric: docs/TESTING.md §7 (edge matrix:
// 0/1/many, min/max, ties) - the RNE ruling explicitly asked for the same evidence standard as
// the fx trace tests, not a smoke test, so this file is pinned known-answer vectors, not
// property/fuzz coverage (the parser has no cross-ISA float path to fuzz against - it never
// touches a float).
#include "fx_test_util.h"

using namespace fx;

TL_TEST(fx_decimal_pinned_pos_t_one_five, "foundation,fx,fast") {
    // The ruling's own pinned case: 1.5 into pos_t (FRAC=18) yields raw 0x60000 (393216 =
    // 1.5 * 262144, exact - no rounding needed here at all).
    const Result<pos_t> r = fx_parse_decimal<pos_t>(sv_lit("1.5"));
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_EXPECT_EQ(r.value.v, (i32)0x60000);
    TL_EXPECT_EQ(r.value.v, 393216);
}

TL_TEST(fx_decimal_raw_matches_typed_wrapper, "foundation,fx,fast") {
    const Result<i32> raw = fx_parse_decimal_raw(sv_lit("1.5"), 18u);
    TL_ASSERT_EQ(raw.err, ERR_OK);
    TL_EXPECT_EQ(raw.value, 393216);
}

TL_TEST(fx_decimal_integer_only_literal, "foundation,fx,fast") {
    const Result<pos_t> r = fx_parse_decimal<pos_t>(sv_lit("3"));
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_EXPECT_EQ(r.value.v, 3 * pos_t::ONE);
}

TL_TEST(fx_decimal_negative_literal, "foundation,fx,fast") {
    const Result<pos_t> r = fx_parse_decimal<pos_t>(sv_lit("-2.5"));
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_EXPECT_EQ(r.value.v, -(i32)(5 * pos_t::ONE / 2));   // -2.5 * 262144 = -655360
    TL_EXPECT_EQ(r.value.v, -655360);
}

TL_TEST(fx_decimal_leading_plus_and_bare_forms, "foundation,fx,fast") {
    TL_EXPECT_EQ(fx_parse_decimal<pos_t>(sv_lit("+1.5")).value.v, 393216);
    TL_EXPECT_EQ(fx_parse_decimal<pos_t>(sv_lit("5.")).value.v, 5 * pos_t::ONE);   // trailing dot
    TL_EXPECT_EQ(fx_parse_decimal<pos_t>(sv_lit(".5")).value.v, pos_t::ONE / 2);   // leading dot
    TL_EXPECT_EQ(fx_parse_decimal<pos_t>(sv_lit("0")).value.v, 0);
    TL_EXPECT_EQ(fx_parse_decimal<pos_t>(sv_lit("0.0")).value.v, 0);
    TL_EXPECT_EQ(fx_parse_decimal<pos_t>(sv_lit("-0")).value.v, 0);
}

TL_TEST(fx_decimal_malformed_literals_named_error, "foundation,fx,fast") {
    // Every one of these must fail with ERR_FX_PARSE, not crash, not silently parse a prefix.
    const char* bad[] = { "", "-", "+", ".", "-.", "1.2.3", "1a", "a1", "1 ", " 1", "1-", "1+2",
                           "--1", "1..", "1.2a", "..", "1.2." };
    for (const char* s : bad) {
        const Result<pos_t> r = fx_parse_decimal<pos_t>(sv(s));
        TL_EXPECT_EQ(r.err, ERR_FX_PARSE);
    }
}

TL_TEST(fx_decimal_empty_strview_named_error, "foundation,fx,fast") {
    const Result<pos_t> r = fx_parse_decimal<pos_t>(StrView{ "", 0u });
    TL_EXPECT_EQ(r.err, ERR_FX_PARSE);
}

TL_TEST(fx_decimal_out_of_range_named_error_not_assert, "foundation,fx,fast") {
    // pos_t's INT_BITS is 13 (+-8192-ish); this is far past it, and must come back as a named
    // error - not a TL_FATAL - because it is untrusted text, exactly the case this file's own
    // header note draws the Result-vs-assert line over.
    TL_EXPECT_EQ(fx_parse_decimal<pos_t>(sv_lit("999999")).err, ERR_FX_RANGE);
    TL_EXPECT_EQ(fx_parse_decimal<pos_t>(sv_lit("-999999")).err, ERR_FX_RANGE);
    // 19 fractional digits: den = 10^19 does not fit u64 at all (the 18-digit cap).
    TL_EXPECT_EQ(fx_parse_decimal<pos_t>(sv_lit("0.1234567890123456789")).err, ERR_FX_RANGE);
    // A numerator that overflows u64 outright during digit accumulation.
    TL_EXPECT_EQ(fx_parse_decimal<pos_t>(sv_lit("99999999999999999999999999999999")).err, ERR_FX_RANGE);
}

TL_TEST(fx_decimal_rne_ties_to_even_low_frac, "foundation,fx,fast") {
    // Short, hand-verifiable ties using fx<i32,1> (ONE = 2) rather than a 19-fractional-digit
    // literal at pos_t's own FRAC=18 (the exact tie there needs FRAC+1 = 19 fractional decimal
    // digits, which the parser's own 18-digit cap refuses by design - true of every real palette
    // row's own resolution boundary, not a gap in the parser). At FRAC=1: raw 0 -> 0.0, raw 1 ->
    // 0.5, raw 2 -> 1.0; the exact midpoints are 0.25 (between 0 and 1) and 0.75 (between 1 and
    // 2), both two-digit decimals.
    using F1 = ::fx::fx<i32, 1>;
    // 0.25 is the tie between raw 0 (even) and raw 1 (odd) -> rounds to 0.
    TL_EXPECT_EQ(fx_parse_decimal<F1>(sv_lit("0.25")).value.v, 0);
    // -0.25 mirrors it (ties to even is symmetric) -> rounds to 0.
    TL_EXPECT_EQ(fx_parse_decimal<F1>(sv_lit("-0.25")).value.v, 0);
    // 0.75 is the tie between raw 1 (odd) and raw 2 (even) -> rounds to 2, not 1.
    TL_EXPECT_EQ(fx_parse_decimal<F1>(sv_lit("0.75")).value.v, 2);
    TL_EXPECT_EQ(fx_parse_decimal<F1>(sv_lit("-0.75")).value.v, -2);
    // 1.25 is the tie between raw 2 (even) and raw 3 (odd) -> rounds to 2.
    TL_EXPECT_EQ(fx_parse_decimal<F1>(sv_lit("1.25")).value.v, 2);
    // Just past the tie either way does NOT round to even - it rounds to the nearer value.
    TL_EXPECT_EQ(fx_parse_decimal<F1>(sv_lit("0.26")).value.v, 1);   // past 0.25 -> nearer to 1 (0.5)
    TL_EXPECT_EQ(fx_parse_decimal<F1>(sv_lit("0.24")).value.v, 0);   // short of 0.25 -> nearer to 0
}

TL_TEST(fx_decimal_rne_tie_matches_rne_div_reference, "foundation,fx,fast") {
    // Cross-check against fx_test_util.h's own independent RNE derivation (ref_rne_div), not
    // just against hand-picked expected values - the same "two derivations checking each other"
    // shape fx_rne.test.cpp already uses for rne_div itself.
    using F3 = ::fx::fx<i32, 3>;   // ONE = 8; ULP = 1/8 = 0.125
    for (i32 raw = -20; raw <= 20; ++raw) {
        // The exact midpoint between raw and raw+1, as an integer rational (2*raw+1)/16, printed
        // as a decimal literal computed independently of the parser under test.
        const i64 num2 = (i64)raw * 2 + 1;   // *2 relative to ONE=8 -> /16 denominator
        // decimal = num2 / 16, computed digit-by-digit via long division (den is a power of two
        // ⇒ always an exact, finite decimal - never a repeating fraction).
        char buf[32];
        u32 n = 0;
        i64 whole = num2 / 16;
        i64 rem = num2 % 16;
        bool neg = false;
        if (whole < 0 || (whole == 0 && rem < 0)) { neg = true; whole = -whole; rem = -rem; }
        // Render "whole.frac" where frac is rem*10^k/16 for enough digits to be exact (16 = 2^4,
        // so 4 more decimal digits beyond the point always terminate it exactly).
        char tmp[24];
        u32 tn = 0;
        i64 w = whole;
        if (w == 0) { tmp[tn++] = '0'; }
        while (w > 0) { tmp[tn++] = (char)('0' + (w % 10)); w /= 10; }
        if (neg) { buf[n++] = '-'; }
        for (u32 k = tn; k > 0; --k) { buf[n++] = tmp[k - 1]; }
        buf[n++] = '.';
        i64 frac10000 = rem * 10000 / 16;   // exact: rem in [0,15], *10000 divisible by 16 always
        char fbuf[8]; u32 fn = 0; i64 f = frac10000;
        for (u32 k = 0; k < 4; ++k) { fbuf[3 - k] = (char)('0' + (f % 10)); f /= 10; }
        fn = 4;
        for (u32 k = 0; k < fn; ++k) { buf[n++] = fbuf[k]; }

        const Result<F3> got = fx_parse_decimal<F3>(StrView{ buf, n });
        TL_ASSERT_EQ(got.err, ERR_OK);
        const i64 expect = ref_rne_div(num2, 2);   // the exact rational (num2/16)*ONE(8) = num2/2
        TL_EXPECT_EQ((i64)got.value.v, expect);
    }
}

TL_TEST(fx_decimal_runtime_frac_matches_every_palette_row, "foundation,fx,fast") {
    // fx_parse_decimal_raw is what both real callers (Inspector's FieldKind walker, Console's
    // CvarDesc::frac_bits) actually call - a runtime frac, not a compile-time row. Exercise it at
    // every palette FRAC directly, matching each row's own typed wrapper result.
    TL_EXPECT_EQ(fx_parse_decimal_raw(sv_lit("1.5"), 18u).value, fx_parse_decimal<pos_t>(sv_lit("1.5")).value.v);
    TL_EXPECT_EQ(fx_parse_decimal_raw(sv_lit("1.5"), 20u).value, fx_parse_decimal<vel_t>(sv_lit("1.5")).value.v);
    TL_EXPECT_EQ(fx_parse_decimal_raw(sv_lit("1.5"), 30u).value, fx_parse_decimal<stiff_t>(sv_lit("1.5")).value.v);
    TL_EXPECT_EQ(fx_parse_decimal_raw(sv_lit("1.5"), 16u).value, fx_parse_decimal<scalar_t>(sv_lit("1.5")).value.v);
}
