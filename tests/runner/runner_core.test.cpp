// runner_core.test.cpp - the runner testing itself. Spec: docs/TESTING.md §1, §9.1.
//
// Why this file exists (W1 runner review 1): the runner shipped with zero tests of its own. Its
// glob matcher, tag matcher, suite parser, property seed and pass/fail predicate decided every
// other test's verdict and none of them had ever been exercised; NEAR_FX, SPAN_EQ and MEM_EQ are
// templates with no call site anywhere in the tree, so they had never even been INSTANTIATED -
// they were shipped as "completed" without a compiler having type-checked them.
// docs/LESSONS.md: an audit is worth what its negative test is worth, and the harness is the only
// determinism safety net there is (docs/TESTING.md §1).
//
// Every test here is a negative test as well as a positive one - the point is the cases that must
// NOT report success.
#include "runner/tl_test.h"
#include "foundation/det_math.h"   // fx_palette rows + the measured FX_*_MAX_ERR_ULP bounds

using namespace fx;

// --- selection: --filter -----------------------------------------------------------------------

TL_TEST(runner_glob_match, "runner,fast") {
    TL_EXPECT_TRUE(tl_glob_match("abc", "abc"));
    TL_EXPECT_TRUE(tl_glob_match("*", "abc"));
    TL_EXPECT_TRUE(tl_glob_match("*", ""));
    TL_EXPECT_TRUE(tl_glob_match("", ""));
    TL_EXPECT_TRUE(tl_glob_match("a*c", "abbbc"));
    TL_EXPECT_TRUE(tl_glob_match("a*c", "ac"));
    TL_EXPECT_TRUE(tl_glob_match("fx_fatal_*", "fx_fatal_div_by_zero"));
    TL_EXPECT_TRUE(tl_glob_match("a?c", "abc"));
    TL_EXPECT_TRUE(tl_glob_match("*abc*", "xxabcyy"));
    TL_EXPECT_TRUE(tl_glob_match("*a*b*", "zzazzbzz"));   // needs backtracking on both stars
    // Negatives: a pattern that matches everything is the silent-pass shape of a filter.
    TL_EXPECT_FALSE(tl_glob_match("", "abc"));
    TL_EXPECT_FALSE(tl_glob_match("abc", ""));
    TL_EXPECT_FALSE(tl_glob_match("abc", "abcd"));
    TL_EXPECT_FALSE(tl_glob_match("abcd", "abc"));
    TL_EXPECT_FALSE(tl_glob_match("a?c", "ac"));          // ? is exactly one, never zero
    TL_EXPECT_FALSE(tl_glob_match("a?c", "abbc"));
    TL_EXPECT_FALSE(tl_glob_match("fx_fatal_*", "fx_review_release_error_values"));
}

// --- selection: --tag --------------------------------------------------------------------------

TL_TEST(runner_has_tag, "runner,fast") {
    TL_EXPECT_TRUE(tl_has_tag("foundation,fx,fast", "foundation"));
    TL_EXPECT_TRUE(tl_has_tag("foundation,fx,fast", "fx"));
    TL_EXPECT_TRUE(tl_has_tag("foundation,fx,fast", "fast"));    // last element, no trailing comma
    TL_EXPECT_TRUE(tl_has_tag("solo", "solo"));
    // Negatives. A substring match here would silently include or exclude the wrong tests, and
    // "slow" vs "slowpath" is exactly the pair the PR lane's `--tag !slow` depends on.
    TL_EXPECT_FALSE(tl_has_tag("foundation,fx,fast", "f"));
    TL_EXPECT_FALSE(tl_has_tag("foundation,fx,fast", "as"));
    TL_EXPECT_FALSE(tl_has_tag("slowpath", "slow"));
    TL_EXPECT_FALSE(tl_has_tag("slow", "slowpath"));
    TL_EXPECT_FALSE(tl_has_tag("foundation,fx", "det"));
    TL_EXPECT_FALSE(tl_has_tag("", "fast"));
    TL_EXPECT_FALSE(tl_has_tag("fast", ""));                     // an empty tag matches nothing
    // Whitespace is stripped by cmake/testlist.cmake before it reaches here; pinned so that a
    // change there shows up as a failure rather than as tests quietly not matching.
    TL_EXPECT_FALSE(tl_has_tag("foundation, fx", "fx"));
}

// --- report: suite naming ----------------------------------------------------------------------

static bool suite_is(const char* path, const char* want) {
    char got[128];
    tl_suite_of(path, got, sizeof(got));
    return tl_mem_eq(got, want, __builtin_strlen(want) + 1);
}

TL_TEST(runner_suite_of, "runner,fast") {
    TL_EXPECT_TRUE(suite_is("tests/foundation/fx_rne.test.cpp", "foundation"));
    TL_EXPECT_TRUE(suite_is("tests\\foundation\\fx_rne.test.cpp", "foundation"));   // CMake may emit either
    TL_EXPECT_TRUE(suite_is("tests/runner/runner_core.test.cpp", "runner"));
    TL_EXPECT_TRUE(suite_is("bare.test.cpp", "bare.test.cpp"));                     // no directory part
    // Truncation must not overrun, and must still terminate.
    char small[4];
    tl_suite_of("tests/foundation/x.test.cpp", small, sizeof(small));
    TL_EXPECT_EQ(small[3], 0);
    TL_EXPECT_TRUE(tl_mem_eq(small, "fou", 3));
}

// --- property seeding --------------------------------------------------------------------------

TL_TEST(runner_seed_is_a_pure_function, "runner,fast") {
    // Same inputs, same seed - the property is that a child re-executed with --run-one i --seed s
    // draws exactly what the in-process run would have drawn (docs/TESTING.md §1).
    TL_EXPECT_EQ(tl_seed_for(0, 0), tl_seed_for(0, 0));
    TL_EXPECT_EQ(tl_seed_for(12345, 7), tl_seed_for(12345, 7));
    // Different index, different seed; different --seed, different seed. Sixteen adjacent rows
    // must not collide, or a whole file of property tests shares one input set.
    TL_EXPECT_NE(tl_seed_for(0, 0), tl_seed_for(0, 1));
    TL_EXPECT_NE(tl_seed_for(0, 5), tl_seed_for(1, 5));
    u32 seen[16];
    for (u32 i = 0; i < 16; ++i) { seen[i] = tl_seed_for(99, i); }
    u32 collisions = 0;
    for (u32 i = 0; i < 16; ++i) {
        for (u32 j = i + 1; j < 16; ++j) { if (seen[i] == seen[j]) { ++collisions; } }
    }
    TL_EXPECT_EQ(collisions, 0u);
}

// --- verdicts: the in-process mapping ----------------------------------------------------------

TL_TEST(runner_ctx_verdict_zero_checks_is_a_failure, "runner,fast") {
    // The rule this file exists for: a body that recorded nothing is not a pass. A test whose
    // every check sits inside an #if this tier did not take (docs/LESSONS.md's tier-conditional
    // pattern) would otherwise report green having verified nothing.
    TL_EXPECT_EQ(tl_ctx_verdict(0, 0, false), VERDICT_FAIL);
    TL_EXPECT_EQ(tl_ctx_verdict(0, 1, false), VERDICT_PASS);
    TL_EXPECT_EQ(tl_ctx_verdict(1, 1, false), VERDICT_FAIL);
    TL_EXPECT_EQ(tl_ctx_verdict(1, 0, false), VERDICT_FAIL);
    // TL_SKIP is the sanctioned way to run no checks, and it is SKIP - never PASS.
    TL_EXPECT_EQ(tl_ctx_verdict(0, 0, true), VERDICT_SKIP);
    TL_EXPECT_EQ(tl_ctx_verdict(0, 3, true), VERDICT_SKIP);
    // A failure already recorded outranks a later skip: a body cannot skip away its own failure.
    TL_EXPECT_EQ(tl_ctx_verdict(1, 3, true), VERDICT_FAIL);
}

// --- verdicts: the child-process mapping -------------------------------------------------------

static ChildResult cr_of(bool spawned, bool abnormal, int code) {
    ChildResult r; r.spawned = spawned; r.abnormal = abnormal; r.timed_out = false; r.exit_code = code;
    return r;
}

// A child the runner killed on --timeout-ms. `code` is whatever the OS reported afterwards - the
// point of the timeout branch is that it does not matter.
static ChildResult cr_timed_out(bool abnormal, int code) {
    ChildResult r; r.spawned = true; r.abnormal = abnormal; r.timed_out = true; r.exit_code = code;
    return r;
}

TL_TEST(runner_child_verdict_ordinary_test, "runner,fast") {
    TL_EXPECT_EQ(tl_child_verdict(false, cr_of(true, false, 0)), VERDICT_PASS);
    TL_EXPECT_EQ(tl_child_verdict(false, cr_of(true, false, TL_EXIT_FAIL)), VERDICT_FAIL);
    TL_EXPECT_EQ(tl_child_verdict(false, cr_of(true, false, TL_EXIT_SKIP)), VERDICT_SKIP);
    // An ordinary test that crashed is a FAIL on every tier - a trap is never good news here.
    TL_EXPECT_EQ(tl_child_verdict(false, cr_of(true, true, -1)), VERDICT_FAIL);
    // An exit code nobody minted (127 from a failed execv, 2 from a real tl_fatal in a test that
    // was not marked fatal-expected) is a FAIL, not an unrecognised-therefore-fine.
    TL_EXPECT_EQ(tl_child_verdict(false, cr_of(true, false, 127)), VERDICT_FAIL);
    TL_EXPECT_EQ(tl_child_verdict(false, cr_of(true, false, 2)), VERDICT_FAIL);
}

TL_TEST(runner_child_verdict_fatal_expected, "runner,fast") {
    // Tier-agnostic (finding B): the real tl_fatal's controlled exit(2) is the expectation in
    // EVERY tier - TL_FATAL/TL_CHECK never compile out. A clean exit means the fatal never
    // fired; an ABNORMAL exit is an uncontrolled crash (stack overflow, segfault) - both fail.
    // A TL_ASSERT-triggered row TL_SKIPs in its own body outside dev, and the skip is honored.
    TL_EXPECT_EQ(tl_child_verdict(true, cr_of(true, false, TL_EXIT_FATAL)), VERDICT_PASS);
    TL_EXPECT_EQ(tl_child_verdict(true, cr_of(true, true, -1)), VERDICT_FAIL);
    TL_EXPECT_EQ(tl_child_verdict(true, cr_of(true, false, 0)), VERDICT_FAIL);
    TL_EXPECT_EQ(tl_child_verdict(true, cr_of(true, false, TL_EXIT_FAIL)), VERDICT_FAIL);
    TL_EXPECT_EQ(tl_child_verdict(true, cr_of(true, false, TL_EXIT_SKIP)), VERDICT_SKIP);
    TL_EXPECT_EQ(tl_child_verdict(true, cr_of(true, true, -1)), VERDICT_FAIL);
}

TL_TEST(runner_child_verdict_a_failed_spawn_is_never_a_pass, "runner,fast") {
    // The review-1 defect, pinned. A CreateProcess/fork failure used to set abnormal = true, and
    // a fatal-expected row read that as "it fataled, PASS" - so a broken exe path, a missing DLL
    // or an exhausted process table turned every fatal-expected test in the suite green while
    // running none of them.
    TL_EXPECT_EQ(tl_child_verdict(true, cr_of(false, true, -1)), VERDICT_FAIL);
    TL_EXPECT_EQ(tl_child_verdict(false, cr_of(false, true, -1)), VERDICT_FAIL);
    // ...and not even if the failed spawn somehow left a zero exit code behind.
    TL_EXPECT_EQ(tl_child_verdict(true, cr_of(false, false, 0)), VERDICT_FAIL);
    TL_EXPECT_EQ(tl_child_verdict(false, cr_of(false, false, 0)), VERDICT_FAIL);
}

// --- NEAR_FX -----------------------------------------------------------------------------------

TL_TEST(runner_near_fx_tolerance_is_raw_ulps, "runner,fx,fast") {
    const pos_t a = fx_raw<pos_t>(1000);
    TL_EXPECT_TRUE(tl_near_fx(a, fx_raw<pos_t>(1000), 0));        // exact, zero tolerance
    TL_EXPECT_TRUE(tl_near_fx(a, fx_raw<pos_t>(1005), 5));        // tolerance is inclusive
    TL_EXPECT_TRUE(tl_near_fx(a, fx_raw<pos_t>(995), 5));         // symmetric in the arguments
    TL_EXPECT_FALSE(tl_near_fx(a, fx_raw<pos_t>(1006), 5));
    TL_EXPECT_FALSE(tl_near_fx(a, fx_raw<pos_t>(994), 5));
    TL_EXPECT_FALSE(tl_near_fx(a, fx_raw<pos_t>(1001), 0));
    // The unit is raw ulps of the row, so it composes with det_math.h's measured bounds.
    const q_t g = fx_raw<q_t>(-500), w = fx_raw<q_t>(-500 + FX_ATAN2_MAX_ERR_ULP);
    TL_EXPECT_TRUE(tl_near_fx(g, w, FX_ATAN2_MAX_ERR_ULP));
    TL_EXPECT_FALSE(tl_near_fx(g, w, (i32)(FX_ATAN2_MAX_ERR_ULP - 1)));
}

TL_TEST(runner_near_fx_extremes_do_not_overflow, "runner,fx,fast") {
    // |INT32_MIN - INT32_MAX| is 2^32-1: computing it in the signed rep is overflow, which is UB
    // and a hard failure in the sanitizer lane (-fno-sanitize-recover=all, docs/CPP-SUBSET.md §5).
    // The answer must be "far apart", in both argument orders, not a wrapped small number.
    const pos_t lo = fx_raw<pos_t>(INT32_MIN);
    const pos_t hi = fx_raw<pos_t>(INT32_MAX);
    TL_EXPECT_FALSE(tl_near_fx(lo, hi, INT32_MAX));
    TL_EXPECT_FALSE(tl_near_fx(hi, lo, INT32_MAX));
    TL_EXPECT_FALSE(tl_near_fx(lo, fx_raw<pos_t>(0), INT32_MAX));   // 2^31 > INT32_MAX ulps
    TL_EXPECT_TRUE(tl_near_fx(hi, fx_raw<pos_t>(0), INT32_MAX));    // exactly INT32_MAX ulps
    TL_EXPECT_TRUE(tl_near_fx(lo, lo, 0));
    TL_EXPECT_TRUE(tl_near_fx(hi, hi, 0));
    // A negative tolerance is a call-site bug; it must reject, never sign-extend into "always".
    TL_EXPECT_FALSE(tl_near_fx(hi, hi, -1));
    TL_EXPECT_FALSE(tl_near_fx(lo, hi, -1));
}

// --- SPAN_EQ / MEM_EQ ---------------------------------------------------------------------------

TL_TEST(runner_span_eq_edges, "runner,fast") {
    const i32 a[4] = { 1, 2, 3, 4 };
    const i32 b[4] = { 1, 2, 3, 4 };
    const i32 c[4] = { 1, 2, 9, 4 };
    TL_EXPECT_TRUE(tl_span_eq(a, b, 4));
    TL_EXPECT_TRUE(tl_span_eq(a, a, 4));
    TL_EXPECT_FALSE(tl_span_eq(a, c, 4));
    TL_EXPECT_TRUE(tl_span_eq(a, c, 2));            // the difference is past the compared prefix
    TL_EXPECT_FALSE(tl_span_eq(a, c, 3));           // ...and inside it
    // n == 0 is true and reads nothing - including through null pointers.
    TL_EXPECT_TRUE(tl_span_eq(a, b, 0));
    TL_EXPECT_TRUE(tl_span_eq<i32>(nullptr, nullptr, 0));
    // A null with n > 0 is answered false, not dereferenced: a segfault reports as "abnormal
    // exit", which a fatal-expected row would have read as a PASS.
    TL_EXPECT_FALSE(tl_span_eq<i32>(nullptr, b, 4));
    TL_EXPECT_FALSE(tl_span_eq<i32>(a, nullptr, 4));
    TL_EXPECT_FALSE(tl_span_eq<i32>(nullptr, nullptr, 4));
    // It compares the ELEMENT type's ==, so an fx row compares on its representation.
    const pos_t p[2] = { fx_raw<pos_t>(7), fx_raw<pos_t>(-7) };
    const pos_t q[2] = { fx_raw<pos_t>(7), fx_raw<pos_t>(-7) };
    const pos_t r[2] = { fx_raw<pos_t>(7), fx_raw<pos_t>(-8) };
    TL_EXPECT_TRUE(tl_span_eq(p, q, 2));
    TL_EXPECT_FALSE(tl_span_eq(p, r, 2));
}

TL_TEST(runner_mem_eq_edges, "runner,fast") {
    struct Row { u32 a; u16 b; u16 _pad0; };
    Row x = { 1, 2, 0 };
    Row y = { 1, 2, 0 };
    Row z = { 1, 3, 0 };
    TL_EXPECT_TRUE(tl_mem_eq(&x, &y, sizeof(Row)));
    TL_EXPECT_FALSE(tl_mem_eq(&x, &z, sizeof(Row)));
    TL_EXPECT_TRUE(tl_mem_eq(&x, &z, sizeof(u32)));     // the difference is past the compared prefix
    TL_EXPECT_TRUE(tl_mem_eq(&x, &y, 0));
    TL_EXPECT_TRUE(tl_mem_eq(nullptr, nullptr, 0));     // legal, and not passed to memcmp
    TL_EXPECT_FALSE(tl_mem_eq(nullptr, &y, sizeof(Row)));
    TL_EXPECT_FALSE(tl_mem_eq(&x, nullptr, sizeof(Row)));
    // Explicit padding is what makes a byte comparison meaningful at all (docs/CPP-SUBSET.md §5).
    TL_EXPECT_EQ(sizeof(Row), (usize)8);
}

// --- TL_SKIP itself -----------------------------------------------------------------------------

TL_TEST(runner_skip_reports_skip_not_pass, "runner,fast") {
    // This test's own verdict is SKIP. It is here so that a real SKIP appears in every report,
    // in the TSV and in the JUnit XML - a status nobody ever sees is a status nobody trusts.
    // Its check budget is deliberately zero: TL_SKIP must not need one.
    TL_SKIP("a live SKIP row, so the status is exercised end to end in every run");
}

TL_TEST(runner_child_verdict_timeout_is_its_own_status, "runner,fast") {
    // docs/TESTING.md §9.1 (--timeout-ms, ruled 2026-08-24): a timed-out child is TIMEOUT, never
    // PASS, FAIL or SKIP - and the timeout beats every other signal, because the exit code of a
    // process the runner killed is the runner's own doing. The dangerous case is the last one: a
    // fatal-expected row whose body hangs instead of fatalling, killed with the fatal exit code,
    // must NOT read as a satisfied expectation.
    TL_EXPECT_EQ(tl_child_verdict(false, cr_timed_out(false, TL_EXIT_OK)),    VERDICT_TIMEOUT);
    TL_EXPECT_EQ(tl_child_verdict(false, cr_timed_out(true,  -1)),            VERDICT_TIMEOUT);
    TL_EXPECT_EQ(tl_child_verdict(false, cr_timed_out(false, TL_EXIT_SKIP)),  VERDICT_TIMEOUT);
    TL_EXPECT_EQ(tl_child_verdict(true, cr_timed_out(false, TL_EXIT_FATAL)), VERDICT_TIMEOUT);
    // A child that never spawned is still FAIL, even flagged as timed out: nothing ran.
    ChildResult never; never.spawned = false; never.abnormal = false; never.timed_out = true; never.exit_code = -1;
    TL_EXPECT_EQ(tl_child_verdict(true, never), VERDICT_FAIL);
    // And the flag is not sticky: the same shapes without it keep their old verdicts.
    TL_EXPECT_EQ(tl_child_verdict(false, cr_of(true, false, TL_EXIT_OK)),    VERDICT_PASS);
    TL_EXPECT_EQ(tl_child_verdict(true, cr_of(true, false, TL_EXIT_FATAL)), VERDICT_PASS);
}
