// loop.test.cpp - Engine: accumulator arithmetic, PRODUCE_WAIT, barrier order, headless forced
// ticks (docs/FRAME-LOOP.md §8.4).
#include "runner/tl_test.h"
#include "core/loop.h"
#include "core/recorder.h"
#include "core/producers/script.h"
#include "core/world_test_util.h"
#include "foundation/snapshot.h"

namespace {

// A minimal, fully-stubbed PlatformApi: engine_frame only ever touches clock/events/draw.present
// (the last one gated by is_headless), so everything else stays zeroed - never called.
struct FakeClockCtx { u64 now; u64 freq; };
u64 fake_ticks(void* ctx) { return ((FakeClockCtx*)ctx)->now; }
u64 fake_frequency(void* ctx) { return ((FakeClockCtx*)ctx)->freq; }
u64 fake_wall_unix_ms(void*) { return 0u; }
u32 fake_pump(void*, RingBuffer<RawEvent>*) { return 0u; }
u32 fake_dropped_total(void*) { return 0u; }

PlatformApi make_fake_platform(FakeClockCtx* clock_ctx) {
    PlatformApi api{};
    api.abi_version = PLATFORM_ABI_VERSION;
    api.clock.ctx = clock_ctx;
    api.clock.ticks = fake_ticks;
    api.clock.frequency = fake_frequency;
    api.clock.wall_unix_ms = fake_wall_unix_ms;
    api.events.ctx = nullptr;
    api.events.pump = fake_pump;
    api.events.dropped_total = fake_dropped_total;
    api.is_headless = 1u;
    return api;
}

ProduceResult zero_produce(void*, u64 tick, InputFrame* out, u8* live_mask) {
    for (u32 p = 0; p < MAX_PEERS; ++p) { out[p] = input_zero_frame(); out[p].tick = (u32)tick; }
    *live_mask = 0b1u;
    return PRODUCE_READY;
}

ProduceResult wait_produce(void*, u64, InputFrame*, u8*) { return PRODUCE_WAIT; }

struct EngineFixture { VMemApi api; ArenaRegistry reg; Scratch scratch; Engine e; };

bool engine_fixture_init(EngineFixture* f, u64 seed, const PlatformApi* platform) {
    f->api = test_vmem_api();
    memset(&f->reg, 0, sizeof(f->reg));
    if (scratch_init(&f->scratch, "loop.test.scratch"_id, 32u * 1024u * 1024u, &f->api) != ERR_OK) { return false; }
    WorldDesc d{};
    d.seed = seed;
    return engine_init(&f->e, &f->reg, &f->scratch, &f->api, platform, &d) == ERR_OK;
}

// Folds w->input[0]'s raw action 0 value into every WPos entity's x every tick, so restore/re-tick
// below has hashed state that actually depends on input, not just the tick/seed singleton
// (docs/FRAME-LOOP.md §8.4's "restore-then-retick reproduces the hash trace" row).
void sys_fold_input_into_wpos(World* w) {
    Span<WPos> col = world_column<WPos>(w);
    for (u32 i = 0; i < col.count; ++i) { col.data[i].x += w->input[0].actions[0].value; }
}

}  // namespace

TL_TEST(engine_tick_once_advances_tick_by_one, "core,loop,tick,fast") {
    EngineFixture f;
    TL_ASSERT_TRUE(engine_fixture_init(&f, 1u, nullptr));
    world_build_schedule(&f.e.world);
    TL_ASSERT_EQ(f.e.world.state->tick, 0u);
    InputFrame frames[MAX_PEERS];
    for (u32 p = 0; p < MAX_PEERS; ++p) { frames[p] = input_zero_frame(); }
    engine_tick_once(&f.e, frames);
    TL_EXPECT_EQ(f.e.world.state->tick, 1u);
    engine_tick_once(&f.e, frames);
    engine_tick_once(&f.e, frames);
    TL_EXPECT_EQ(f.e.world.state->tick, 3u);
}

TL_TEST(engine_frame_steps_exactly_one_tick_per_60hz_interval, "core,loop,accumulator,fast") {
    FakeClockCtx clock_ctx{ 0u, 60u };   // 60 ticks-per-second clock frequency
    PlatformApi platform = make_fake_platform(&clock_ctx);
    EngineFixture f;
    TL_ASSERT_TRUE(engine_fixture_init(&f, 1u, &platform));
    world_build_schedule(&f.e.world);
    input_set_producer(&f.e, InputProducer{ nullptr, zero_produce });

    clock_ctx.now = 1u;   // 1/60 second elapsed - exactly one FIXED_DT
    const f32 alpha = engine_frame(&f.e);
    TL_EXPECT_EQ(f.e.last_steps, 1u);
    TL_EXPECT_EQ(f.e.world.state->tick, 1u);
    TL_EXPECT_TRUE(alpha >= 0.0f && alpha < 1.0f);
}

TL_TEST(engine_frame_max_steps_cap_drops_time, "core,loop,accumulator,maxsteps,fast") {
    FakeClockCtx clock_ctx{ 0u, 60u };
    PlatformApi platform = make_fake_platform(&clock_ctx);
    EngineFixture f;
    TL_ASSERT_TRUE(engine_fixture_init(&f, 1u, &platform));
    world_build_schedule(&f.e.world);
    input_set_producer(&f.e, InputProducer{ nullptr, zero_produce });

    // 20 ticks worth of elapsed time in one frame; clock_tick clamps real_dt to 0.25 s (15
    // ticks at 60 Hz) before the accumulator ever sees it (docs/FRAME-LOOP.md §1) - MAX_STEPS (5)
    // still caps it well under that.
    clock_ctx.now = 20u;
    const f32 alpha = engine_frame(&f.e);
    TL_EXPECT_EQ(f.e.last_steps, MAX_STEPS);
    TL_EXPECT_EQ(f.e.world.state->tick, (u64)MAX_STEPS);
    TL_EXPECT_EQ(f.e.accumulator, 0.0);   // dropped, not carried (docs/FRAME-LOOP.md §0)
    TL_EXPECT_EQ(alpha, 0.0f);
}

TL_TEST(engine_frame_produce_wait_renders_without_ticking, "core,loop,producewait,fast") {
    FakeClockCtx clock_ctx{ 0u, 60u };
    PlatformApi platform = make_fake_platform(&clock_ctx);
    EngineFixture f;
    TL_ASSERT_TRUE(engine_fixture_init(&f, 1u, &platform));
    world_build_schedule(&f.e.world);
    input_set_producer(&f.e, InputProducer{ nullptr, wait_produce });

    clock_ctx.now = 5u;   // several ticks worth of elapsed time
    const f32 alpha = engine_frame(&f.e);
    TL_EXPECT_EQ(f.e.last_steps, 0u);
    TL_EXPECT_EQ(f.e.world.state->tick, 0u);
    // Accumulated time is preserved across a WAIT (docs/FRAME-LOOP.md §0: "render again, no tick").
    TL_EXPECT_TRUE(f.e.accumulator > 0.0);
    // ... which loop.h's contract still bounds alpha to [0, 1) against (review round 1 finding 2:
    // an unclamped `accumulator / FIXED_DT_SECONDS` returned alpha >= 1.0 here, since a WAIT can
    // leave accumulator holding several whole ticks' worth of unconsumed time).
    TL_EXPECT_TRUE(alpha >= 0.0f && alpha < 1.0f);
    TL_EXPECT_TRUE(f.e.accumulator >= FIXED_DT_SECONDS);   // the case that actually exercises the clamp
}

// review round 2 defect 2: a plain modulus (accumulator - whole_ticks * FIXED_DT_SECONDS) satisfies
// [0, 1) but CYCLES as accumulator keeps growing during a sustained WAIT stall - a full 0->1 alpha
// sawtooth at the render rate, every frame, with the sim frozen. The fix clamps pending to at most
// one tick's worth instead of taking the remainder, so alpha rises to (just below) 1.0 once and
// PARKS there for the rest of the stall, however long it runs. Two frames deep into the same stall,
// both past the one-tick threshold, must return the identical parked alpha - not two different
// points on a sawtooth.
TL_TEST(engine_frame_produce_wait_alpha_parks_instead_of_cycling, "core,loop,producewait,fast") {
    FakeClockCtx clock_ctx{ 0u, 60u };
    PlatformApi platform = make_fake_platform(&clock_ctx);
    EngineFixture f;
    TL_ASSERT_TRUE(engine_fixture_init(&f, 1u, &platform));
    world_build_schedule(&f.e.world);
    input_set_producer(&f.e, InputProducer{ nullptr, wait_produce });

    clock_ctx.now = 90u;   // 1.5 ticks worth - past the clamp threshold
    const f32 alpha_1 = engine_frame(&f.e);
    TL_EXPECT_EQ(f.e.last_steps, 0u);
    TL_EXPECT_TRUE(alpha_1 >= 0.0f && alpha_1 < 1.0f);

    clock_ctx.now = 600u;   // stall continues for many more ticks' worth of elapsed time
    const f32 alpha_2 = engine_frame(&f.e);
    TL_EXPECT_EQ(f.e.last_steps, 0u);
    TL_EXPECT_TRUE(alpha_2 >= 0.0f && alpha_2 < 1.0f);

    // A modulus would put alpha_2 wherever ((600-90)/60) mod 1 tick lands - not equal to alpha_1
    // in general. The clamp parks both at the same just-below-1.0 value regardless of how much
    // further the stall runs.
    TL_EXPECT_EQ(alpha_1, alpha_2);
}

TL_TEST(engine_barrier_order_event_and_command_visible_next_tick, "core,loop,barrier,fast") {
    EngineFixture f;
    TL_ASSERT_TRUE(engine_fixture_init(&f, 1u, nullptr));
    world_register_component(&f.e.world, &WPos_info);
    world_register_component(&f.e.world, &WVel_info);
    world_register_component(&f.e.world, &WCfg_info);
    world_register_event(&f.e.world, &WEvSpawned_info, 64u);
    world_build_schedule(&f.e.world);

    InputFrame frames[MAX_PEERS];
    for (u32 p = 0; p < MAX_PEERS; ++p) { frames[p] = input_zero_frame(); }

    // No systems registered: emit the event and record the command directly, as if a LAST-phase
    // system had (the phase machinery itself is world.h's, already tested there - this test is
    // about ENGINE's barrier sequencing: does world_events_swap/apply_commands both land before
    // the next tick's phases would run).
    WEvSpawned ev{ handle_make<Entity>(1u, 1u), 7u };
    eq_emit(&f.e.world, ev);
    WCfg cfg{ 999u, 42u, {0,0,0,0} };
    world_singleton_set_cmd(&f.e.world, world_component_id<WCfg>(&f.e.world), &cfg);
    TL_EXPECT_EQ(eq_read<WEvSpawned>(&f.e.world).count, 0u);   // not visible yet this tick

    engine_tick_once(&f.e, frames);

    TL_ASSERT_EQ(eq_read<WEvSpawned>(&f.e.world).count, 1u);
    TL_EXPECT_EQ(eq_read<WEvSpawned>(&f.e.world).data[0].n, 7u);
    TL_EXPECT_EQ(world_singleton<WCfg>(&f.e.world)->mode, 42u);
}

TL_TEST(interp_pingpong_copies_current_into_prev, "core,loop,interp,fast") {
    EngineFixture f;
    TL_ASSERT_TRUE(engine_fixture_init(&f, 1u, nullptr));
    world_register_component(&f.e.world, &WPos_info);
    world_register_component(&f.e.world, &WVel_info);
    world_build_schedule(&f.e.world);

    const ComponentId pos_id = world_component_id<WPos>(&f.e.world);
    const ComponentId vel_id = world_component_id<WVel>(&f.e.world);   // stand-in "prev" column: same stride is all interp_pingpong checks
    interp_register_pair(&f.e, pos_id, vel_id);

    Entity e = world_spawn(&f.e.world);
    world_add(&f.e.world, e, WPos{ 10, 20 });
    world_add(&f.e.world, e, WVel{ 0, 0 });
    world_flush(&f.e.world);

    interp_pingpong(&f.e.world, f.e.interp_pairs, f.e.interp_pair_count);
    WVel* v = world_get<WVel>(&f.e.world, e);
    TL_ASSERT_NOT_NULL(v);
    TL_EXPECT_EQ(v->dx, 10);
    TL_EXPECT_EQ(v->dy, 20);
}

// review round 2 defect 3: "the two columns of a pair are added/removed together" (this file's own
// comment, the stated caller contract) bounds PRESENCE, not DENSE ORDER - column_remove is
// swap-remove, so a column's dense order is a function of ITS OWN add/remove history. Two entities
// added to the pair's columns in opposite order reproduce identical counts (main's
// render/extract.cpp's only guard) while diverging per-index identity - exactly the shape that
// would silently smear one entity's current pose against a different entity's previous one in a
// dense-index consumer. interp_pingpong itself looks up by entity, so it does not corrupt its own
// copy - but it is the one place that can see the divergence, so it must fail loudly here.
TL_TEST_EXPECT_FATAL(interp_pingpong_dense_order_divergence_is_fatal, "core,loop,interp,fatal,fast") {
    EngineFixture f;
    TL_ASSERT_TRUE(engine_fixture_init(&f, 1u, nullptr));
    world_register_component(&f.e.world, &WPos_info);
    world_register_component(&f.e.world, &WVel_info);
    world_build_schedule(&f.e.world);

    const ComponentId pos_id = world_component_id<WPos>(&f.e.world);
    const ComponentId vel_id = world_component_id<WVel>(&f.e.world);
    interp_register_pair(&f.e, pos_id, vel_id);

    Entity a = world_spawn(&f.e.world);
    Entity b = world_spawn(&f.e.world);
    world_add(&f.e.world, a, WPos{ 1, 1 });
    world_add(&f.e.world, b, WPos{ 2, 2 });   // "current" dense order: a, b
    world_add(&f.e.world, b, WVel{ 0, 0 });
    world_add(&f.e.world, a, WVel{ 0, 0 });   // "prev" dense order: b, a - diverged from current
    world_flush(&f.e.world);

    interp_pingpong(&f.e.world, f.e.interp_pairs, f.e.interp_pair_count);   // must TL_FATAL: order mismatch at d=0
}

TL_TEST(interp_snap_entity_updates_one_entity_immediately, "core,loop,interp,snap,fast") {
    EngineFixture f;
    TL_ASSERT_TRUE(engine_fixture_init(&f, 1u, nullptr));
    world_register_component(&f.e.world, &WPos_info);
    world_register_component(&f.e.world, &WVel_info);
    world_build_schedule(&f.e.world);

    const ComponentId pos_id = world_component_id<WPos>(&f.e.world);
    const ComponentId vel_id = world_component_id<WVel>(&f.e.world);   // stand-in "prev" column
    interp_register_pair(&f.e, pos_id, vel_id);

    Entity a = world_spawn(&f.e.world);
    world_add(&f.e.world, a, WPos{ 10, 20 });
    world_add(&f.e.world, a, WVel{ 0, 0 });
    Entity b = world_spawn(&f.e.world);
    world_add(&f.e.world, b, WPos{ 100, 200 });
    world_add(&f.e.world, b, WVel{ 0, 0 });
    world_flush(&f.e.world);

    // A teleport: current (WPos) jumps for `a` only, snapped to prev (WVel) IMMEDIATELY rather
    // than waiting for the next barrier's ping-pong (docs/FRAME-LOOP.md section 4).
    WPos* pa = world_get<WPos>(&f.e.world, a);
    TL_ASSERT_NOT_NULL(pa);
    pa->x = 999;
    pa->y = 888;
    interp_snap_entity(&f.e.world, f.e.interp_pairs, f.e.interp_pair_count, a);

    WVel* va = world_get<WVel>(&f.e.world, a);
    TL_ASSERT_NOT_NULL(va);
    TL_EXPECT_EQ(va->dx, 999);
    TL_EXPECT_EQ(va->dy, 888);

    // b was never snapped - its prev stays at whatever it was, untouched by a's snap.
    WVel* vb = world_get<WVel>(&f.e.world, b);
    TL_ASSERT_NOT_NULL(vb);
    TL_EXPECT_EQ(vb->dx, 0);
    TL_EXPECT_EQ(vb->dy, 0);
}

// review round 2 defect 8: docs/FRAME-LOOP.md §8.4's last row - "restore-then-retick reproduces
// the hash trace" - had no test. tests/foundation/registry.test.cpp proves the arena mechanism
// with a hand-rolled sim_step; nothing drove it through engine_tick_once, the actual barrier.
TL_TEST(engine_restore_then_retick_reproduces_hash_trace, "core,loop,restore,determinism,fast") {
    EngineFixture f;
    TL_ASSERT_TRUE(engine_fixture_init(&f, 42u, nullptr));
    world_register_component(&f.e.world, &WPos_info);
    SystemDesc sd{};
    sd.fn = sys_fold_input_into_wpos;
    sd.label = "fold_input"_id;
    sd.phase = PHASE_UPDATE;
    sd.reads = Span<const ComponentId>{ nullptr, 0 };
    sd.writes = Span<const ComponentId>{ nullptr, 0 };
    sd.before = Span<const NameHash>{ nullptr, 0 };
    sd.after = Span<const NameHash>{ nullptr, 0 };
    sd.flags = 0;
    world_register_system(&f.e.world, &sd);
    world_build_schedule(&f.e.world);
    registry_seal(&f.reg);   // registry_hash_all (recorder_tick's) requires it
    Entity ent = world_spawn(&f.e.world);
    world_add(&f.e.world, ent, WPos{ 0, 0 });
    world_flush(&f.e.world);

    VMemArena sp_arena;
    TL_ASSERT_EQ(vmem_arena_init(&sp_arena, "loop.test.restore.sp"_id, 64u * 1024u, 0u, &f.api), ERR_OK);
    ScriptProducer sp;
    script_producer_init(&sp, &sp_arena, 8u, 0b1u);
    script_hold(&sp, (ActionId)0u, 5, 0u, 10u, 0u);   // down (value 5) for every tick in [0, 10)

    VMemArena ring_arena;
    TL_ASSERT_EQ(vmem_arena_init(&ring_arena, "loop.test.restore.ring"_id, 32u * 1024u * 1024u, 0u, &f.api), ERR_OK);
    SnapshotRing ring;   // ring_init reserves slot_cap_bytes * CONFIRMATION_HORIZON_TICKS (6) slots
    TL_ASSERT_EQ(ring_init(&ring, 4u << 20, &ring_arena), ERR_OK);

    VMemArena rec_arena;
    TL_ASSERT_EQ(vmem_arena_init(&rec_arena, "loop.test.restore.rec"_id, 64u * 1024u, 0u, &f.api), ERR_OK);
    Recorder rec;
    u8 build_id[32] = {};
    u8 fingerprint[32] = {};
    recorder_init(&rec, &rec_arena, 32u, 0u, 42u, 1u, 0b1u, build_id, fingerprint);
    recorder_attach(&f.e, &rec);   // attached once, before any tick - init-only door

    u8 live_mask = 0u;
    InputFrame captured[6][MAX_PEERS];   // ticks 4..9's frames, for the driver-fed re-tick below

    for (u32 i = 0; i < 4u; ++i) {   // ticks 0..3: no snapshot yet
        script_produce(&sp, f.e.world.state->tick, f.e.frames, &live_mask);
        engine_tick_once(&f.e, f.e.frames);
    }

    Snapshot* snap = ring_push(&ring, f.e.world.state->tick);   // snapshot right at tick 4
    TL_ASSERT_TRUE(snap != nullptr);
    TL_ASSERT_EQ(registry_snapshot(&f.reg, snap, f.e.world.state->tick), ERR_OK);

    for (u32 i = 0; i < 6u; ++i) {   // first run: ticks 4..9, via the producer
        script_produce(&sp, f.e.world.state->tick, f.e.frames, &live_mask);
        memcpy(captured[i], f.e.frames, sizeof(captured[i]));
        engine_tick_once(&f.e, f.e.frames);
    }
    TL_ASSERT_EQ(rec.rows.count, 10u);

    TL_ASSERT_EQ(registry_restore(&f.reg, snap), ERR_OK);
    TL_ASSERT_EQ(f.e.world.state->tick, 4u);

    // Second run: re-tick 4..9 with the frames captured above, fed directly - never re-querying
    // the producer, matching FRAME-LOOP.md §8.3's own driver contract ("calls engine_tick_once for
    // each tick up to the present with corrected frames - never from inside a system").
    for (u32 i = 0; i < 6u; ++i) { engine_tick_once(&f.e, captured[i]); }
    TL_ASSERT_EQ(rec.rows.count, 16u);

    for (u32 i = 0; i < 6u; ++i) {
        TL_EXPECT_EQ(rec.rows.data[10u + i].world_hash, rec.rows.data[4u + i].world_hash);
        TL_EXPECT_EQ(memcmp(&rec.rows.data[10u + i].frames[0], &rec.rows.data[4u + i].frames[0], sizeof(InputFrame)), 0);
    }
}

TL_TEST(engine_shutdown_releases_the_event_arena, "core,loop,shutdown,fast") {
    FakeClockCtx clock_ctx{ 0u, 60u };
    PlatformApi platform = make_fake_platform(&clock_ctx);
    EngineFixture f;
    TL_ASSERT_TRUE(engine_fixture_init(&f, 1u, &platform));
    world_build_schedule(&f.e.world);
    engine_shutdown(&f.e);   // review round 1 finding 9: zero call sites before this test
}

TL_TEST_EXPECT_FATAL(input_set_producer_after_first_tick_is_fatal, "core,loop,fatal,fast") {
    EngineFixture f;
    if (!engine_fixture_init(&f, 1u, nullptr)) { return; }
    world_build_schedule(&f.e.world);
    InputFrame frames[MAX_PEERS];
    for (u32 p = 0; p < MAX_PEERS; ++p) { frames[p] = input_zero_frame(); }
    engine_tick_once(&f.e, frames);
    ++t->checks;
    input_set_producer(&f.e, InputProducer{ nullptr, zero_produce });   // must TL_FATAL: init-only door
}

TL_TEST_EXPECT_FATAL(interp_register_pair_after_first_tick_is_fatal, "core,loop,fatal,fast") {
    EngineFixture f;
    if (!engine_fixture_init(&f, 1u, nullptr)) { return; }
    world_register_component(&f.e.world, &WPos_info);
    world_register_component(&f.e.world, &WVel_info);
    world_build_schedule(&f.e.world);
    InputFrame frames[MAX_PEERS];
    for (u32 p = 0; p < MAX_PEERS; ++p) { frames[p] = input_zero_frame(); }
    engine_tick_once(&f.e, frames);
    const ComponentId pos_id = world_component_id<WPos>(&f.e.world);
    const ComponentId vel_id = world_component_id<WVel>(&f.e.world);
    ++t->checks;
    interp_register_pair(&f.e, pos_id, vel_id);   // must TL_FATAL: init-only door
}

TL_TEST_EXPECT_FATAL(recorder_attach_after_first_tick_is_fatal, "core,loop,fatal,fast") {
    EngineFixture f;
    if (!engine_fixture_init(&f, 1u, nullptr)) { return; }
    world_build_schedule(&f.e.world);
    InputFrame frames[MAX_PEERS];
    for (u32 p = 0; p < MAX_PEERS; ++p) { frames[p] = input_zero_frame(); }
    engine_tick_once(&f.e, frames);
    Recorder rec{};
    ++t->checks;
    recorder_attach(&f.e, &rec);   // must TL_FATAL: init-only door
}
