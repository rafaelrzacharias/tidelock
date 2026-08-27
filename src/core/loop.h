#pragma once
// ---------------------------------------------------------------------------------------------
// loop.h - Engine: the fixed-step loop, the lockstep tick contract, and the generic interpolation
//   ping-pong.
//
// Spec: docs/FRAME-LOOP.md §0 (the loop), §1 (time - core/time.h), §3 (the end-of-tick barrier),
//   §4 (interpolation), §5 (lockstep integration points), §6 (headless mode), §8.1/§8.3 (this
//   header). docs/ECS.md §3/§4 (run_phase/apply_commands, unchanged - this file only calls them).
// Purpose: `engine_tick_once` is THE lockstep contract function every caller (this loop,
//   tl_driver, the rollback path) drives identically (docs/FRAME-LOOP.md §8.3). `engine_frame` is
//   the real-time wrapper: one wall-clock read, the fixed-step accumulator, and the render half.
// Invariants: `FIXED_DT_SECONDS`/`MAX_STEPS` are startup constants, never changed mid-run
//   (docs/FRAME-LOOP.md §0 - changing dt breaks determinism and alpha). Tick gating is the
//   producer's call (`PRODUCE_WAIT`); Live/Script/Replay never wait. `Engine::platform` may be
//   null (a driver that only calls `engine_tick_once` directly, docs/FRAME-LOOP.md §6) - every
//   `engine_frame` step through it is skipped in that mode; `engine_frame` itself asserts
//   platform is set (a null-platform caller has no real-time frame to drive and should call
//   `engine_tick_once` directly instead).
// Interpolation (docs/FRAME-LOOP.md §4) is GENERIC by design (TODO.md RR-31): `interp_register_pair`
//   is the registration seam a concrete consumer calls once its columns exist. `src/core/transform.h`
//   is now that consumer (`main`, post-round-2-merge) - see its own contract block and
//   `FRAME-LOOP.md` §3's recorded deviation for the per-field-copy-vs-pointer-swap shape RR-28 ruled.
//   Camera state is NOT an interp-pingponged ECS column: Rafael's D1 ruling (render2d lane,
//   2026-08-27) took Camera2D/CameraPrev/CameraFollow off the ECS entirely (registered components'
//   f32 bytes land in registry_hash_all; a camera pan read as a lockstep desync) onto `RenderQueue`,
//   outside this barrier. PRE_RENDER's `alpha` reach (TODO.md RR-30(c)) no longer describes a real
//   gap: `render/extract.cpp`'s `sys_extract`, a registered `SystemFn`, reads `w->render->alpha`
//   (`RenderQueue`, a `World`-owned field) rather than needing an `Engine*` - the consumer solved
//   the reachability problem by carrying the value through `World` state instead of widening
//   `SystemFn`'s signature. This header still returns `alpha` to `engine_frame`'s caller unchanged;
//   whether/where that value is written into `w->render->alpha` is render2d's/the wiring layer's
//   concern, not this file's.
// Determinism: `engine_tick_once` touches nothing outside `World` and `InputFrame`; `Engine`'s
//   other members (Clock, the raw-event ring, the accumulator) are real-time/render-side and
//   never read by sim code. f64/f32 are legal here - core/ is not a sim TU (docs/CPP-SUBSET.md §1).
// Threading: v0 single-threaded (docs/FRAME-LOOP.md §0); one Engine per process/test.
// Includes: core/world.h, core/time.h, core/input.h, foundation/{vmem_arena,ring}.h,
//   platform/platform.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/vmem_arena.h"
#include "foundation/ring.h"
#include "core/world.h"
#include "core/time.h"
#include "core/input.h"
#include "platform/platform.h"

// docs/CANON.md "TICK_HZ" = 60; FIXED_DT_SECONDS = 1/60, render-side f64 only.
constexpr f64 FIXED_DT_SECONDS = 1.0 / 60.0;
// docs/CANON.md "MAX_STEPS" = 5: the spiral-of-death cap (drop time, never spiral).
constexpr u32 MAX_STEPS = 5u;

// docs/PLATFORM.md PlatformConfig's stated default (event_ring_cap_log2 = 10 -> 1024 RawEvents);
// the Live producer drains whatever is available every produce() call (input.h's design note),
// so this is a burst buffer, not a per-tick budget.
enum : u32 { ENGINE_EVENT_RING_CAP = 1024u };

// One registered {current, prev} column pair for the barrier ping-pong (docs/FRAME-LOOP.md §4).
// `current`/`prev` must be same-stride, same-layout columns (the caller's contract - typically a
// component and its "Prev" shadow, e.g. Transform/TransformPrev).
struct InterpPair { ComponentId current; ComponentId prev; };
enum : u32 { INTERP_MAX_PAIRS = 8u };

// The engine composite: World plus everything the real-time loop needs that is NOT sim state
// (docs/FRAME-LOOP.md §1: "there is no other clock anywhere in src/core/src/sim" - Clock lives on
// Engine, one level up from World, for exactly this reason).
struct Engine {
    World world;
    Clock clock;
    const PlatformApi* platform;         // nullable (docs/FRAME-LOOP.md §6 headless mode)
    InputProducer producer;              // input_set_producer, init only
    VMemArena events_arena;              // backs raw_events; unused when platform is null
    RingBuffer<RawEvent> raw_events;
    InputFrame frames[MAX_PEERS];        // this tick's produced frames (input.h's "per-tick frame storage")
    u8 live_mask;
    u8 _pad0[3];
    u32 last_steps;                      // diagnostic: sim steps taken in the last engine_frame call; not hashed
    f64 accumulator;
    f32 last_alpha;                      // diagnostic mirror of engine_frame's return; not hashed
    u32 _pad1;
    InterpPair interp_pairs[INTERP_MAX_PAIRS];
    u32 interp_pair_count;
    u32 _pad2;
    struct Recorder* recorder;           // nullable (core/recorder.h); attached via recorder_attach
    u8 ticked;                           // set by the first engine_tick_once call; closes the
                                          // producer/interp-pair/recorder registration doors
    u8 _pad3[7];
};

// Builds e->world (world_init) and, when `platform` is non-null, e->clock and the raw-event ring.
// Registration (components/systems/the action map/data tables) is app/wiring.cpp's job against
// &e->world afterward (docs/FRAME-LOOP.md §8.2) - this call does none of it. Zero-fills `e` first.
ErrCode engine_init(Engine* e, ArenaRegistry* registry, Scratch* scratch, const VMemApi* os,
                    const PlatformApi* platform, const WorldDesc* desc);

// Sets the input producer (init only; docs/INPUT.md §4's mechanism, wired at docs/FRAME-LOOP.md
// §8.2 step 7 - AFTER world_build_schedule, so the gate is "before the first tick", not world
// sealing). Deviates from docs/INPUT.md §4's World*-shaped signature: see this header's contract
// block - the producer is a per-frame loop concern, never registered/hashed/snapshotted, so it
// lives on Engine, which owns both World and the loop. TL_FATAL once ticking has begun.
void input_set_producer(Engine* e, InputProducer producer);

// Registers one interpolated column pair (docs/FRAME-LOOP.md §4; init only). TL_FATAL once
// ticking has begun, past INTERP_MAX_PAIRS, or on a duplicate `current` id (one prev per current).
void interp_register_pair(Engine* e, ComponentId current, ComponentId prev);

// Attaches a recorder (docs/INPUT.md §9.5); engine_tick_once calls recorder_tick(rec, &e->world,
// frames) once per tick, right after the LAST phase runs (see core/recorder.h's contract block
// for why this is a direct call rather than a registered LAST-phase system). Init only.
void recorder_attach(Engine* e, struct Recorder* rec);

// The end-of-tick barrier's step 3 (docs/FRAME-LOOP.md §3/§8.3): for every registered pair,
// copies `current`'s row bytes into `prev`'s row, entity-for-entity, for every entity `current`'s
// column holds (a pair whose `prev` column lacks an entity `current` has is a caller bug -
// TL_CHECK, since the two columns of one pair are meant to be added/removed together). Pure byte
// copy; never interprets the row's fields. O(live entities in `current`'s column) per pair.
void interp_pingpong(World* w, const InterpPair* pairs, u32 pair_count);

// docs/FRAME-LOOP.md §4 "transform_snap(e) for teleports/camera cuts": sets prev = current for e
// across every registered pair IMMEDIATELY (not waiting for the next barrier), so a teleported
// entity does not appear to slide from its old position this frame. A pair `current` e does not
// carry is silently skipped (not every interpolated pair need be present on every entity).
void interp_snap_entity(World* w, const InterpPair* pairs, u32 pair_count, Entity e);

// THE lockstep contract function (docs/FRAME-LOOP.md §8.3): sets w->input = frames, runs
// FIRST..LAST (each phase's own command barrier already applies via run_phase), then the
// end-of-tick barrier (events swap, interp ping-pong, e->world.state->tick += 1). Called
// identically by engine_frame, tl_driver, and the rollback path (docs/FRAME-LOOP.md §5) - never
// from inside a system.
void engine_tick_once(Engine* e, const InputFrame* frames);

// The §0 loop body for one real frame: the ONE `clock_tick` read, pumps platform events into
// e->raw_events (TL_ASSERT(e->platform != nullptr) - a null-platform caller drives
// engine_tick_once directly instead, docs/FRAME-LOOP.md §6), steps engine_tick_once 0..MAX_STEPS
// times via the fixed-step accumulator (PRODUCE_WAIT stops the loop for this frame without
// consuming accumulator time - "render again, no tick"), drops time at the MAX_STEPS cap
// (slowdown, never a spiral), then runs PRE_RENDER/RENDER and presents when not headless.
// Returns alpha in [0, 1) - render-side only, never sim state (docs/FRAME-LOOP.md §0). Resets
// e->world.scratch at the very end (the "main scratch resets after render" ordering,
// docs/FRAME-LOOP.md §8.3's comment on scratch_reset_all).
f32 engine_frame(Engine* e);

// Releases e->events_arena (when platform was non-null at init); world/registry teardown is the
// caller's (docs/FRAME-LOOP.md §8.2's init order reversed at shutdown).
void engine_shutdown(Engine* e);
