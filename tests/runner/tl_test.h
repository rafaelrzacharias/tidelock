#pragma once
// ---------------------------------------------------------------------------------------------
// tl_test.h - the test runner's public surface.
//
// Spec: docs/TESTING.md §1, §9.1. Completed by the W1 runner+driver lane: NEAR_FX, SPAN_EQ,
//   MEM_EQ, TL_TEST_EXPECT_FATAL, property seeding, against docs/TESTING.md §9.
// Invariants: no static registration - cmake/testlist.cmake generates test_list.inc from the
//   TL_TEST(/TL_TEST_EXPECT_FATAL( occurrences in tests/**/*.test.cpp, so discovery order is a
//   pure function of the tree. Tests obey the C++ subset with the two exemptions of
//   docs/TESTING.md §8 R-2 (the generated list, and printf-class io/clock/filesystem access -
//   read as covering the process-spawn primitives the isolate pool needs, since the feature
//   TESTING.md §9.1 specifies cannot exist without them; recorded in TODO.md).
// Determinism: EXPECT_* record and continue; ASSERT_* record and return - so ASSERT_* is usable
//   only at test-function top level, helpers return bool (docs/TESTING.md §9.1). A
//   TL_TEST_EXPECT_FATAL body always runs in a child process (docs/TESTING.md §9.1), never
//   in-process, so a real trap cannot take down the parent.
// Vacuity: a test that records ZERO checks is reported FAIL, not PASS (runner_core.h
//   tl_ctx_verdict) - the sanctioned way to run no checks is TL_SKIP, which reports SKIP. The
//   two macros this header cannot yet implement honestly (TL_ASSERT_NO_ALLOC,
//   TL_ASSERT_DETERMINISTIC) refuse to COMPILE rather than pass; see below.
// Threading: --isolate runs one test per child process; a test body is single-threaded.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "runner/runner_core.h"

#include <string.h>   // memcmp - the sanctioned MEM_EQ primitive (docs/CPP-SUBSET.md §1)

struct TestCtx {
    const char* name;
    u32 failures;        // hard (ASSERT) + soft (EXPECT) failures recorded so far
    u32 soft_failures;
    u32 checks;          // read by tl_ctx_verdict: zero checks is a FAIL, never a silent PASS
    u32 seed;            // property tests: tl_seed_for(--seed, row index); identical in a child
    u8  skipped;         // set by TL_SKIP
    const char* skip_reason;
};

// One test. `tags` is a comma-separated tag list, spelled either as one literal ("a,b") or as
// separate literals ("a", "b") - docs/TESTING.md §1 uses the second form, §9.1 spells the macro
// variadic, and cmake/testlist.cmake already accepts both, so the macro is variadic too (the
// fixed two-parameter form rejected §1's own example - W1 runner review 1).
// The body is a free function `test_<name>`; the generated list is the only registration.
#define TL_TEST(name, ...) void test_##name(TestCtx* t)

// A test whose body is expected to hit TL_ASSERT/TL_CHECK/TL_FATAL (docs/TESTING.md §9.1). Same
// signature as TL_TEST; cmake/testlist.cmake tags the generated TestInfo row `expect_fatal = 1`
// so the runner always re-executes it in a child process (even without --isolate) and inverts
// the pass condition: on a tier where TL_ASSERT is compiled in, the child terminating abnormally
// is PASS and a clean exit is FAIL (the assert that was supposed to fire did not).
//
// KNOWN GAP, half closed at the W1 wave merge (TODO.md, "the TL_TEST_EXPECT_FATAL
// tightening"): the contract is child exit code 2 plus a TL_FATAL_MARKER stderr line. The real
// tl_fatal is linked now, so the runner matches exit code 2 exactly (an abnormal exit is an
// uncontrolled crash and FAILS). The marker + file:line half still needs child-stderr capture;
// TODO.md carries the remaining condition.
#define TL_TEST_EXPECT_FATAL(name, ...) void test_##name(TestCtx* t)

// The stderr prefix a real tl_fatal prints before aborting. This is the LITERAL the tooling-rt
// lane's crash writer emits - fprintf(stderr, "TL_FATAL origin=%s %s:%u: %s") in
// src/foundation/crash.cpp, where the "TL_FATAL" prefix is fixed regardless of whether the
// origin was TL_FATAL, TL_CHECK or TL_ASSERT, precisely so these tests can grep for one string.
// It was spelled "TL_FATAL:" here until W1 runner review 1 compared the two lanes: the colon
// made the two halves of a contract documented as shared disagree, and the "mechanical"
// tightening would have compiled and then matched nothing.
#define TL_FATAL_MARKER "TL_FATAL origin="

// Records a failure at file:line with the failing expression. Defined in tests/runner/main.cpp.
void tl_test_fail(TestCtx* t, const char* file, u32 line, const char* expr, bool soft);

// Declares this test not applicable on this tier/configuration and returns. Reported as SKIP -
// a visible status in the TSV, the JUnit XML and the stdout summary, never counted as a pass.
// Like ASSERT_*, it returns, so it is usable only at test-function top level.
#define TL_SKIP(reason) do { t->skipped = 1; t->skip_reason = (reason); return; } while (0)

#define TL_EXPECT_TRUE(c)  do { ++t->checks; if (!(c)) { tl_test_fail(t, __FILE__, (u32)__LINE__, #c, true); } } while (0)
#define TL_ASSERT_TRUE(c)  do { ++t->checks; if (!(c)) { tl_test_fail(t, __FILE__, (u32)__LINE__, #c, false); return; } } while (0)
#define TL_EXPECT_FALSE(c) TL_EXPECT_TRUE(!(c))
#define TL_ASSERT_FALSE(c) TL_ASSERT_TRUE(!(c))
#define TL_EXPECT_EQ(a, b) TL_EXPECT_TRUE((a) == (b))
#define TL_ASSERT_EQ(a, b) TL_ASSERT_TRUE((a) == (b))
#define TL_EXPECT_NE(a, b) TL_EXPECT_TRUE((a) != (b))
#define TL_ASSERT_NE(a, b) TL_ASSERT_TRUE((a) != (b))
#define TL_EXPECT_LT(a, b) TL_EXPECT_TRUE((a) <  (b))
#define TL_ASSERT_LT(a, b) TL_ASSERT_TRUE((a) <  (b))
#define TL_EXPECT_LE(a, b) TL_EXPECT_TRUE((a) <= (b))
#define TL_ASSERT_LE(a, b) TL_ASSERT_TRUE((a) <= (b))
#define TL_EXPECT_GT(a, b) TL_EXPECT_TRUE((a) >  (b))
#define TL_ASSERT_GT(a, b) TL_ASSERT_TRUE((a) >  (b))
#define TL_EXPECT_GE(a, b) TL_EXPECT_TRUE((a) >= (b))
#define TL_ASSERT_GE(a, b) TL_ASSERT_TRUE((a) >= (b))
#define TL_EXPECT_NULL(p)     TL_EXPECT_TRUE((p) == nullptr)
#define TL_ASSERT_NULL(p)     TL_ASSERT_TRUE((p) == nullptr)
#define TL_EXPECT_NOT_NULL(p) TL_EXPECT_TRUE((p) != nullptr)
#define TL_ASSERT_NOT_NULL(p) TL_ASSERT_TRUE((p) != nullptr)
#define TL_EXPECT_IN_RANGE(v, lo, hi) TL_EXPECT_TRUE((v) >= (lo) && (v) <= (hi))
#define TL_ASSERT_IN_RANGE(v, lo, hi) TL_ASSERT_TRUE((v) >= (lo) && (v) <= (hi))

// NEAR_FX<R>(a, b, tol): a, b are one fx<Rep,F> row; tol is in raw ulps of that row, the same
// unit as det_math.h's FX_*_MAX_ERR_ULP constants, so a test spells
// TL_EXPECT_NEAR_FX(got, want, FX_ATAN2_MAX_ERR_ULP). Never raw float equality anywhere
// (docs/TESTING.md §1) - this is the one sanctioned "close enough" over fx rows, always compared
// as integers on the representation.
//
// The difference is taken in the UNSIGNED domain and only then compared: |INT32_MIN - INT32_MAX|
// is 2^32-1, which does not fit a 32-bit signed rep, and computing it there is signed overflow -
// UB, and a hard failure in the sanitizer lane, which builds -fno-sanitize-recover=all
// (docs/CPP-SUBSET.md §5). A negative tol is rejected rather than sign-extended into a huge
// unsigned bound.
template <typename R>
inline bool tl_near_fx(R a, R b, typename R::rep tol) {
    if (tol < 0) { return false; }
    const i64 av = (i64)a.v;
    const i64 bv = (i64)b.v;
    const u64 d = av >= bv ? (u64)av - (u64)bv : (u64)bv - (u64)av;
    return d <= (u64)(i64)tol;
}
#define TL_EXPECT_NEAR_FX(a, b, tol) TL_EXPECT_TRUE(tl_near_fx((a), (b), (tol)))
#define TL_ASSERT_NEAR_FX(a, b, tol) TL_ASSERT_TRUE(tl_near_fx((a), (b), (tol)))

// SPAN_EQ(a, b, n): elementwise == over n elements of two pointers of the same element type
// (containers rubric: two-instance determinism, docs/TESTING.md §7). n == 0 is TRUE and reads
// nothing, so a null/null/0 pair is legal; a null pointer with n > 0 is a bug at the call site
// and is answered false here rather than dereferenced, because a segfault reports as "abnormal
// exit", which for a fatal-expected test would read as a PASS.
// Length mismatch is the caller's to state: two spans of different lengths are unequal by
// definition, so compare the lengths with TL_EXPECT_EQ first, then the elements.
template <typename T>
inline bool tl_span_eq(const T* a, const T* b, usize n) {
    if (n == 0) { return true; }
    if (a == nullptr || b == nullptr) { return false; }
    for (usize i = 0; i < n; ++i) { if (!(a[i] == b[i])) { return false; } }
    return true;
}
#define TL_EXPECT_SPAN_EQ(a, b, n) TL_EXPECT_TRUE(tl_span_eq((a), (b), (usize)(n)))
#define TL_ASSERT_SPAN_EQ(a, b, n) TL_ASSERT_TRUE(tl_span_eq((a), (b), (usize)(n)))

// MEM_EQ(a, b, bytes): raw byte comparison (memcmp is sanctioned, docs/CPP-SUBSET.md §1) - for
// POD structs and snapshot/golden byte-stability checks (docs/TESTING.md §7.10). Same null rule
// as SPAN_EQ, and for the same reason; memcmp(nullptr, nullptr, 0) is also UB by the letter of
// the standard even though every implementation returns 0.
inline bool tl_mem_eq(const void* a, const void* b, usize bytes) {
    if (bytes == 0) { return true; }
    if (a == nullptr || b == nullptr) { return false; }
    return memcmp(a, b, bytes) == 0;
}
#define TL_EXPECT_MEM_EQ(a, b, bytes) TL_EXPECT_TRUE(tl_mem_eq((a), (b), (usize)(bytes)))
#define TL_ASSERT_MEM_EQ(a, b, bytes) TL_ASSERT_TRUE(tl_mem_eq((a), (b), (usize)(bytes)))

// TL_ASSERT_NO_ALLOC(arena, stmt) and TL_ASSERT_DETERMINISTIC(setup_fn, ticks) - docs/TESTING.md
// §1's zero-alloc guard and run-twice harness. NEITHER CAN BE IMPLEMENTED IN THIS TREE: the
// first needs VMemArena's mark pair (docs/MEMORY.md §2) and the CRT counting shim
// (docs/MEMORY.md §8.1 alloc_shim.cpp), the second needs a World; neither exists (mem lane and
// the ECS milestone, TODO.md).
//
// They therefore refuse to COMPILE. They shipped as no-op stubs that ran the statement and
// asserted nothing - a macro spelled ASSERT that always passes is a disarmed tripwire, and the
// W0 lesson is that a gate which cannot fail is worse than no gate: the first zero-alloc test
// written against one would have reported PASS and been believed. A doc comment saying "do not
// trust this" is not a gate. Refusing to compile costs nothing today (neither macro has a call
// site anywhere in the tree, checked 2026-08-24) and cannot be mistaken for a green run.
// Replace each body with the real check the day its dependency lands - TODO.md carries both.
//
// The dependent-false static_assert keeps the diagnostic attached to the expansion point rather
// than firing when this header is merely included.
template <typename T> struct tl_never_true { static constexpr bool value = false; };
#define TL_ASSERT_NO_ALLOC(arena, stmt) \
    static_assert(tl_never_true<decltype(t)>::value, \
        "TL_ASSERT_NO_ALLOC is not implemented: VMemArena and alloc_shim.cpp have not landed (TODO.md). It refuses to compile rather than report PASS having checked nothing.")
#define TL_ASSERT_DETERMINISTIC(setup_fn, ticks) \
    static_assert(tl_never_true<decltype(t)>::value, \
        "TL_ASSERT_DETERMINISTIC is not implemented: no World exists yet (TODO.md). It refuses to compile rather than report PASS having checked nothing.")

struct TestInfo {
    const char* name;
    const char* tags;
    void (*fn)(TestCtx*);
    const char* file;
    u8 expect_fatal;
};
