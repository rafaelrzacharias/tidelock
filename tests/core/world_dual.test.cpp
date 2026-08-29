// The §10.8 dual-sim row: two worlds, same seed, same registrations, same (empty) inputs ->
// identical per-arena hashes every tick, under structural churn (keyed-RNG spawns, moves,
// destroys, events); plus the mid-run restore reproducing the hash trace (MEMORY.md §8.8's
// third criterion, the ECS half). Spec: docs/ECS.md §8/§10.8; docs/DETERMINISM.md §6.
#include "world_test_util.h"
#include "foundation/rng.h"
#include "foundation/snapshot.h"

namespace {

// The churn systems: every effect is a pure function of (seed, tick, entity bits) through
// rng_for - scheduling and iteration order cannot change a draw (docs/DETERMINISM.md §3).
// system_id values 2..4 are test-local (the closed enum is rng_systems.h's; tests are outside
// the sim registry and just need nonzero, distinct keys).

void sys_dual_spawner(World* w) {
    const u64 tick = w->state->tick;
    if (tick % 3u != 0u) { return; }
    Entity e = world_spawn(w);
    const u64 r = rng_for(w->state->seed, tick, 2u, 0u, 0u);
    WPos p = { (i32)(u32)(r & 0xFFFFu), (i32)(u32)((r >> 16) & 0xFFFFu) };
    world_add<WPos>(w, e, p);
    if ((r >> 63) != 0u) {
        WVel v = { (i32)((r >> 32) % 5u) - 2, (i32)((r >> 40) % 5u) - 2 };
        world_add<WVel>(w, e, v);
    }
    WEvSpawned ev = { e, (u32)tick };
    eq_emit<WEvSpawned>(w, ev);
}

void sys_dual_move(World* w) {
    Span<WVel> vels = world_column<WVel>(w);
    Span<Entity> ents = world_entities<WVel>(w);
    for (u32 i = 0; i < vels.count; ++i) {
        WPos* p = world_get<WPos>(w, ents.data[i]);
        if (p != nullptr) { p->x += vels.data[i].dx; p->y += vels.data[i].dy; }
    }
}

void sys_dual_reaper(World* w) {
    Span<Entity> ents = world_entities<WPos>(w);
    for (u32 i = 0; i < ents.count; ++i) {
        const u64 r = rng_for(w->state->seed, w->state->tick, 3u, ents.data[i].bits, 0u);
        if ((r & 7u) == 0u) { world_destroy(w, ents.data[i]); }
    }
}

// Feeds last tick's events back into HASHED state (the WCfg singleton) - the dual half runs
// it so the event path is inside the determinism claim. The restore half must NOT register it:
// a restore clears both event halves (docs/ECS.md §10.4), so the tick after a restore would
// see fewer events than the original run did and legitimately diverge - the cross-lane
// consequence for rollback resim is filed in TODO.md (W2 ecs notes, E-5).
void sys_dual_reader(World* w) {
    Span<const WEvSpawned> evs = eq_read<WEvSpawned>(w);
    for (u32 i = 0; i < evs.count; ++i) { world_singleton<WCfg>(w)->gravity += evs.data[i].n + 1u; }
}

void dual_register(WorldFixture* f, bool with_reader) {
    world_fixture_register_std(f);
    SystemDesc d;
    memset(&d, 0, sizeof(d));
    d.fn = sys_dual_spawner; d.label = "dspawn"_id; d.phase = PHASE_PRE_UPDATE;
    world_register_system(&f->w, &d);
    d.fn = sys_dual_move; d.label = "dmove"_id; d.phase = PHASE_UPDATE;
    world_register_system(&f->w, &d);
    d.fn = sys_dual_reaper; d.label = "dreap"_id; d.phase = PHASE_POST_UPDATE;
    world_register_system(&f->w, &d);
    if (with_reader) {
        d.fn = sys_dual_reader; d.label = "dread"_id; d.phase = PHASE_POST_UPDATE;
        world_register_system(&f->w, &d);
    }
    world_build_schedule(&f->w);
    registry_seal(&f->reg);
}

}  // namespace

TL_TEST(world_dual_identical_per_arena_hashes_every_tick, "core,ecs,world,determinism,fast") {
    // Two worlds in one process (why static state is banned), one with a guard armed and a
    // deliberately dirtied scratch - neither may reach the hashes.
    WorldFixture& a = *wt_fixture(0u);
    WorldFixture& b = *wt_fixture(1u);
    TL_ASSERT_TRUE(world_fixture_init(&a, 99u));
    TL_ASSERT_TRUE(world_fixture_init(&b, 99u));
    dual_register(&a, true);
    dual_register(&b, true);

    ArenaGuard guard;
    memset(&guard, 0, sizeof(guard));
    b.w.guard = &guard;
    void* junk = scratch_push(&b.scratch, 128u * 1024u, 16u);
    memset(junk, 0x5A, 128u * 1024u);
    scratch_reset(&b.scratch);

    TL_ASSERT_EQ(a.reg.count, b.reg.count);
    u32 live_ticks = 0;
    for (u32 tick = 0; tick < 120u; ++tick) {
        wt_tick(&a.w);
        guard_tick_begin(&guard, &b.reg);
        wt_tick(&b.w);
        guard_tick_end(&guard, &b.reg);
        u64 pa[MAX_ARENAS];
        u64 pb[MAX_ARENAS];
        const u64 ha = registry_hash_all(&a.reg, pa);
        const u64 hb = registry_hash_all(&b.reg, pb);
        if (ha != hb) {
            TL_ASSERT_EQ(ha, hb);   // first divergent tick: fail loud with both values
        }
        for (u32 i = 0; i < a.reg.count; ++i) {
            if (pa[i] != pb[i]) { TL_ASSERT_EQ(pa[i], pb[i]); }
        }
        ++live_ticks;
    }
    TL_EXPECT_EQ(live_ticks, 120u);
    // The churn actually churned: entities exist, some died, events flowed into the singleton.
    TL_EXPECT_GT(a.w.entities.live_count, 0u);
    TL_EXPECT_GT(a.w.entities.free_list.count + a.w.entities.quarantined, 0u);
    TL_EXPECT_GT(world_singleton<WCfg>(&a.w)->gravity, 0u);
    TL_EXPECT_EQ(world_singleton<WCfg>(&a.w)->gravity, world_singleton<WCfg>(&b.w)->gravity);
}

TL_TEST(world_dual_restore_reproduces_the_hash_trace, "core,ecs,world,determinism,fast") {
    // Run 40 ticks, snapshot, run 40 more recording the trace, restore, re-run 40: identical
    // trace (docs/MEMORY.md §8.8). No event->hashed-state feedback in this half (see
    // sys_dual_reader's note): a restore clears the event halves by contract.
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 1234u));
    dual_register(&f, false);

    for (u32 tick = 0; tick < 40u; ++tick) { wt_tick(&f.w); }

    VMemArena blob_arena;
    TL_ASSERT_EQ(vmem_arena_init(&blob_arena, "wd.blob"_id, 64u * 1024u * 1024u, 0u, &f.api), ERR_OK);
    Snapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.blob_cap = 64u * 1024u * 1024u;
    snap.blob = (u8*)arena_push(&blob_arena, snap.blob_cap, 64u);
    TL_ASSERT_EQ(registry_snapshot(&f.reg, &snap, f.w.state->tick), ERR_OK);

    u64 trace[40];
    u64 per[MAX_ARENAS];
    for (u32 i = 0; i < 40u; ++i) {
        wt_tick(&f.w);
        trace[i] = registry_hash_all(&f.reg, per);
    }

    TL_ASSERT_EQ(registry_restore(&f.reg, &snap), ERR_OK);
    world_post_restore(&f.w);
    for (u32 i = 0; i < 40u; ++i) {
        wt_tick(&f.w);
        const u64 h = registry_hash_all(&f.reg, per);
        if (h != trace[i]) { TL_ASSERT_EQ(h, trace[i]); }
    }
    ++t->checks;
}

// --- RR-48: the hashed extent is the LIVE extent, not a high-water mark ------------------------

TL_TEST(world_divergent_histories_hash_the_same_extent, "core,ecs,determinism") {
    // The property RR-48 buys, and the one the previous "used never shrinks" ruling explicitly
    // did NOT hold: build the SAME live state two different ways and require the same hash.
    // NAMED FOR THE EXTENT DELIBERATELY (RR-54, ruled 2026-08-28; PR #17 ship round D1). RR-48
    // closes the EXTENT channel only. This fixture removes a2 - dense index 1 with `last` == 2 -
    // so swap-remove lifts a3 into slot 1 and reproduces B's order exactly; the dense-ORDER
    // channel is therefore silent here BY CONSTRUCTION, and an earlier name claiming the general
    // "equal state hashes equally" property overstated what the body pins. The order channel is
    // open by design and gets its own negation row below.
    // LESSONS.md names this directly - "two instances, same op sequence" is the weakest
    // determinism test that still looks like one; the property worth testing is divergent
    // histories that converge. Before RR-48 world A's dense arena carried three rows' worth of
    // `used` against B's two, and the two hashed differently with every live byte equal.
    WorldFixture& a = *wt_fixture(0u);
    WorldFixture& b = *wt_fixture(1u);
    TL_ASSERT_TRUE(world_fixture_init(&a, 7u));
    TL_ASSERT_TRUE(world_fixture_init(&b, 7u));
    // Components register arenas, so the registry is sealed AFTER registration, and the world
    // must be sealed (world_build_schedule) before commands.cpp will accept a command.
    world_fixture_register_std(&a);
    world_fixture_register_std(&b);
    world_build_schedule(&a.w);
    world_build_schedule(&b.w);
    registry_seal(&a.reg);
    registry_seal(&b.reg);

    // A: spawn three, give all three a WPos, then remove the middle one's.
    Entity a1 = world_spawn(&a.w), a2 = world_spawn(&a.w), a3 = world_spawn(&a.w);
    world_flush(&a.w);
    world_add<WPos>(&a.w, a1, WPos{ 11, 12 });
    world_add<WPos>(&a.w, a2, WPos{ 21, 22 });
    world_add<WPos>(&a.w, a3, WPos{ 31, 32 });
    world_flush(&a.w);
    world_remove(&a.w, a2, (ComponentId)world_component_id<WPos>(&a.w));
    world_flush(&a.w);

    // B: spawn the same three, give WPos to only the two that survive in A. Same live rows,
    // never a third row allocated - the histories differ, the state does not.
    Entity b1 = world_spawn(&b.w), b2 = world_spawn(&b.w), b3 = world_spawn(&b.w);
    (void)b2;
    world_flush(&b.w);
    world_add<WPos>(&b.w, b1, WPos{ 11, 12 });
    world_add<WPos>(&b.w, b3, WPos{ 31, 32 });
    world_flush(&b.w);

    u64 pa[MAX_ARENAS];
    u64 pb[MAX_ARENAS];
    const u64 ha = registry_hash_all(&a.reg, pa);
    const u64 hb = registry_hash_all(&b.reg, pb);
    // Per-arena first, so a failure names WHICH arena rather than only the fold.
    TL_ASSERT_EQ(a.reg.count, b.reg.count);
    for (u32 i = 0; i < a.reg.count; ++i) {
        if (pa[i] != pb[i]) { TL_ASSERT_EQ(pa[i], pb[i]); }
    }
    TL_EXPECT_EQ(ha, hb);
}

TL_TEST(world_divergent_removal_order_hashes_differently_by_design, "core,ecs,determinism") {
    // The NEGATION of the row above, and the reason it is here rather than filed as a defect:
    // RR-54 (ruled 2026-08-28 by Rafael) settled that the dense-ORDER channel stays
    // history-dependent. column_remove is swap-remove, so WHICH row was removed decides the
    // permutation; src/core/interp.cpp:29 is the standing statement of that. LESSONS.md: where a
    // property is false by design, test the negation and say so, so nobody "fixes" it later.
    // Differs from the row above in ONE character - a1 instead of a2 - and that is the whole
    // point: removing dense index 0 makes swap-remove lift the LAST row into slot 0, which no
    // add-only history reproduces. If this row ever goes green, the order channel was closed
    // without amending RR-54 and column.h's HASHING RULING block is now wrong.
    WorldFixture& a = *wt_fixture(0u);
    WorldFixture& b = *wt_fixture(1u);
    TL_ASSERT_TRUE(world_fixture_init(&a, 7u));
    TL_ASSERT_TRUE(world_fixture_init(&b, 7u));
    world_fixture_register_std(&a);
    world_fixture_register_std(&b);
    world_build_schedule(&a.w);
    world_build_schedule(&b.w);
    registry_seal(&a.reg);
    registry_seal(&b.reg);

    // A: spawn three, give all three a WPos, then remove the FIRST one's. Swap-remove lifts a3
    // into slot 0, so A's dense order is [a3, a2] - values [{31,32}, {21,22}].
    Entity a1 = world_spawn(&a.w), a2 = world_spawn(&a.w), a3 = world_spawn(&a.w);
    (void)a2; (void)a3;
    world_flush(&a.w);
    world_add<WPos>(&a.w, a1, WPos{ 11, 12 });
    world_add<WPos>(&a.w, a2, WPos{ 21, 22 });
    world_add<WPos>(&a.w, a3, WPos{ 31, 32 });
    world_flush(&a.w);
    world_remove(&a.w, a1, (ComponentId)world_component_id<WPos>(&a.w));
    world_flush(&a.w);

    // B: the same two survivors, added directly - dense order [b2, b3], values
    // [{21,22}, {31,32}]. Same live SET, same row count, opposite permutation.
    Entity b1 = world_spawn(&b.w), b2 = world_spawn(&b.w), b3 = world_spawn(&b.w);
    (void)b1;
    world_flush(&b.w);
    world_add<WPos>(&b.w, b2, WPos{ 21, 22 });
    world_add<WPos>(&b.w, b3, WPos{ 31, 32 });
    world_flush(&b.w);

    // Same number of live rows...
    TL_ASSERT_EQ(world_column<WPos>(&a.w).count, world_column<WPos>(&b.w).count);
    // ...and RR-48 holds, so every hashed arena spans the same number of bytes. This is the half
    // that must stay true: if an extent ever differs here the failure is RR-48's, not RR-54's.
    TL_ASSERT_EQ(a.reg.count, b.reg.count);
    for (u32 i = 0; i < a.reg.count; ++i) {
        TL_ASSERT_EQ(a.reg.e[i].arena->used, b.reg.e[i].arena->used);
    }

    // ...yet the fold differs, because the bytes are in a different order. By design.
    u64 pa[MAX_ARENAS];
    u64 pb[MAX_ARENAS];
    const u64 ha = registry_hash_all(&a.reg, pa);
    const u64 hb = registry_hash_all(&b.reg, pb);
    TL_EXPECT_TRUE(ha != hb);
}
