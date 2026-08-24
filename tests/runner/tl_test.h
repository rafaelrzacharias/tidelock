#pragma once
// ---------------------------------------------------------------------------------------------
// tl_test.h - the test runner's public surface.
//
// Spec: docs/TESTING.md §1, §9.1. Completed by the W1 runner+driver lane: NEAR_FX, SPAN_EQ,
//   MEM_EQ, TL_TEST_EXPECT_FATAL, TL_ASSERT_NO_ALLOC, TL_ASSERT_DETERMINISTIC, property
//   generators, against docs/TESTING.md §9.
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
// Threading: --isolate runs one test per child process; a test body is single-threaded.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

#include <string.h>   // memcmp - the sanctioned MEM_EQ primitive (docs/CPP-SUBSET.md §1)

struct TestCtx {
    const char* name;
    u32 failures;        // hard (ASSERT) + soft (EXPECT) failures recorded so far
    u32 soft_failures;
    u32 checks;
    u32 seed;            // property tests: --seed, or the test's row index if none was given
};

// One test. `tags` is a comma-separated literal list; --tag matches one tag, --tag !t excludes.
// The body is a free function `test_<name>`; the generated list is the only registration.
#define TL_TEST(name, tags) void test_##name(TestCtx* t)

// A test whose body is expected to hit TL_ASSERT/TL_CHECK/TL_FATAL (docs/TESTING.md §9.1). Same
// signature as TL_TEST; `tools/testlist.cmake` (sic, cmake/testlist.cmake) tags the generated
// TestInfo row `expect_fatal = 1` so the runner always re-executes it in a child process (even
// without --isolate) and inverts the pass condition: the child terminating abnormally is PASS,
// a clean exit 0 is FAIL (the assert that was supposed to fire did not).
//
// KNOWN GAP (TODO.md "fx tests that need the runner lane"): the contract is child exit code 2 +
// a stderr marker (TL_FATAL_MARKER below), but tl_fatal is currently the tooling-rt lane's trap
// stub (src/foundation/tl_assert.cpp: __builtin_trap(), no message, no controlled exit code) -
// __builtin_trap() surfaces as an OS-abnormal exit (e.g. STATUS_ILLEGAL_INSTRUCTION on Windows),
// not exit 2. Until tooling-rt lands the crash writer, the runner matches on "child exited
// abnormally" (any non-zero, non-EXIT_FAIL-shaped termination) instead of the exit-2+marker
// contract; the day tl_fatal writes the marker and returns 2, tighten tl_test_child_is_fatal in
// main.cpp to check both and delete this note.
#define TL_TEST_EXPECT_FATAL(name, tags) void test_##name(TestCtx* t)

// The stderr line the exit-2+marker contract expects a real tl_fatal to print before aborting
// (docs/TESTING.md §9.1). Not emitted by anything today (see the gap note above) - named here so
// the tooling-rt lane's crash writer and this runner agree on one spelling without a second doc.
#define TL_FATAL_MARKER "TL_FATAL:"

// Records a failure at file:line with the failing expression. Defined in tests/runner/main.cpp.
void tl_test_fail(TestCtx* t, const char* file, u32 line, const char* expr, bool soft);

#define TL_EXPECT_TRUE(c)  do { ++t->checks; if (!(c)) { tl_test_fail(t, __FILE__, (u32)__LINE__, #c, true); } } while (0)
#define TL_ASSERT_TRUE(c)  do { ++t->checks; if (!(c)) { tl_test_fail(t, __FILE__, (u32)__LINE__, #c, false); return; } } while (0)
#define TL_EXPECT_FALSE(c) TL_EXPECT_TRUE(!(c))
#define TL_ASSERT_FALSE(c) TL_ASSERT_TRUE(!(c))
#define TL_EXPECT_EQ(a, b) TL_EXPECT_TRUE((a) == (b))
#define TL_ASSERT_EQ(a, b) TL_ASSERT_TRUE((a) == (b))
#define TL_EXPECT_NE(a, b) TL_EXPECT_TRUE((a) != (b))
#define TL_EXPECT_LT(a, b) TL_EXPECT_TRUE((a) <  (b))
#define TL_EXPECT_LE(a, b) TL_EXPECT_TRUE((a) <= (b))
#define TL_EXPECT_GT(a, b) TL_EXPECT_TRUE((a) >  (b))
#define TL_EXPECT_GE(a, b) TL_EXPECT_TRUE((a) >= (b))
#define TL_EXPECT_NULL(p)     TL_EXPECT_TRUE((p) == nullptr)
#define TL_EXPECT_NOT_NULL(p) TL_EXPECT_TRUE((p) != nullptr)
#define TL_EXPECT_IN_RANGE(v, lo, hi) TL_EXPECT_TRUE((v) >= (lo) && (v) <= (hi))

// NEAR_FX<R>(a, b, tol): a, b are one fx<Rep,F> row; tol is in raw ulps of that row. Never raw
// float equality anywhere (docs/TESTING.md §1) - this is the one sanctioned "close enough" over
// fx rows, always compared as integers on the representation.
template <typename R>
inline bool tl_near_fx(R a, R b, typename R::rep tol) {
    const typename R::rep d = a.v > b.v ? typename R::rep(a.v - b.v) : typename R::rep(b.v - a.v);
    return d <= tol;
}
#define TL_EXPECT_NEAR_FX(a, b, tol) TL_EXPECT_TRUE(tl_near_fx((a), (b), (tol)))
#define TL_ASSERT_NEAR_FX(a, b, tol) TL_ASSERT_TRUE(tl_near_fx((a), (b), (tol)))

// SPAN_EQ(a, b, n): elementwise `==` over n elements of two pointers of the same element type
// (containers rubric: two-instance determinism, docs/TESTING.md §7).
template <typename T>
inline bool tl_span_eq(const T* a, const T* b, usize n) {
    for (usize i = 0; i < n; ++i) { if (!(a[i] == b[i])) { return false; } }
    return true;
}
#define TL_EXPECT_SPAN_EQ(a, b, n) TL_EXPECT_TRUE(tl_span_eq((a), (b), (usize)(n)))
#define TL_ASSERT_SPAN_EQ(a, b, n) TL_ASSERT_TRUE(tl_span_eq((a), (b), (usize)(n)))

// MEM_EQ(a, b, bytes): raw byte comparison (memcmp is sanctioned, docs/CPP-SUBSET.md §1) - for
// POD structs and snapshot/golden byte-stability checks (docs/TESTING.md §7.10).
inline bool tl_mem_eq(const void* a, const void* b, usize bytes) { return memcmp(a, b, bytes) == 0; }
#define TL_EXPECT_MEM_EQ(a, b, bytes) TL_EXPECT_TRUE(tl_mem_eq((a), (b), (usize)(bytes)))
#define TL_ASSERT_MEM_EQ(a, b, bytes) TL_ASSERT_TRUE(tl_mem_eq((a), (b), (usize)(bytes)))

// TL_ASSERT_NO_ALLOC(arena, stmt) - the zero-alloc guard: an arena mark pair (docs/MEMORY.md §2
// `arena_mark`/`arena_reset_to`) around `stmt`, plus a read of the CRT-malloc counting shim
// (docs/MEMORY.md §8.1 `alloc_shim.cpp`). STUB (TODO.md): neither `VMemArena` nor `alloc_shim`
// has landed (mem lane, W1, checked 2026-08-24 - w1-mem carries no commits past main), so there
// is nothing to mark or count yet. The macro runs `stmt` and asserts nothing; it exists so call
// sites elsewhere write against the eventual signature today. Do not add a test that reports
// this macro as having verified zero allocation - it has not. Wire the real mark pair + counter
// read the day `foundation/vmem_arena.h` and `alloc_shim.cpp` exist, then delete this note.
#define TL_ASSERT_NO_ALLOC(arena, stmt) do { (void)(arena); stmt; } while (0)

// TL_ASSERT_DETERMINISTIC(setup_fn, ticks) - the run-twice harness (docs/TESTING.md §9.1,
// docs/DETERMINISM.md §6): build two worlds via setup_fn, run `ticks` ticks on each, compare
// per-arena hashes every tick. STUB (TODO.md): `World`/ECS have not landed. The macro evaluates
// its arguments (so callers type-check against the eventual signature) and asserts nothing. Do
// not add a test that reports this macro as having verified determinism - it has not. Wire
// `harness_dual_sim` the day a `World` exists, then delete this note.
#define TL_ASSERT_DETERMINISTIC(setup_fn, ticks) do { (void)(setup_fn); (void)(ticks); } while (0)

struct TestInfo {
    const char* name;
    const char* tags;
    void (*fn)(TestCtx*);
    const char* file;
    u8 expect_fatal;
};
