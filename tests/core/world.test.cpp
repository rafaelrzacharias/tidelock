// world.h - registration doors and their registry effects, typed access, singletons, the
// reflection hash, and the snapshot -> restore -> post_restore round trip.
// Spec: docs/ECS.md §2/§7/§10.3; docs/MEMORY.md §5/§8.8 (the restore half). Rubric: TESTING §7.
#include "world_test_util.h"
#include "foundation/snapshot.h"

TL_TEST(world_registration_wires_the_registry_in_contract_order, "core,ecs,world,smoke,fast") {
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 7u));
    // world_init registers: singletons + the entity slotmap's four columns.
    TL_ASSERT_EQ(f.reg.count, 5u);
    TL_EXPECT_EQ(f.reg.e[0].id, "world.singletons"_id);
    TL_EXPECT_EQ(f.reg.e[0].flags, (u32)(ARENA_HASHED | ARENA_SNAPSHOT));
    TL_EXPECT_EQ(f.reg.e[1].id, "world.entities.slots"_id);
    TL_EXPECT_EQ(f.reg.e[4].id, "world.entities.live"_id);
    TL_EXPECT_EQ(f.reg.e[2].flags, (u32)(ARENA_HASHED | ARENA_SNAPSHOT | ARENA_GROWS_AT_BARRIER));

    // A column adds three entries (dense/entities hashed, pages snapshot-only + barrier); a
    // singleton adds one (hashed, no growth flag).
    const ComponentId pos = world_register_component(&f.w, &WPos_info);
    TL_EXPECT_EQ(pos, (ComponentId)0);
    TL_ASSERT_EQ(f.reg.count, 8u);
    TL_EXPECT_EQ(f.reg.e[5].flags, (u32)(ARENA_HASHED | ARENA_SNAPSHOT | ARENA_GROWS_AT_BARRIER));
    TL_EXPECT_EQ(f.reg.e[6].flags, (u32)(ARENA_HASHED | ARENA_SNAPSHOT | ARENA_GROWS_AT_BARRIER));
    TL_EXPECT_EQ(f.reg.e[7].flags, (u32)(ARENA_SNAPSHOT | ARENA_GROWS_AT_BARRIER));
    const ComponentId vel = world_register_component(&f.w, &WVel_info);
    const ComponentId cfg = world_register_component(&f.w, &WCfg_info);
    TL_EXPECT_EQ(vel, (ComponentId)1);
    TL_EXPECT_EQ(cfg, (ComponentId)2);
    TL_ASSERT_EQ(f.reg.count, 12u);
    TL_EXPECT_EQ(f.reg.e[11].flags, (u32)(ARENA_HASHED | ARENA_SNAPSHOT));

    TL_EXPECT_EQ(world_find_component(&f.w, "WPos"_id), 0u);
    TL_EXPECT_EQ(world_find_component(&f.w, "WGhost"_id), (u32)MAX_COMPONENT_TYPES);
    TL_EXPECT_EQ(world_component_id<WVel>(&f.w), (ComponentId)1);

    // tick + seed live behind `state` in the singleton arena, inside its hashed extent.
    TL_EXPECT_EQ(f.w.state->seed, 7u);
    TL_EXPECT_EQ(f.w.state->tick, 0u);
    TL_EXPECT_TRUE((const u8*)f.w.state == f.w.sing_arena.base);
    TL_EXPECT_EQ(f.w.sing_arena.used, (u64)sizeof(WorldTickState));
}

TL_TEST(world_singleton_access_and_empty_columns, "core,ecs,world,fast") {
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 1u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);

    // A singleton is present and zeroed from registration; writes persist in place.
    WCfg* cfg = world_singleton<WCfg>(&f.w);
    TL_ASSERT_NOT_NULL(cfg);
    TL_EXPECT_EQ(cfg->gravity, 0u);
    cfg->gravity = 981u;
    cfg->mode = 3u;
    TL_EXPECT_EQ(world_singleton<WCfg>(&f.w)->gravity, 981u);

    // Empty columns give empty spans; probes on absent/null entities are null, assert-free.
    TL_EXPECT_EQ(world_column<WPos>(&f.w).count, 0u);
    TL_EXPECT_EQ(world_entities<WPos>(&f.w).count, 0u);
    TL_EXPECT_NULL(world_get<WPos>(&f.w, Entity{ 0 }));
    TL_EXPECT_NULL(world_get<WPos>(&f.w, handle_make<Entity>(12u, 3u)));
    TL_EXPECT_FALSE(world_entity_alive(&f.w, handle_make<Entity>(12u, 3u)));
}

TL_TEST(world_reflection_hash_pins_the_registration_set, "core,ecs,world,determinism,fast") {
    // Same registrations in two worlds -> equal; an extra event type -> different; component
    // order is part of the fold (a, b) != (b, a).
    WorldFixture& a = *wt_fixture(0u);
    WorldFixture& b = *wt_fixture(1u);
    TL_ASSERT_TRUE(world_fixture_init(&a, 1u));
    TL_ASSERT_TRUE(world_fixture_init(&b, 2u));
    world_fixture_register_std(&a);
    world_fixture_register_std(&b);
    TL_EXPECT_EQ(world_reflection_hash(&a.w), world_reflection_hash(&b.w));

    WorldFixture& c = *wt_fixture(2u);
    TL_ASSERT_TRUE(world_fixture_init(&c, 1u));
    world_fixture_register_std(&c);
    world_register_event(&c.w, &WPos_info, 8u);   // one more event table
    TL_EXPECT_NE(world_reflection_hash(&a.w), world_reflection_hash(&c.w));

    WorldFixture& d = *wt_fixture(3u);
    TL_ASSERT_TRUE(world_fixture_init(&d, 1u));
    world_register_component(&d.w, &WVel_info);   // swapped component order vs _std
    world_register_component(&d.w, &WPos_info);
    world_register_component(&d.w, &WCfg_info);
    world_register_event(&d.w, &WEvSpawned_info, 64u);
    TL_EXPECT_NE(world_reflection_hash(&a.w), world_reflection_hash(&d.w));
}

TL_TEST(world_snapshot_restore_post_restore_round_trip, "core,ecs,world,determinism,fast") {
    // The MEMORY.md §8.8 shape for the ECS half: capture, mutate structurally, restore,
    // world_post_restore -> derived counts and the world hash equal the captured state's.
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 42u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);

    // Build some state: three entities, mixed components, one destroy (so the free list and a
    // zeroed tail exist), then capture.
    Entity e0 = world_spawn(&f.w);
    Entity e1 = world_spawn(&f.w);
    Entity e2 = world_spawn(&f.w);
    WPos p = { 10, 20 };
    WVel v = { 1, -1 };
    world_add<WPos>(&f.w, e0, p);
    world_add<WPos>(&f.w, e1, p);
    world_add<WVel>(&f.w, e1, v);
    world_add<WPos>(&f.w, e2, p);
    world_flush(&f.w);
    world_destroy(&f.w, e2);
    world_flush(&f.w);
    world_singleton<WCfg>(&f.w)->gravity = 5u;
    f.w.state->tick = 9u;

    registry_seal(&f.reg);
    u64 before_per[MAX_ARENAS];
    const u64 hash_before = registry_hash_all(&f.reg, before_per);

    VMemArena blob_arena;
    TL_ASSERT_EQ(vmem_arena_init(&blob_arena, "wt.blob"_id, 64u * 1024u * 1024u, 0u, &f.api), ERR_OK);
    Snapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.blob_cap = 64u * 1024u * 1024u;
    snap.blob = (u8*)arena_push(&blob_arena, snap.blob_cap, 64u);
    TL_ASSERT_EQ(registry_snapshot(&f.reg, &snap, f.w.state->tick), ERR_OK);

    // Mutate structure past the capture: new entity (reusing e2's slot), remove, field writes.
    Entity e3 = world_spawn(&f.w);
    world_add<WVel>(&f.w, e3, v);
    world_remove(&f.w, e1, world_component_id<WVel>(&f.w));
    world_flush(&f.w);
    world_singleton<WCfg>(&f.w)->gravity = 77u;
    f.w.state->tick = 30u;
    TL_EXPECT_NE(registry_hash_all(&f.reg, before_per), hash_before);

    TL_ASSERT_EQ(registry_restore(&f.reg, &snap), ERR_OK);
    world_post_restore(&f.w);

    // Derived counts match the captured world: 2 live (e0, e1), one freed slot, no pending.
    TL_EXPECT_EQ(f.w.entities.live_count, 2u);
    TL_EXPECT_EQ(f.w.entities.slots.count, 3u);
    TL_EXPECT_EQ(f.w.entities.free_list.count, 1u);
    TL_EXPECT_EQ(f.w.entities.free_list.data[0], handle_index(e2));
    TL_EXPECT_EQ(f.w.pending_fresh, 0u);
    TL_EXPECT_TRUE(world_entity_alive(&f.w, e0));
    TL_EXPECT_TRUE(world_entity_alive(&f.w, e1));
    TL_EXPECT_FALSE(world_entity_alive(&f.w, e3));   // post-capture entity is gone
    TL_EXPECT_EQ(world_column<WPos>(&f.w).count, 2u);
    TL_EXPECT_EQ(world_column<WVel>(&f.w).count, 1u);
    TL_ASSERT_NOT_NULL(world_get<WVel>(&f.w, e1));   // the removed row is back
    TL_EXPECT_EQ(world_singleton<WCfg>(&f.w)->gravity, 5u);
    TL_EXPECT_EQ(f.w.state->tick, 9u);

    // The whole registered set hashes exactly as at capture.
    u64 after_per[MAX_ARENAS];
    TL_EXPECT_EQ(registry_hash_all(&f.reg, after_per), hash_before);

    // And the restored world is LIVE: the freed slot reuses with the next generation.
    Entity e4 = world_spawn(&f.w);
    world_flush(&f.w);
    TL_EXPECT_EQ(handle_index(e4), handle_index(e2));
    TL_EXPECT_EQ(handle_gen(e4), handle_gen(e2) + 1u);
    TL_EXPECT_TRUE(world_entity_alive(&f.w, e4));
}

TL_TEST_EXPECT_FATAL(world_duplicate_component_is_fatal, "core,ecs,world,fatal") {
    WorldFixture& f = *wt_fixture(0u);
    if (!world_fixture_init(&f, 1u)) { return; }
    world_register_component(&f.w, &WPos_info);
    ++t->checks;
    world_register_component(&f.w, &WPos_info);   // must TL_FATAL: duplicate name
}

TL_TEST_EXPECT_FATAL(world_register_after_seal_is_fatal, "core,ecs,world,fatal") {
    WorldFixture& f = *wt_fixture(0u);
    if (!world_fixture_init(&f, 1u)) { return; }
    world_register_component(&f.w, &WPos_info);
    world_build_schedule(&f.w);
    ++t->checks;
    world_register_component(&f.w, &WVel_info);   // must TL_CHECK-fatal: sealed
}

TL_TEST_EXPECT_FATAL(world_column_of_a_singleton_is_fatal, "core,ecs,world,fatal") {
    WorldFixture& f = *wt_fixture(0u);
    if (!world_fixture_init(&f, 1u)) { return; }
    world_register_component(&f.w, &WCfg_info);
    ++t->checks;
    (void)world_column<WCfg>(&f.w);   // must TL_CHECK-fatal: singletons have no column
}

TL_TEST(world_snapshot_with_pending_reservation_restores_consistently, "core,ecs,world,determinism,fast") {
    // Review 1 D3: a reservation is a cursor, never a byte move, so a snapshot captured while
    // one is outstanding (FRAME-LOOP.md §2's own LAST-system capture shape) restores to a
    // state whose derived free count agrees with the bytes - and the next spawn cannot alias
    // a live entity.
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 7u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);
    registry_seal(&f.reg);

    Entity e0 = world_spawn(&f.w);
    Entity e1 = world_spawn(&f.w);
    world_flush(&f.w);
    world_destroy(&f.w, e1);
    world_flush(&f.w);
    TL_ASSERT_EQ(f.w.entities.free_list.count, 1u);

    Entity pend = world_spawn(&f.w);   // outstanding reservation of e1's slot - no flush
    TL_EXPECT_EQ(handle_index(pend), handle_index(e1));
    TL_EXPECT_EQ(f.w.reserved_free, 1u);
    TL_EXPECT_EQ(f.w.entities.free_list.count, 1u);   // the bytes have not moved

    VMemArena blob_arena;
    TL_ASSERT_EQ(vmem_arena_init(&blob_arena, "wp.blob"_id, 64u * 1024u * 1024u, 0u, &f.api), ERR_OK);
    Snapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.blob_cap = 64u * 1024u * 1024u;
    snap.blob = (u8*)arena_push(&blob_arena, snap.blob_cap, 64u);
    TL_ASSERT_EQ(registry_snapshot(&f.reg, &snap, f.w.state->tick), ERR_OK);

    commands_discard(&f.w);   // the rollback order: discard, then restore
    TL_ASSERT_EQ(registry_restore(&f.reg, &snap), ERR_OK);
    world_post_restore(&f.w);
    TL_EXPECT_EQ(f.w.entities.free_list.count, 1u);
    TL_EXPECT_EQ(f.w.entities.free_list.data[0], handle_index(e1));
    TL_EXPECT_EQ(f.w.entities.live_count, 1u);
    TL_EXPECT_EQ(f.w.reserved_free, 0u);

    // The regenerated spawn reuses e1's slot at its current generation - never e0's bits.
    Entity e2 = world_spawn(&f.w);
    world_flush(&f.w);
    TL_EXPECT_EQ(handle_index(e2), handle_index(e1));
    TL_EXPECT_NE(e2.bits, e0.bits);
    TL_EXPECT_TRUE(world_entity_alive(&f.w, e2));
    TL_EXPECT_TRUE(world_entity_alive(&f.w, e0));
    TL_EXPECT_EQ(f.w.entities.free_list.count, 0u);
}
