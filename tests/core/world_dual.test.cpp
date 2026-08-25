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
    WorldFixture a;
    WorldFixture b;
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
    WorldFixture f;
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
