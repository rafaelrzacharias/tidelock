// fx_fatal.test.cpp - the dev-tier fatal half of the release-value contract that
// fx_review_release_error_values (fx_review.test.cpp) checks the netcode/ship side of. TODO.md
// "fx tests that need the runner lane": div by zero, sqrt of a negative, normalize of a zero
// vector, atan2(0,0), to<R> out of range, clamp with lo > hi - each is documented (fx.h,
// det_math.h) to TL_ASSERT in debug/dev and return a specific value once TL_ASSERT compiles out.
// This file proves the ASSERT half fires; fx_review's #else branch proves the RETURNED VALUE.
//
// Every case runs only in TL_DEV: TL_ASSERT is `((void)0)` in netcode/ship (docs/CPP-SUBSET.md
// §7b), so there is nothing to fatal there - the call is instead exercised for its return value
// by fx_review_release_error_values. The generated test list registers every TL_TEST_EXPECT_FATAL
// unconditionally (`docs/LESSONS.md`: a tier-conditional test puts the #if inside the body), so
// the non-dev tiers run a body that does nothing and passes; the runner's child_passes (W1
// runner lane, tests/runner/main.cpp) judges a TL_TEST_EXPECT_FATAL as an ordinary must-exit-0
// test outside TL_DEV for the same reason - never a link failure, never a false fatal
// expectation on a tier where the assert cannot fire.
//
// Spec: docs/TESTING.md §9.1 (TL_TEST_EXPECT_FATAL: child process, exit code + stderr marker -
// tests/runner/tl_test.h documents the current gap: matched on abnormal exit until tooling-rt's
// crash writer replaces the tl_fatal trap stub, TODO.md). No `volatile`/constexpr-defeating
// tricks needed: TL_ASSERT is a real runtime call to tl_assert_failed (fx.h/det_math.h are
// `inline`/`constexpr` but called here at runtime with ordinary values), so it fires exactly
// where the source says it does.
#include "runner/tl_test.h"
#include "foundation/det_math.h"

using namespace fx;

TL_TEST_EXPECT_FATAL(fx_fatal_div_by_zero, "foundation,fx,fatal") {
    (void)t;
#if TL_DEV
    (void)div<q_t>(fx_raw<pos_t>(5), fx_raw<pos_t>(0));
#endif
}

TL_TEST_EXPECT_FATAL(fx_fatal_sqrt_of_negative, "foundation,fx,fatal") {
    (void)t;
#if TL_DEV
    (void)sqrt<pos_t>(fx_raw<pos_t>(-1));
#endif
}

TL_TEST_EXPECT_FATAL(fx_fatal_normalize_zero_vector, "foundation,fx,fatal") {
    (void)t;
#if TL_DEV
    (void)normalize(vec2<pos_t>{ fx_raw<pos_t>(0), fx_raw<pos_t>(0) });
#endif
}

TL_TEST_EXPECT_FATAL(fx_fatal_atan2_zero_zero, "foundation,fx,fatal") {
    (void)t;
#if TL_DEV
    (void)atan2(fx_raw<pos_t>(0), fx_raw<pos_t>(0));
#endif
}

TL_TEST_EXPECT_FATAL(fx_fatal_to_out_of_range, "foundation,fx,fatal") {
    (void)t;
#if TL_DEV
    // pos_t (1<<19) is one quantum past the last value to<q_t> can hold: fx_review's edge-matrix
    // test pins -(1<<19) as the last value that DOES fit (to<q_t>(...).v == INT32_MIN).
    (void)to<q_t>(fx_raw<pos_t>(1 << 19));
#endif
}

TL_TEST_EXPECT_FATAL(fx_fatal_clamp_lo_gt_hi, "foundation,fx,fatal") {
    (void)t;
#if TL_DEV
    (void)clamp(fx_int<pos_t>(0), fx_int<pos_t>(5), fx_int<pos_t>(-5));
#endif
}
