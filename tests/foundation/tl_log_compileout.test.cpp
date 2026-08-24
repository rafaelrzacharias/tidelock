// tl_log_compileout.test.cpp - the half of docs/TOOLING.md §9.5 "log_levels_and_compile_out" that
// tl_log.test.cpp cannot reach: "a TL_LOG_TRACE call site under TL_LOG_MIN=2 leaves no symbol and
// does not evaluate its argument". tl_log.test.cpp mirrors the header's #if, so in a tier whose
// TL_LOG_MIN is 0 its #else arms are dead code and the compiled-out branch is never exercised at
// all - and until this lane's review, TL_LOG_MIN was undefined in EVERY tier, so it never was.
// This TU pins TL_LOG_MIN itself, before the header, so the barred branch is exercised in every
// tier the suite is built for. tests/ is exempt from the io ban (docs/TESTING.md §8 R-2).
#define TL_LOG_MIN 4          // above LOG_WARN: TRACE..WARN are all compiled out here
#include "foundation/tl_log.h"
#include "runner/tl_test.h"

// A function, not `++n`: a call the preprocessor did not delete would leave a call to this in the
// object file, so the counter proves argument evaluation and not merely a value.
static int g_side_effects = 0;
static int bump(void) { g_side_effects += 1; return g_side_effects; }

TL_TEST(log_compiled_out_levels_do_not_evaluate_arguments, "foundation") {
    g_side_effects = 0;
    TL_LOG_TRACE("%d", bump());
    TL_LOG_DEBUG("%d", bump());
    TL_LOG_INFO("%d", bump());
    TL_LOG_WARN("%d", bump());
    TL_EXPECT_EQ(g_side_effects, 0);   // four compiled-out call sites, zero evaluations
    TL_EXPECT_EQ(TL_LOG_MIN, 4);       // the override actually took (the header is #ifndef-guarded)

    // TL_LOG_ERR is never compiled out, at any TL_LOG_MIN.
    TL_LOG_ERR("%d", bump());
    TL_EXPECT_EQ(g_side_effects, 1);
}
