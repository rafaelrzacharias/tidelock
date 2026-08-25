// commands.h + the world recording API - spawn-before-realize, LIFO reuse, destroy-from-all-
// columns, chunk apply order (= schedule order), record order within a chunk, set-field,
// events across ticks, the guard's barrier window, and the E-4 add-after-destroy fatal.
// Spec: docs/ECS.md §4/§10.5, §10.8 (commands.test.cpp line). Rubric: docs/TESTING.md §7.
#include "world_test_util.h"
#include "foundation/arena_registry.h"

namespace {

// Cross-system plumbing for the system-driven tests (tests are exempt from the src/ static
// ban - docs/TESTING.md §8 R-2). Reset at each test's start.
Entity g_target = Entity{ 0 };
u32 g_reader_seen = 0;

// Chunk-order probes: each sets the WCfg singleton via the command channel; the LAST chunk in
// ascending chunk_id order (= schedule order) must win.
void sys_set_cfg_1(World* w) {
    WCfg v = { 1u, 1u, { 0, 0, 0, 0 } };
    world_singleton_set_cmd(w, world_component_id<WCfg>(w), &v);
}
void sys_set_cfg_2(World* w) {
    // Two records in one chunk: record order within the chunk means the second wins.
    WCfg first = { 100u, 100u, { 0, 0, 0, 0 } };
    WCfg second = { 2u, 2u, { 0, 0, 0, 0 } };
    world_singleton_set_cmd(w, world_component_id<WCfg>(w), &first);
    world_singleton_set_cmd(w, world_component_id<WCfg>(w), &second);
}
void sys_set_cfg_3(World* w) {
    WCfg v = { 3u, 3u, { 0, 0, 0, 0 } };
    world_singleton_set_cmd(w, world_component_id<WCfg>(w), &v);
}

// Spawner + reader pair for the event/tick integration test.
void sys_spawner(World* w) {
    if (w->state->tick != 0u) { return; }
    Entity e = world_spawn(w);
    WPos p = { 7, 8 };
    world_add<WPos>(w, e, p);
    WEvSpawned ev = { e, 1u };
    eq_emit<WEvSpawned>(w, ev);
    g_target = e;
}
void sys_reader(World* w) {
    Span<const WEvSpawned> evs = eq_read<WEvSpawned>(w);
    for (u32 i = 0; i < evs.count; ++i) {
        g_reader_seen += evs.data[i].n;
        // One-tick latency: by the time the event is readable, the barrier has realized the
        // spawn and applied the add.
        if (world_entity_alive(w, evs.data[i].who) && world_get<WPos>(w, evs.data[i].who) != nullptr) {
            g_reader_seen += 100u;
        }
    }
}

// E-4 pair: the destroyer runs (and applies) before the adder's chunk in the same barrier.
void sys_destroyer(World* w) {
    if (w->state->tick == 1u) { world_destroy(w, g_target); }
}

// Review 1 D2/D4 probes: a FIRST-phase fresh spawner (its chunk applies before the external
// chunk that recorded first), and a FIRST-phase destroyer of the externally reserved target.
Entity g_sys_spawned = Entity{ 0 };
void sys_first_fresh_spawner(World* w) {
    if (w->state->tick == 0u) { g_sys_spawned = world_spawn(w); }
}
void sys_first_destroys_target(World* w) {
    if (w->state->tick == 0u) { world_destroy(w, g_target); }
}
void sys_adder(World* w) {
    if (w->state->tick == 1u) {
        WVel v = { 9, 9 };
        world_add<WVel>(w, g_target, v);
    }
}

// Registers a bare system in a phase (no deps).
void reg_sys(World* w, SystemFn fn, NameHash label, Phase phase, NameHash before, NameHash after) {
    SystemDesc d;
    d.fn = fn;
    d.label = label;
    d.phase = phase;
    d.reads = Span<const ComponentId>{ nullptr, 0 };
    d.writes = Span<const ComponentId>{ nullptr, 0 };
    d.before = Span<const NameHash>{ before != 0u ? &before : nullptr, before != 0u ? 1u : 0u };
    d.after = Span<const NameHash>{ after != 0u ? &after : nullptr, after != 0u ? 1u : 0u };
    d.flags = 0;
    world_register_system(w, &d);
}

}  // namespace

TL_TEST(commands_spawn_id_usable_before_realize, "core,ecs,commands,smoke,fast") {
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 3u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);

    Entity e = world_spawn(&f.w);   // external chunk (no system running)
    TL_EXPECT_FALSE(world_entity_alive(&f.w, e));   // reserved, not realized
    TL_EXPECT_FALSE(handle_is_null(e));
    WPos p = { 4, 5 };
    world_add<WPos>(&f.w, e, p);    // commandable before realize (docs/ECS.md §1)
    TL_EXPECT_EQ(world_column<WPos>(&f.w).count, 0u);   // nothing visible mid-window

    world_flush(&f.w);
    TL_EXPECT_TRUE(world_entity_alive(&f.w, e));
    TL_ASSERT_EQ(world_column<WPos>(&f.w).count, 1u);
    TL_EXPECT_EQ(world_entities<WPos>(&f.w).data[0].bits, e.bits);
    WPos* got = world_get<WPos>(&f.w, e);
    TL_ASSERT_NOT_NULL(got);
    TL_EXPECT_EQ(got->x, 4);

    // Three fresh reservations in one window get consecutive indices, all realized together.
    Entity a = world_spawn(&f.w);
    Entity b = world_spawn(&f.w);
    Entity c = world_spawn(&f.w);
    TL_EXPECT_EQ(handle_index(b), handle_index(a) + 1u);
    TL_EXPECT_EQ(handle_index(c), handle_index(a) + 2u);
    TL_EXPECT_EQ(f.w.pending_fresh, 3u);
    world_flush(&f.w);
    TL_EXPECT_EQ(f.w.pending_fresh, 0u);
    TL_EXPECT_TRUE(world_entity_alive(&f.w, a));
    TL_EXPECT_TRUE(world_entity_alive(&f.w, c));
}

TL_TEST(commands_destroy_removes_from_every_column_then_lifo_reuse, "core,ecs,commands,fast") {
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 3u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);

    Entity e = world_spawn(&f.w);
    WPos p = { 1, 2 };
    WVel v = { 3, 4 };
    world_add<WPos>(&f.w, e, p);
    world_add<WVel>(&f.w, e, v);
    world_flush(&f.w);
    TL_ASSERT_EQ(world_column<WPos>(&f.w).count, 1u);
    TL_ASSERT_EQ(world_column<WVel>(&f.w).count, 1u);

    world_destroy(&f.w, e);
    world_flush(&f.w);
    TL_EXPECT_FALSE(world_entity_alive(&f.w, e));
    TL_EXPECT_EQ(world_column<WPos>(&f.w).count, 0u);
    TL_EXPECT_EQ(world_column<WVel>(&f.w).count, 0u);
    TL_EXPECT_NULL(world_get<WPos>(&f.w, e));   // stale handle: absent, no assert

    // Destroying the dead handle again is the normal stale flow: applies as a no-op.
    world_destroy(&f.w, e);
    world_flush(&f.w);
    TL_EXPECT_FALSE(world_entity_alive(&f.w, e));

    // LIFO reuse at the next generation.
    Entity e2 = world_spawn(&f.w);
    world_flush(&f.w);
    TL_EXPECT_EQ(handle_index(e2), handle_index(e));
    TL_EXPECT_EQ(handle_gen(e2), handle_gen(e) + 1u);
    TL_EXPECT_NULL(world_get<WPos>(&f.w, e));   // old handle still absent against the reused slot
}

TL_TEST(commands_apply_in_schedule_chunk_order_record_order_within, "core,ecs,commands,determinism,fast") {
    // Registration order: cfg1, cfg2, cfg3 - but cfg3 is scheduled BEFORE cfg2 via before/after,
    // so chunk order is cfg1(0), cfg3(1), cfg2(2) and cfg2's LAST record wins the barrier.
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 3u));
    world_fixture_register_std(&f);
    reg_sys(&f.w, sys_set_cfg_1, "cfg1"_id, PHASE_UPDATE, 0, 0);
    reg_sys(&f.w, sys_set_cfg_2, "cfg2"_id, PHASE_UPDATE, 0, "cfg3"_id);
    reg_sys(&f.w, sys_set_cfg_3, "cfg3"_id, PHASE_UPDATE, 0, 0);
    world_build_schedule(&f.w);

    run_phase(&f.w, PHASE_UPDATE);
    TL_EXPECT_EQ(world_singleton<WCfg>(&f.w)->gravity, 2u);   // cfg2: last chunk, last record
    TL_EXPECT_EQ(world_singleton<WCfg>(&f.w)->mode, 2u);
}

TL_TEST(commands_set_field_pokes_one_field_at_the_barrier, "core,ecs,commands,fast") {
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 3u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);

    Entity e = world_spawn(&f.w);
    WPos p = { 11, 22 };
    world_add<WPos>(&f.w, e, p);
    world_flush(&f.w);

    // Field 1 (y) via the editor/Luau cold path; x must be untouched.
    const i32 new_y = -9;
    world_set_field_cmd(&f.w, e, world_component_id<WPos>(&f.w), 1u, &new_y, sizeof(new_y));
    TL_EXPECT_EQ(world_get<WPos>(&f.w, e)->y, 22);   // deferred until the barrier
    world_flush(&f.w);
    TL_EXPECT_EQ(world_get<WPos>(&f.w, e)->x, 11);
    TL_EXPECT_EQ(world_get<WPos>(&f.w, e)->y, -9);

    // Singleton field poke: field 1 (mode).
    const u32 new_mode = 5u;
    world_set_field_cmd(&f.w, Entity{ 0 }, world_component_id<WCfg>(&f.w), 1u, &new_mode, sizeof(new_mode));
    world_flush(&f.w);
    TL_EXPECT_EQ(world_singleton<WCfg>(&f.w)->mode, 5u);
    TL_EXPECT_EQ(world_singleton<WCfg>(&f.w)->gravity, 0u);
}

TL_TEST(commands_ticks_spawn_event_read_next_tick_under_the_guard, "core,ecs,commands,determinism,fast") {
    // Full-tick integration: a spawner emits an event in tick 0; the reader sees it in tick 1
    // with the entity realized and its component applied; the arena-offset guard brackets every
    // tick, so all structural growth must land inside the barrier windows.
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 3u));
    world_fixture_register_std(&f);
    g_target = Entity{ 0 };
    g_reader_seen = 0;
    reg_sys(&f.w, sys_spawner, "spawner"_id, PHASE_UPDATE, 0, 0);
    reg_sys(&f.w, sys_reader, "reader"_id, PHASE_POST_UPDATE, 0, 0);
    world_build_schedule(&f.w);
    registry_seal(&f.reg);

    ArenaGuard guard;
    memset(&guard, 0, sizeof(guard));
    f.w.guard = &guard;

    guard_tick_begin(&guard, &f.reg);
    wt_tick(&f.w);   // tick 0: spawn + emit
    guard_tick_end(&guard, &f.reg);
    TL_EXPECT_EQ(g_reader_seen, 0u);   // one-tick latency
    TL_EXPECT_TRUE(world_entity_alive(&f.w, g_target));

    guard_tick_begin(&guard, &f.reg);
    wt_tick(&f.w);   // tick 1: the reader sees the event AND the realized entity
    guard_tick_end(&guard, &f.reg);
    TL_EXPECT_EQ(g_reader_seen, 101u);

    guard_tick_begin(&guard, &f.reg);
    wt_tick(&f.w);   // tick 2: the event is gone
    guard_tick_end(&guard, &f.reg);
    TL_EXPECT_EQ(g_reader_seen, 101u);
    TL_EXPECT_EQ(f.w.state->tick, 3u);
}

TL_TEST_EXPECT_FATAL(commands_add_after_destroy_in_one_window_is_fatal, "core,ecs,commands,fatal") {
    // TODO.md E-4: the destroyer's chunk applies first (schedule order), so the adder's
    // CMD_ADD meets a dead entity - currently a TL_CHECK, filed for a ruling.
    WorldFixture& f = *wt_fixture(0u);
    if (!world_fixture_init(&f, 3u)) { return; }
    world_fixture_register_std(&f);
    g_target = Entity{ 0 };
    reg_sys(&f.w, sys_spawner, "spawner"_id, PHASE_UPDATE, 0, 0);
    reg_sys(&f.w, sys_destroyer, "destroyer"_id, PHASE_UPDATE, 0, 0);
    reg_sys(&f.w, sys_adder, "adder"_id, PHASE_UPDATE, 0, 0);
    world_build_schedule(&f.w);
    wt_tick(&f.w);   // tick 0: spawn only
    ++t->checks;
    wt_tick(&f.w);   // tick 1: destroy (chunk 1) then add (chunk 2) -> TL_CHECK fatal
}

TL_TEST(commands_external_and_system_fresh_spawns_interleave, "core,ecs,commands,edge,fast") {
    // Review 1 D2: the external chunk RECORDS first and APPLIES last, so fresh ids realize out
    // of reservation order - the system's realize pushes both slots, the external realize
    // finds its slot already pushed. Both entities land; pending returns to zero.
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 3u));
    world_fixture_register_std(&f);
    g_sys_spawned = Entity{ 0 };
    reg_sys(&f.w, sys_first_fresh_spawner, "ffspawn"_id, PHASE_FIRST, 0, 0);
    world_build_schedule(&f.w);

    Entity ext = world_spawn(&f.w);   // external: reserves fresh idx 0
    TL_EXPECT_EQ(handle_index(ext), 0u);
    TL_EXPECT_EQ(f.w.pending_fresh, 1u);
    wt_tick(&f.w);                    // system reserves idx 1; realizes 1 then 0
    TL_EXPECT_EQ(handle_index(g_sys_spawned), 1u);
    TL_EXPECT_TRUE(world_entity_alive(&f.w, ext));
    TL_EXPECT_TRUE(world_entity_alive(&f.w, g_sys_spawned));
    TL_EXPECT_EQ(f.w.pending_fresh, 0u);
    TL_EXPECT_EQ(f.w.entities.slots.count, 2u);
    TL_EXPECT_EQ(f.w.entities.live_count, 2u);
}

TL_TEST(commands_same_window_reuse_spawn_then_destroy_keeps_the_free_list_exact, "core,ecs,commands,edge,fast") {
    // Review 1 D3's interleaving hazard: a reused reservation realized AND destroyed in one
    // window. The deferred pop applies at the window's start, the destroy pushes the slot
    // back - the free list ends with exactly one entry and no aliasing is possible.
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 3u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);

    Entity e = world_spawn(&f.w);
    world_flush(&f.w);
    world_destroy(&f.w, e);
    world_flush(&f.w);
    TL_ASSERT_EQ(f.w.entities.free_list.count, 1u);

    Entity e2 = world_spawn(&f.w);            // reserves the freed slot - a cursor, no byte moves
    TL_EXPECT_EQ(f.w.entities.free_list.count, 1u);
    TL_EXPECT_EQ(f.w.reserved_free, 1u);
    world_destroy(&f.w, e2);                  // same chunk, later record: realize then destroy
    world_flush(&f.w);
    TL_EXPECT_FALSE(world_entity_alive(&f.w, e2));
    TL_ASSERT_EQ(f.w.entities.free_list.count, 1u);
    TL_EXPECT_EQ(f.w.entities.free_list.data[0], handle_index(e));
    TL_EXPECT_EQ(f.w.reserved_free, 0u);

    Entity e3 = world_spawn(&f.w);
    world_flush(&f.w);
    TL_EXPECT_EQ(handle_index(e3), handle_index(e));
    TL_EXPECT_EQ(handle_gen(e3), handle_gen(e) + 2u);   // two destroys, two bumps
    TL_EXPECT_TRUE(world_entity_alive(&f.w, e3));
    TL_EXPECT_EQ(f.w.entities.free_list.count, 0u);
}

TL_TEST_EXPECT_FATAL(commands_destroy_before_realize_is_fatal, "core,ecs,commands,fatal") {
    // Review 1 D4: a reserved-but-unrealized target destroyed by an earlier-applying chunk is
    // loud - a silent no-op would let the later realize resurrect a recorded destroy (the
    // policy ruling is E-4's; today both orders fail loud).
    WorldFixture& f = *wt_fixture(0u);
    if (!world_fixture_init(&f, 3u)) { return; }
    world_fixture_register_std(&f);
    reg_sys(&f.w, sys_first_destroys_target, "fdestroy"_id, PHASE_FIRST, 0, 0);
    world_build_schedule(&f.w);
    g_target = world_spawn(&f.w);   // external reservation; the system's destroy applies first
    ++t->checks;
    wt_tick(&f.w);
}
