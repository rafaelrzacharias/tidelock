// arena_registry.h (ArenaGuard) - the sanctioned windows pass; plus the 10k-tick headless run
// of the docs/MEMORY.md §8.8 done criterion.
// Spec: docs/MEMORY.md §2, §8.4, §8.7 test list. Rubric: docs/TESTING.md §7.
// Deferred to the runner lane's TL_TEST_EXPECT_FATAL: growth outside the window (registered
// arena push mid-tick), GROWS_AT_BARRIER growth before/after its window, shrink mid-tick,
// nonzero CRT delta - each is the guard's TL_FATAL by design.
#include "runner/tl_test.h"
#include "foundation/arena_registry.h"
#include "foundation/scratch.h"
#include "vmem_test_api.h"

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
