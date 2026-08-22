#pragma once
// ---------------------------------------------------------------------------------------------
// tl_test.h - the test runner's public surface.
//
// Spec: docs/TESTING.md §1, §9.1. W0 ships the *stub*: TL_TEST, the scalar/pointer assertion
//   macros, tag/filter selection, --isolate, TSV + JUnit reports. The W1 runner+driver lane
//   completes it (NEAR_FX, SPAN_EQ, MEM_EQ, TL_TEST_EXPECT_FATAL, TL_ASSERT_NO_ALLOC,
//   TL_ASSERT_DETERMINISTIC, property generators) against docs/TESTING.md §9.
// Invariants: no static registration - cmake/testlist.cmake generates test_list.inc from the
//   TL_TEST( occurrences in tests/**/*.test.cpp, so discovery order is a pure function of the
//   tree. Tests obey the C++ subset with the two exemptions of docs/TESTING.md §8 R-2 (the
//   generated list, and printf-class io/clock/filesystem).
// Determinism: EXPECT_* record and continue; ASSERT_* record and return - so ASSERT_* is usable
//   only at test-function top level, helpers return bool (docs/TESTING.md §9.1).
// Threading: --isolate runs one test per child process; a test body is single-threaded.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

struct TestCtx {
    const char* name;
    u32 failures;        // hard (ASSERT) + soft (EXPECT) failures recorded so far
    u32 soft_failures;
    u32 checks;
};

// One test. `tags` is a comma-separated literal list; --tag matches one tag, --tag !t excludes.
// The body is a free function `test_<name>`; the generated list is the only registration.
#define TL_TEST(name, tags) void test_##name(TestCtx* t)

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

struct TestInfo {
    const char* name;
    const char* tags;
    void (*fn)(TestCtx*);
    const char* file;
};
