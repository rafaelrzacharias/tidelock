// fx_fatal.test.cpp - the dev-tier fatal half of the release-value contract that
// fx_review_release_error_values (fx_review.test.cpp) checks the netcode/ship side of. TODO.md
// "fx tests that need the runner lane": div by zero, sqrt of a negative, normalize of a zero
// vector, atan2(0,0), to<R> out of range, clamp with lo > hi - each is documented (fx.h,
// det_math.h) to TL_ASSERT in debug/dev and return a specific value once TL_ASSERT compiles out.
// This file proves the ASSERT half fires; fx_review's #else branch proves the RETURNED VALUE for
// all six - the to<R> row landed 2026-08-24 (W1 ruling-closeout), with the documented
// out-of-range behaviour in fx.h beside it. The pairing is complete.
//
// Every case runs only in TL_DEV: TL_ASSERT is `((void)0)` in netcode/ship (docs/CPP-SUBSET.md
// §7b), so there is nothing to fatal there. The generated test list registers every
// TL_TEST_EXPECT_FATAL unconditionally (`docs/LESSONS.md`: a tier-conditional test puts the #if
// inside the body), so on those tiers each body TL_SKIPs - reported as SKIP, with its reason, in
// the TSV, the JUnit XML and the summary. It was an empty body reporting PASS until W1 runner
// review 1: six rows of green having executed nothing is the disarmed-tripwire shape the runner
// now refuses (a zero-check body is a FAIL; TL_SKIP is the sanctioned way to run no checks).
// The runner's tl_child_verdict (tests/runner/runner_core.h) judges a TL_TEST_EXPECT_FATAL row as
// a fatal expectation only when TL_DEV, and as an ordinary child otherwise - so the SKIP survives.
//
// Spec: docs/TESTING.md §9.1 (TL_TEST_EXPECT_FATAL: child process, exit code + stderr marker -
// tests/runner/tl_test.h documents the current gap: matched on abnormal exit until tooling-rt's
// crash writer replaces the tl_fatal trap stub AND the runner captures the child's stderr; both
// halves of the tightening are in TODO.md). No `volatile`/constexpr-defeating tricks needed:
// TL_ASSERT is a real runtime call to tl_assert_failed (fx.h/det_math.h are `inline`/`constexpr`
// but called here at runtime with ordinary values), so it fires exactly where the source says.
#include "runner/tl_test.h"
#include "foundation/det_math.h"

using namespace fx;

// One spelling, six rows (docs/CLAUDE.md: one fact, one home).
#define FX_FATAL_SKIP_REASON \
    "TL_ASSERT is ((void)0) outside TL_DEV, so this call cannot fatal here; " \
    "fx_review_release_error_values covers the returned value on this tier"

TL_TEST_EXPECT_FATAL(fx_fatal_div_by_zero, "foundation,fx,fatal") {
#if TL_DEV
    (void)t;
    (void)div<q_t>(fx_raw<pos_t>(5), fx_raw<pos_t>(0));
#else
    TL_SKIP(FX_FATAL_SKIP_REASON);
#endif
}

TL_TEST_EXPECT_FATAL(fx_fatal_sqrt_of_negative, "foundation,fx,fatal") {
#if TL_DEV
    (void)t;
    (void)sqrt<pos_t>(fx_raw<pos_t>(-1));
#else
    TL_SKIP(FX_FATAL_SKIP_REASON);
#endif
}

TL_TEST_EXPECT_FATAL(fx_fatal_normalize_zero_vector, "foundation,fx,fatal") {
#if TL_DEV
    (void)t;
    (void)normalize(vec2<pos_t>{ fx_raw<pos_t>(0), fx_raw<pos_t>(0) });
#else
    TL_SKIP(FX_FATAL_SKIP_REASON);
#endif
}

TL_TEST_EXPECT_FATAL(fx_fatal_atan2_zero_zero, "foundation,fx,fatal") {
#if TL_DEV
    (void)t;
    (void)atan2(fx_raw<pos_t>(0), fx_raw<pos_t>(0));
#else
    TL_SKIP(FX_FATAL_SKIP_REASON);
#endif
}

TL_TEST_EXPECT_FATAL(fx_fatal_to_out_of_range, "foundation,fx,fatal") {
#if TL_DEV
    (void)t;
    // pos_t (1<<19) is one quantum past the last value to<q_t> can hold: fx_review's edge-matrix
    // test pins -(1<<19) as the last value that DOES fit (to<q_t>(...).v == INT32_MIN).
    (void)to<q_t>(fx_raw<pos_t>(1 << 19));
#else
    TL_SKIP(FX_FATAL_SKIP_REASON);
#endif
}

TL_TEST_EXPECT_FATAL(fx_fatal_clamp_lo_gt_hi, "foundation,fx,fatal") {
#if TL_DEV
    (void)t;
    (void)clamp(fx_int<pos_t>(0), fx_int<pos_t>(5), fx_int<pos_t>(-5));
#else
    TL_SKIP(FX_FATAL_SKIP_REASON);
#endif
}
