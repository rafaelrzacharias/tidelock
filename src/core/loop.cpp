// loop.cpp - Engine: init/shutdown, the input-producer/interp-pair registration doors,
//   engine_tick_once (the lockstep contract), engine_frame (the real-time wrapper).
// See loop.h for the full contract.
#include "core/loop.h"
#include "core/column.h"
#include "core/recorder.h"
#include "foundation/hash.h"
#include <string.h>

ErrCode engine_init(Engine* e, ArenaRegistry* registry, Scratch* scratch, const VMemApi* os,
                    const PlatformApi* platform, const WorldDesc* desc) {
    memset(e, 0, sizeof(Engine));
    const ErrCode err = world_init(&e->world, registry, scratch, os, desc);
    if (err != ERR_OK) { return err; }
    e->platform = platform;
    if (platform != nullptr) {
        clock_init(&e->clock, &platform->clock);
        const ErrCode ev_err = vmem_arena_init(&e->events_arena, "engine.events"_id,
            (u64)ENGINE_EVENT_RING_CAP * sizeof(RawEvent), 0u, os);
        if (ev_err != ERR_OK) { return ev_err; }
        ring_init(&e->raw_events, &e->events_arena, ENGINE_EVENT_RING_CAP, true);
    }
    return ERR_OK;
}

void input_set_producer(Engine* e, InputProducer producer) {
    if (e->ticked != 0u) { TL_FATAL("input_set_producer: called after the first tick (init only)"); }
    e->producer = producer;
}

void recorder_attach(Engine* e, Recorder* rec) {
    if (e->ticked != 0u) { TL_FATAL("recorder_attach: called after the first tick (init only)"); }
    e->recorder = rec;
}

void engine_tick_once(Engine* e, const InputFrame* frames) {
    World* w = &e->world;
    e->ticked = 1u;
    if (w->guard != nullptr) { guard_tick_begin(w->guard, w->registry); }
    w->input = frames;
    for (Phase p = PHASE_FIRST; p <= PHASE_LAST; p = (Phase)((u8)p + 1u)) {
        run_phase(w, p);
    }
    // LAST has run (registered LAST-phase systems, e.g. net_send once netcode lands). The
    // recorder is a direct call, not a registered system (core/recorder.h's contract block: a
    // SystemFn cannot reach Engine-level state). Then the end-of-tick barrier (docs/FRAME-LOOP.md
    // §3): events swap, interp ping-pong. Worker scratch reset (docs/JOBS.md) does not exist in
    // v0 (single-threaded) - the MAIN scratch resets after render instead (engine_frame, matching
    // docs/FRAME-LOOP.md §8.3's own comment).
    if (e->recorder != nullptr) { recorder_tick(e->recorder, w, frames); }
    world_events_swap(w);
    interp_pingpong(w, e->interp_pairs, e->interp_pair_count);
    w->state->tick += 1u;
    if (w->guard != nullptr) { guard_tick_end(w->guard, w->registry); }
}

f32 engine_frame(Engine* e) {
    TL_ASSERT(e->platform != nullptr);
    const f64 real_dt = clock_tick(&e->clock);
    e->platform->events.pump(e->platform->events.ctx, &e->raw_events);
    e->accumulator += real_dt;
    u32 steps = 0u;
    while (e->accumulator >= FIXED_DT_SECONDS && steps < MAX_STEPS) {
        const ProduceResult r = e->producer.produce(e->producer.ctx, e->world.state->tick, e->frames, &e->live_mask);
        if (r == PRODUCE_WAIT) { break; }   // lockstep: not confirmed - render again, no tick
        engine_tick_once(e, e->frames);
        e->accumulator -= FIXED_DT_SECONDS;
        steps += 1u;
    }
    if (steps == MAX_STEPS) { e->accumulator = 0.0; }   // spiral-of-death cap: drop time
    e->last_steps = steps;
    const f32 alpha = (f32)(e->accumulator / FIXED_DT_SECONDS);
    e->last_alpha = alpha;
    run_phase(&e->world, PHASE_PRE_RENDER);
    run_phase(&e->world, PHASE_RENDER);
    if (e->platform->is_headless == 0u) {
        e->platform->draw.present(e->platform->draw.ctx);
    }
    scratch_reset(e->world.scratch);
    return alpha;
}

void engine_shutdown(Engine* e) {
    if (e->platform != nullptr) {
        e->events_arena.os->release(e->events_arena.os->ctx, e->events_arena.base, e->events_arena.reserved);
    }
}
