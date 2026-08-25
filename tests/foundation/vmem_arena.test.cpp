// vmem_arena.h - push/align/commit growth, reset poison, decommit-then-repush-is-zero, the
// high_water re-zero rule, the alignment-gap re-zero rule.
// Spec: docs/MEMORY.md §8.2, §8.7 test list. Rubric: docs/TESTING.md §7.
// Deferred to the runner lane's TL_TEST_EXPECT_FATAL (docs/TESTING.md §9.1): wrapped-extent push,
// bad align, reset mark > used - each is a dev assert with no release value to pin (the arena has
// no error path there by design). The over-reserve push is covered now that the macro exists
// (vmem_push_one_byte_past_reserve_is_fatal, bottom of this file).
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

    // reserve_bytes rounds up to COMMIT_GRANULE, not to a page (ruled 2026-08-24).
    VMemArena b = {};
    TL_ASSERT_EQ(vmem_arena_init(&b, 0x2222u, 100u, 0u, &api), ERR_OK);
    TL_EXPECT_EQ(b.reserved, (u64)COMMIT_GRANULE);
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

TL_TEST(vmem_commit_exactly_at_reserve_edge, "foundation,mem,fast") {
    // Edge matrix: a push whose commit lands EXACTLY on the reserve boundary is legal - the
    // over-reserve fatal is strictly `>`, not `>=` (docs/MEMORY.md §8.2).
    VMemApi api = test_vmem_api();
    VMemArena a = {};
    TL_ASSERT_EQ(vmem_arena_init(&a, 0x8888u, (u64)COMMIT_GRANULE * 4u, 0u, &api), ERR_OK);
    u8* p = (u8*)arena_push(&a, (u64)COMMIT_GRANULE * 4u, 1u);
    TL_ASSERT_TRUE(p != nullptr);
    TL_EXPECT_EQ(a.used, a.reserved);
    TL_EXPECT_EQ(a.committed, a.reserved);
    p[a.reserved - 1u] = 0x7Fu;   // the last byte is really committed
    TL_EXPECT_EQ(a.base[a.reserved - 1u], (u8)0x7F);
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

// --- the 2026-08-24 ruling: the stated budget is the usable budget -----------------------------

TL_TEST(vmem_reserve_rounds_up_to_commit_granule, "foundation,mem,smoke,fast") {
    // docs/MEMORY.md section 8.2: vmem_arena_init rounds the reserve up to COMMIT_GRANULE, not to
    // a page. arena_push commits in granule multiples and fatals when align_up(end,
    // COMMIT_GRANULE) > reserved, so under page rounding the usable budget was
    // round_down(reserved, COMMIT_GRANULE): a sub-64 KB reserve could never push a single byte,
    // and a non-multiple reserve's tail was unreachable. Both cases, both properties: `reserved`
    // is a granule multiple >= the request, and EVERY requested byte is really pushable and
    // writable.
    VMemApi api = test_vmem_api();

    // (a) sub-granule reserve. 100 bytes rounded to one granule; before the ruling this arena
    // reserved one page and the first push of any size fatalled.
    VMemArena small = {};
    TL_ASSERT_EQ(vmem_arena_init(&small, 0x9001u, 100u, 0u, &api), ERR_OK);
    TL_EXPECT_EQ(small.reserved, (u64)COMMIT_GRANULE);
    TL_EXPECT_EQ(small.reserved % (u64)COMMIT_GRANULE, (u64)0);
    u8* ps = (u8*)arena_push(&small, 100u, 1u);
    TL_ASSERT_TRUE(ps != nullptr);
    for (u64 i = 0; i < 100u; ++i) { ps[i] = (u8)(i + 1u); }
    TL_EXPECT_EQ(ps[99], (u8)100);
    TL_EXPECT_EQ(small.used, (u64)100);
    api.release(api.ctx, small.base, small.reserved);

    // (b) non-multiple reserve: two granules plus a 100-byte tail. Before the ruling `reserved`
    // was 2 granules + one page and pushing the whole REQUESTED size fatalled - the request was
    // not a lie, the rounding was.
    const u64 want = (u64)COMMIT_GRANULE * 2u + 100u;
    VMemArena odd = {};
    TL_ASSERT_EQ(vmem_arena_init(&odd, 0x9002u, want, 0u, &api), ERR_OK);
    TL_EXPECT_EQ(odd.reserved, (u64)COMMIT_GRANULE * 3u);
    TL_EXPECT_GE(odd.reserved, want);
    TL_EXPECT_EQ(odd.reserved % (u64)COMMIT_GRANULE, (u64)0);
    u8* po = (u8*)arena_push(&odd, want, 1u);
    TL_ASSERT_TRUE(po != nullptr);
    po[0] = 0x11u;
    po[want - 1u] = 0x22u;                       // the last REQUESTED byte is committed
    TL_EXPECT_EQ(odd.base[want - 1u], (u8)0x22);
    TL_EXPECT_EQ(odd.used, want);

    // And the rounded-up remainder is usable too, up to the edge exactly: the fatal is at
    // `> reserved`, and `reserved` is now the number the caller can actually spend.
    u8* tail = (u8*)arena_push(&odd, odd.reserved - want, 1u);
    TL_ASSERT_TRUE(tail != nullptr);
    TL_EXPECT_EQ(odd.used, odd.reserved);
    TL_EXPECT_EQ(odd.committed, odd.reserved);
    odd.base[odd.reserved - 1u] = 0x33u;
    TL_EXPECT_EQ(odd.base[odd.reserved - 1u], (u8)0x33);
    api.release(api.ctx, odd.base, odd.reserved);
}

TL_TEST_EXPECT_FATAL(vmem_push_one_byte_past_reserve_is_fatal, "foundation,mem,fatal") {
    (void)t;
    // The other half of "the fatal coincides with the real edge": with the whole reserve spent,
    // ONE more byte is over budget and TL_FATALs (docs/MEMORY.md section 1.1). Paired with
    // vmem_reserve_rounds_up_to_commit_granule, which proves every byte up to `reserved` is
    // pushable, this pins the edge from both sides. A sub-granule request on purpose: this arena
    // could not push at all before the ruling. The over-reserve check is a TL_FATAL, live in
    // EVERY tier, and tl_child_verdict has been tier-agnostic since the wave merge
    // (runner_core.h) - so this row runs on all four tiers. The 2026-08-25 review sweep replaced
    // the netcode/ship TL_SKIP here, which cited a runner limitation that no longer exists.
    VMemApi api = test_vmem_api();
    VMemArena a = {};
    if (vmem_arena_init(&a, 0x9003u, 100u, 0u, &api) != ERR_OK) {
        return;   // setup failed: exits 0, which the runner scores FAIL for a fatal-expected row
    }
    (void)arena_push(&a, a.reserved, 1u);   // the whole budget: legal, the edge is inclusive
    (void)arena_push(&a, 1u, 1u);           // one past it: TL_FATAL, exit 2
}
