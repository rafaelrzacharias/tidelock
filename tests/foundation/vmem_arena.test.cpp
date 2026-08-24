// vmem_arena.h - push/align/commit growth, reset poison, decommit-then-repush-is-zero, the
// high_water re-zero rule, the alignment-gap re-zero rule.
// Spec: docs/MEMORY.md §8.2, §8.7 test list. Rubric: docs/TESTING.md §7.
// Deferred to the runner lane's TL_TEST_EXPECT_FATAL (docs/TESTING.md §9.1): over-reserve push,
// wrapped-extent push, bad align, reset mark > used - each is a dev assert with no release
// value to pin (the arena has no error path there by design).
#include "runner/tl_test.h"
#include "foundation/vmem_arena.h"
#include "vmem_test_api.h"

#include <string.h>

TL_TEST(vmem_init_happy_and_errors, "foundation,mem,smoke,fast") {
    VMemApi api = test_vmem_api();
    TL_ASSERT_TRUE(api.page_size != 0u && (api.page_size & (api.page_size - 1u)) == 0u);

    VMemArena a = {};
    TL_ASSERT_EQ(vmem_arena_init(&a, 0x1111u, 1u << 20, 0u, &api), ERR_OK);
    TL_EXPECT_NOT_NULL(a.base);
    TL_EXPECT_EQ(a.reserved, (u64)(1u << 20));      // 1 MB is already page aligned
    TL_EXPECT_EQ(a.committed, (u64)0);
    TL_EXPECT_EQ(a.used, (u64)0);
    TL_EXPECT_EQ(a.high_water, (u64)0);
    TL_EXPECT_EQ(a.id, (NameHash)0x1111u);
    api.release(api.ctx, a.base, a.reserved);

    // reserve_bytes rounds up to a page.
    VMemArena b = {};
    TL_ASSERT_EQ(vmem_arena_init(&b, 0x2222u, 100u, 0u, &api), ERR_OK);
    TL_EXPECT_EQ(b.reserved, (u64)api.page_size);
    api.release(api.ctx, b.base, b.reserved);

    // Error paths: state must stay untouched (no partial init).
    VMemArena c = {};
    TL_EXPECT_EQ(vmem_arena_init(&c, 1u, 0u, 0u, &api), ERR_MEM_BAD_ARG);   // zero reserve
    TL_EXPECT_EQ(vmem_arena_init(&c, 1u, 4096u, 0u, nullptr), ERR_MEM_BAD_ARG);
    TL_EXPECT_EQ(vmem_arena_init(nullptr, 1u, 4096u, 0u, &api), ERR_MEM_BAD_ARG);
    TL_EXPECT_NULL(c.base);
}

TL_TEST(vmem_push_align_and_commit_growth, "foundation,mem,smoke,fast") {
    VMemApi api = test_vmem_api();
    VMemArena a = {};
    TL_ASSERT_EQ(vmem_arena_init(&a, 0x3333u, 16u << 20, 0u, &api), ERR_OK);

    // Alignment matrix: 1/8/64/page; each pointer aligned, used advances monotonically.
    u8* p1 = (u8*)arena_push(&a, 3u, 1u);
    TL_ASSERT_TRUE(p1 != nullptr);
    TL_EXPECT_EQ(a.used, (u64)3);
    u8* p8 = (u8*)arena_push(&a, 8u, 8u);
    TL_EXPECT_EQ(((u64)(p8 - a.base)) & 7u, (u64)0);
    TL_EXPECT_EQ((u64)(p8 - a.base), (u64)8);        // gap 3..8 skipped
    u8* p64 = (u8*)arena_push(&a, 1u, 64u);
    TL_EXPECT_EQ(((u64)(p64 - a.base)) & 63u, (u64)0);
    u8* pp = (u8*)arena_push(&a, 1u, a.page);
    TL_EXPECT_EQ(((u64)(pp - a.base)) % a.page, (u64)0);

    // Commit growth: in COMMIT_GRANULE multiples, never past reserve, always >= used.
    TL_EXPECT_EQ(a.committed % (u64)COMMIT_GRANULE, (u64)0);
    TL_EXPECT_GE(a.committed, a.used);
    (void)arena_push(&a, (u64)COMMIT_GRANULE * 3u + 5u, 16u);
    TL_EXPECT_EQ(a.committed % (u64)COMMIT_GRANULE, (u64)0);
    TL_EXPECT_GE(a.committed, a.used);
    TL_EXPECT_LE(a.committed, a.reserved);

    // Fresh memory is zero (OS pages), even mid-granule.
    u8* z = (u8*)arena_push(&a, 4096u, 16u);
    u32 nonzero = 0;
    for (u32 i = 0; i < 4096u; ++i) { nonzero += (z[i] != 0u) ? 1u : 0u; }
    TL_EXPECT_EQ(nonzero, 0u);

    // Zero-byte push: legal, returns an aligned pointer, moves nothing.
    const u64 before = a.used;
    (void)arena_push(&a, 0u, 16u);
    TL_EXPECT_EQ(a.used, ((before + 15u) & ~(u64)15u));

    api.release(api.ctx, a.base, a.reserved);
}

TL_TEST(vmem_zero_on_push_covers_block_and_gap, "foundation,mem,smoke,fast") {
    VMemApi api = test_vmem_api();
    VMemArena a = {};
    TL_ASSERT_EQ(vmem_arena_init(&a, 0x4444u, 1u << 20, ARENA_ZERO_ON_PUSH, &api), ERR_OK);

    // Dirty a range, roll back, and re-push: every byte inside [base, used) must read zero -
    // hashed memory is a pure function of state (docs/CPP-SUBSET.md §5).
    u8* p = (u8*)arena_push(&a, 300u, 1u);
    memset(p, 0xAB, 300u);
    arena_reset_to(&a, 0u);

    // Re-push with an alignment gap: 5 unaligned bytes then a 64-aligned block. The gap bytes
    // [5, 64) are inside used and must be zero too, not just the block.
    u8* q5 = (u8*)arena_push(&a, 5u, 1u);
    memset(q5, 0x5A, 5u);                    // caller-owned bytes may be anything
    u8* q = (u8*)arena_push(&a, 100u, 64u);
    TL_EXPECT_EQ((u64)(q - a.base), (u64)64);
    u32 dirty = 0;
    for (u32 i = 5; i < 64u; ++i) { dirty += (a.base[i] != 0u) ? 1u : 0u; }   // the gap
    for (u32 i = 0; i < 100u; ++i) { dirty += (q[i] != 0u) ? 1u : 0u; }       // the block
    TL_EXPECT_EQ(dirty, 0u);

    // high_water rule: above it nothing is re-zeroed because it is OS-zero already.
    TL_EXPECT_EQ(a.high_water, (u64)300);   // the first push still owns the high-water mark
    u8* r = (u8*)arena_push(&a, 400u, 1u);  // crosses high_water: [164,300) dirty, [300,564) fresh
    u32 bad = 0;
    for (u32 i = 0; i < 400u; ++i) { bad += (r[i] != 0u) ? 1u : 0u; }
    TL_EXPECT_EQ(bad, 0u);

    api.release(api.ctx, a.base, a.reserved);
}

TL_TEST(vmem_reset_poison_dev, "foundation,mem,fast") {
#if TL_DEV
    VMemApi api = test_vmem_api();
    VMemArena a = {};
    TL_ASSERT_EQ(vmem_arena_init(&a, 0x5555u, 1u << 20, ARENA_POISON, &api), ERR_OK);
    u8* p = (u8*)arena_push(&a, 128u, 1u);
    memset(p, 0x11, 128u);
    arena_reset_to(&a, 16u);
    u32 not_poisoned = 0;
    for (u32 i = 16; i < 128u; ++i) { not_poisoned += (a.base[i] != 0xDDu) ? 1u : 0u; }
    TL_EXPECT_EQ(not_poisoned, 0u);
    for (u32 i = 0; i < 16u; ++i) { TL_EXPECT_EQ(a.base[i], (u8)0x11); }   // below mark untouched
    TL_EXPECT_EQ(a.used, (u64)16);
    TL_EXPECT_EQ(a.high_water, (u64)128);   // high_water survives reset - it tracks dirt, not use
    api.release(api.ctx, a.base, a.reserved);
#else
    TL_EXPECT_TRUE(true);   // poison is dev/debug-only by contract (docs/MEMORY.md §8.2)
#endif
}

TL_TEST(vmem_decommit_then_repush_is_zero, "foundation,mem,smoke") {
    VMemApi api = test_vmem_api();
    VMemArena a = {};
    TL_ASSERT_EQ(vmem_arena_init(&a, 0x6666u, 4u << 20, 0u, &api), ERR_OK);

    u8* p = (u8*)arena_push(&a, (u64)COMMIT_GRANULE * 3u, 16u);
    memset(p, 0xEE, (usize)COMMIT_GRANULE * 3u);
    const u64 committed_before = a.committed;

    arena_decommit_above(&a, 0u);
    TL_EXPECT_EQ(a.committed, (u64)0);
    TL_EXPECT_EQ(a.used, (u64)0);
    TL_EXPECT_EQ(a.high_water, (u64)0);
    TL_EXPECT_LT(a.committed, committed_before);

    // Re-push the same range WITHOUT ARENA_ZERO_ON_PUSH: pages must come back zero from the OS
    // (docs/PLATFORM.md §9.3 zero-fill guarantee) - this is what makes decommit safe on hashed
    // arenas.
    u8* q = (u8*)arena_push(&a, (u64)COMMIT_GRANULE * 3u, 16u);
    TL_EXPECT_TRUE(q == p);
    u32 nonzero = 0;
    for (u64 i = 0; i < (u64)COMMIT_GRANULE * 3u; ++i) { nonzero += (q[i] != 0u) ? 1u : 0u; }
    TL_EXPECT_EQ(nonzero, 0u);

    // Partial decommit: keep one granule's worth; high_water clamps to the decommit edge only.
    memset(q, 0xEE, (usize)COMMIT_GRANULE * 3u);
    arena_decommit_above(&a, 100u);
    TL_EXPECT_EQ(a.committed, (u64)COMMIT_GRANULE);
    TL_EXPECT_EQ(a.used, (u64)100);
    TL_EXPECT_EQ(a.high_water, (u64)COMMIT_GRANULE);   // [100, 64K) is still committed + dirty

    api.release(api.ctx, a.base, a.reserved);
}

TL_TEST(vmem_mark_reset_round_trip, "foundation,mem,fast") {
    VMemApi api = test_vmem_api();
    VMemArena a = {};
    TL_ASSERT_EQ(vmem_arena_init(&a, 0x7777u, 1u << 20, 0u, &api), ERR_OK);

    TL_EXPECT_EQ(arena_mark(&a), (u64)0);
    (void)arena_push(&a, 100u, 1u);
    const u64 m = arena_mark(&a);
    TL_EXPECT_EQ(m, (u64)100);
    (void)arena_push(&a, 100u, 1u);
    arena_reset_to(&a, m);
    TL_EXPECT_EQ(a.used, m);
    arena_reset_to(&a, arena_mark(&a));   // reset-to-self is a no-op
    TL_EXPECT_EQ(a.used, m);
    arena_reset_to(&a, 0u);               // full reset (empty edge)
    TL_EXPECT_EQ(a.used, (u64)0);

    api.release(api.ctx, a.base, a.reserved);
}
