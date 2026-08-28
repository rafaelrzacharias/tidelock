// arena_registry.h (ArenaGuard) - the sanctioned windows pass; plus the 10k-tick headless run
// of the docs/MEMORY.md §8.8 done criterion.
// Spec: docs/MEMORY.md §2, §8.4, §8.7 test list. Rubric: docs/TESTING.md §7.
// Deferred to the runner lane's TL_TEST_EXPECT_FATAL: growth outside the window (registered
// arena push mid-tick), GROWS_AT_BARRIER growth before its window, nonzero CRT delta - each is
// the guard's TL_FATAL by design. **"shrink mid-tick" came off this list 2026-08-28** (PR #17
// review D2): the macro exists now, and that row is what pins guard_barrier_begin's `!=`
// comparison against a "fix" to `>` - see guard_barrier_begin_fatals_on_a_shrink_before_the_window.
#include "runner/tl_test.h"
#include "foundation/arena_registry.h"
#include "foundation/alloc_shim.h"
#include "foundation/scratch.h"
#include "vmem_test_api.h"

TL_TEST(alloc_shim_anchor_links_the_tripwires, "foundation,mem,smoke,fast") {
    // Ruled 2026-08-26: the CRT-malloc counter is DROPPED (docs/MEMORY.md §2) - the mechanism
    // is the new/delete TL_FATAL tripwires + the symbol audit + vendor pool hooks. This row
    // pins the link contract that remains: calling the anchor pulls alloc_shim's object (and
    // with it the tripwire operators) into every guard-using binary. The tripwires themselves
    // are fatal-expected rows (TESTING.md §9.1) the day TL_TEST_EXPECT_FATAL covers them.
    tl_alloc_shim_anchor();
    TL_EXPECT_TRUE(true);   // the check is that this TU links; the call above is the contract
}

TL_TEST(guard_sanctioned_windows_pass, "foundation,mem,smoke,fast") {
#if TL_DEV
    VMemApi api = test_vmem_api();
    VMemArena cols = {}, pool = {};
    TL_ASSERT_EQ(vmem_arena_init(&cols, 0x10u, 1u << 20, ARENA_ZERO_ON_PUSH, &api), ERR_OK);
    TL_ASSERT_EQ(vmem_arena_init(&pool, 0x20u, 1u << 20, ARENA_ZERO_ON_PUSH, &api), ERR_OK);
    ArenaRegistry reg = {};
    registry_add(&reg, 0x10u, &cols, ARENA_HASHED | ARENA_SNAPSHOT | ARENA_GROWS_AT_BARRIER);
    registry_add(&reg, 0x20u, &pool, ARENA_HASHED | ARENA_SNAPSHOT);
    registry_seal(&reg);

    ArenaGuard g = {};
    // A quiet tick: nothing moves, guard passes.
    guard_tick_begin(&g, &reg);
    guard_barrier_begin(&g, &reg);
    guard_barrier_end(&g, &reg);
    guard_tick_end(&g, &reg);

    // Growth INSIDE the barrier window on a GROWS_AT_BARRIER arena: sanctioned.
    guard_tick_begin(&g, &reg);
    guard_barrier_begin(&g, &reg);
    (void)arena_push(&cols, 512u, 16u);
    guard_barrier_end(&g, &reg);
    guard_tick_end(&g, &reg);

    // SHRINK inside the barrier window: equally sanctioned (docs/MEMORY.md section 2, ruled
    // 2026-08-28). Added by PR #17's review, D2: the shrink is what makes an ECS column's hashed
    // extent the LIVE extent (core/column.h), its legality rested on guard_barrier_begin
    // comparing `used != used_at_start` rather than `>`, and NO row exercised arena_reset_to
    // inside the window - so a later author "fixing" the guard to match the old grow-only prose
    // would have broken lockstep hashing with the suite green.
    guard_tick_begin(&g, &reg);
    guard_barrier_begin(&g, &reg);
    const u64 mark_before = arena_mark(&cols);
    (void)arena_push(&cols, 256u, 16u);
    arena_reset_to(&cols, mark_before);          // back down inside the same window
    TL_EXPECT_EQ(arena_mark(&cols), mark_before);
    guard_barrier_end(&g, &reg);
    guard_tick_end(&g, &reg);

    // And a shrink that is NOT balanced by a growth - `used` genuinely below where the tick
    // started, which is the shape column_remove produces.
    guard_tick_begin(&g, &reg);
    guard_barrier_begin(&g, &reg);
    arena_reset_to(&cols, arena_mark(&cols) - 128u);
    guard_barrier_end(&g, &reg);
    guard_tick_end(&g, &reg);

    // The re-baseline holds across consecutive ticks.
    guard_tick_begin(&g, &reg);
    guard_barrier_begin(&g, &reg);
    guard_barrier_end(&g, &reg);
    guard_tick_end(&g, &reg);

    api.release(api.ctx, cols.base, cols.reserved);
    api.release(api.ctx, pool.base, pool.reserved);
#else
    TL_EXPECT_TRUE(true);   // the guard compiles out below dev (docs/MEMORY.md §8.4)
#endif
}

TL_TEST(guard_10k_tick_headless_run, "foundation,mem,soak") {
#if TL_DEV
    // docs/MEMORY.md §8.8: "the arena-offset guard passes a 10k-tick headless run". No loop or
    // ECS exists yet, so the tick shape is synthetic but complete: scratch churn every tick,
    // barrier-window growth every 16th tick, scratch reset at the barrier - the §2 discipline.
    VMemApi api = test_vmem_api();
    VMemArena cols = {};
    TL_ASSERT_EQ(vmem_arena_init(&cols, 0x30u, 64u << 20, ARENA_ZERO_ON_PUSH, &api), ERR_OK);
    Scratch scratch = {};
    TL_ASSERT_EQ(scratch_init(&scratch, 0x40u, 16u << 20, &api), ERR_OK);
    ArenaRegistry reg = {};
    registry_add(&reg, 0x30u, &cols, ARENA_HASHED | ARENA_SNAPSHOT | ARENA_GROWS_AT_BARRIER);
    registry_seal(&reg);

    ArenaGuard g = {};
    for (u32 tick = 0; tick < 10000u; ++tick) {
        guard_tick_begin(&g, &reg);
        // Mid-tick work allocates only from scratch (the §2 exception).
        TL_SCRATCH_SCOPE_BEGIN(&scratch);
        u8* tmp = (u8*)scratch_push(&scratch, 256u + (tick & 1023u), 16u);
        tmp[0] = (u8)tick;
        TL_SCRATCH_SCOPE_END(&scratch);
        // Barrier: command-apply growth on the flagged arena, then scratch reset.
        guard_barrier_begin(&g, &reg);
        if ((tick & 15u) == 0u) { (void)arena_push(&cols, 64u, 16u); }
        guard_barrier_end(&g, &reg);
        scratch_reset(&scratch);
        guard_tick_end(&g, &reg);
    }
    TL_EXPECT_EQ(cols.used, (u64)((10000u + 15u) / 16u) * 64u);   // 625 windows grew (ticks 0,16,...,9984)

    api.release(api.ctx, cols.base, cols.reserved);
    api.release(api.ctx, scratch.a.base, scratch.a.reserved);
#else
    TL_EXPECT_TRUE(true);
#endif
}

// --- PR #17 review D2: the shrink half of the GROWS_AT_BARRIER contract ------------------------

TL_TEST_EXPECT_FATAL(guard_shrink_before_the_barrier_window_is_fatal, "foundation,mem,fatal") {
#if !TL_DEV
    // The whole guard is `#if TL_DEV` (arena_guard.cpp:11) and MEMORY.md section 2 says the
    // TL_FATAL is "in debug" - so below dev there is no trap to expect, and an EXPECT_FATAL body
    // that exits cleanly is a FAIL by design (runner/tl_test.h: "a trap is PASS and a clean exit
    // is FAIL"). TL_SKIP is the sanctioned no-checks path; registry.test.cpp records the same
    // shape. Caught by CI, not locally: this row passed dev and reddened both netcode and both
    // ship legs, which is LESSONS.md's "local validation on one tier proves nothing about a gate
    // that tier never exercises" - the four-leg matrix working as designed.
    TL_SKIP("the arena guard compiles out below dev (docs/MEMORY.md section 8.4)");
#else
    (void)t;
    // `used` MOVING outside the barrier window is fatal in BOTH directions, and until RR-48 only
    // the growth direction had a row. This one discriminates: guard_barrier_begin compares
    // `used != used_at_start`, and the docs said "grew" until 2026-08-28 - so an author
    // reconciling code to prose by relaxing the comparison to `>` would silently permit a
    // mid-tick shrink on a HASHED arena, which is a lockstep hash moving for a reason no state
    // change explains. NOTE the declaration is on ONE line: the generated test list is scanned
    // line-wise, and a split TL_TEST/TL_TEST_EXPECT_FATAL emits a truncated tag string that fails
    // the build inside test_list.inc, naming neither this file nor this test.
    // The sanctioned-window rows cannot catch that: a GROWS_AT_BARRIER arena is
    // exempt at guard_tick_end by flag, so guard_barrier_begin is the only gate that sees this.
    VMemApi api = test_vmem_api();
    VMemArena cols = {};
    TL_ASSERT_EQ(vmem_arena_init(&cols, 0x10u, 1u << 20, ARENA_ZERO_ON_PUSH, &api), ERR_OK);
    ArenaRegistry reg = {};
    registry_add(&reg, 0x10u, &cols, ARENA_HASHED | ARENA_SNAPSHOT | ARENA_GROWS_AT_BARRIER);
    registry_seal(&reg);

    // Grow BEFORE the baseline, so the shrink below is the only movement the guard can see -
    // otherwise this row would fatal on a growth and pass while naming a shrink.
    (void)arena_push(&cols, 512u, 16u);
    const u64 baseline = arena_mark(&cols);

    ArenaGuard g = {};
    guard_tick_begin(&g, &reg);              // baselines used[] at `baseline`
    arena_reset_to(&cols, baseline - 256u);  // SHRINK, mid-tick, outside the window
    guard_barrier_begin(&g, &reg);           // <- fatals here: used != used_at_start
    guard_barrier_end(&g, &reg);
    guard_tick_end(&g, &reg);
#endif
}
