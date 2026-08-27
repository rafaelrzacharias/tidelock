// loop.test.cpp - Engine: accumulator arithmetic, PRODUCE_WAIT, barrier order, headless forced
// ticks (docs/FRAME-LOOP.md §8.4).
#include "runner/tl_test.h"
#include "core/loop.h"
#include "core/recorder.h"
#include "core/world_test_util.h"

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
