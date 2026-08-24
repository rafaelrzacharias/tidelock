// scratch.h - scope nesting, reset, poison, and the not-zero-on-reuse contract.
// Spec: docs/MEMORY.md §1.3, §8.1. Rubric: docs/TESTING.md §7.
// Deferred to the runner lane's TL_TEST_EXPECT_FATAL: scope overflow (SCRATCH_MAX_SCOPES),
// scope underflow, reset with open scopes - all dev asserts.
#include "runner/tl_test.h"
#include "foundation/scratch.h"
#include "vmem_test_api.h"

#include <string.h>

TL_TEST(scratch_scopes_nest_and_roll_back, "foundation,mem,smoke,fast") {
    VMemApi api = test_vmem_api();
    Scratch s = {};
    TL_ASSERT_EQ(scratch_init(&s, 0xACACu, 4u << 20, &api), ERR_OK);
    TL_EXPECT_EQ(s.depth, 0u);

    (void)scratch_push(&s, 64u, 16u);
    const u64 outer = arena_mark(&s.a);

    TL_SCRATCH_SCOPE_BEGIN(&s);
    (void)scratch_push(&s, 1000u, 16u);
    TL_SCRATCH_SCOPE_BEGIN(&s);                    // nested scope
    (void)scratch_push(&s, 2000u, 16u);
    TL_EXPECT_EQ(s.depth, 2u);
    TL_SCRATCH_SCOPE_END(&s);
    TL_EXPECT_EQ(s.depth, 1u);
    const u64 after_inner = arena_mark(&s.a);
    TL_EXPECT_GE(after_inner, outer + 1000u);      // inner rolled back, outer's push kept
    TL_EXPECT_LT(after_inner, outer + 3000u);
    TL_SCRATCH_SCOPE_END(&s);
    TL_EXPECT_EQ(arena_mark(&s.a), outer);         // back to the outer mark exactly
    TL_EXPECT_EQ(s.depth, 0u);

    scratch_reset(&s);
    TL_EXPECT_EQ(arena_mark(&s.a), (u64)0);

    api.release(api.ctx, s.a.base, s.a.reserved);
}

TL_TEST(scratch_reuse_is_not_zero_and_poisons_in_dev, "foundation,mem,fast") {
    VMemApi api = test_vmem_api();
    Scratch s = {};
    TL_ASSERT_EQ(scratch_init(&s, 0xBDBDu, 1u << 20, &api), ERR_OK);

    u8* p = (u8*)scratch_push(&s, 256u, 16u);
    memset(p, 0x77, 256u);
    scratch_reset(&s);
    u8* q = (u8*)scratch_push(&s, 256u, 16u);
    TL_EXPECT_TRUE(q == p);
#if TL_DEV
    // ARENA_POISON (set by scratch_init in dev): a stale read shows as 0xDD garbage, never as
    // the old data and never silently as zero (docs/MEMORY.md section 1.3).
    u32 not_poisoned = 0;
    for (u32 i = 0; i < 256u; ++i) { not_poisoned += (q[i] != 0xDDu) ? 1u : 0u; }
    TL_EXPECT_EQ(not_poisoned, 0u);
#else
    // Release: contents are unspecified; the contract is only "do not assume zero". Touch one
    // byte to keep the path exercised.
    q[0] = 1u;
    TL_EXPECT_EQ(q[0], (u8)1);
#endif

    api.release(api.ctx, s.a.base, s.a.reserved);
}

TL_TEST(scratch_zero_and_empty_edges, "foundation,mem,fast") {
    VMemApi api = test_vmem_api();
    Scratch s = {};
    TL_ASSERT_EQ(scratch_init(&s, 0xCECEu, 1u << 20, &api), ERR_OK);

    // Empty scope pair: a no-op, depth balanced.
    TL_SCRATCH_SCOPE_BEGIN(&s);
    TL_SCRATCH_SCOPE_END(&s);
    TL_EXPECT_EQ(s.depth, 0u);
    TL_EXPECT_EQ(arena_mark(&s.a), (u64)0);

    // Reset on an empty scratch: legal no-op.
    scratch_reset(&s);
    TL_EXPECT_EQ(arena_mark(&s.a), (u64)0);

    // Max-depth nesting works (overflow beyond it is a deferred fatal test).
    for (u32 i = 0; i < SCRATCH_MAX_SCOPES; ++i) { TL_SCRATCH_SCOPE_BEGIN(&s); }
    TL_EXPECT_EQ(s.depth, SCRATCH_MAX_SCOPES);
    for (u32 i = 0; i < SCRATCH_MAX_SCOPES; ++i) { TL_SCRATCH_SCOPE_END(&s); }
    TL_EXPECT_EQ(s.depth, 0u);

    api.release(api.ctx, s.a.base, s.a.reserved);
}
