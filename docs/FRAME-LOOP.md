# Frame loop, time, phases, interpolation (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §7. Carries foundry CORE §3, D7, D10,
> D11's knobs, FOUNDRY-API §2 into C++ + fixed point. **§8 (loop.h/.cpp, time.h, interp.cpp)
> implemented by w3-loop-input, 2026-08-26/27** — declarations for the interpolation ping-pong
> live in `loop.h` (no separate `interp.h`; `interp.cpp` is a pure implementation split, matching
> this doc's own §8.1 file list, which never named one). Three filed, non-blocking ruling
> requests from that lane are in `TODO.md` (`RR-24`..`RR-26`): the generic (not `Transform`-
> named) interpolation registration API, the LAST-phase recorder as a direct call rather than a
> registered system (`SystemFn` has no path to `Engine`-level state), and the same gap for §0's
> render-side `alpha`.
> **Owns:** `src/core/loop.h`, `time.h`, `phase.h`; `app/main.cpp` instantiates it.

---

## 0. The loop (DECIDED)

```cpp
for (;;) {
    f64 real_dt = clock_tick(&clock);                       // the ONLY wall-clock read (platform seam)
    platform->pump_events(platform, &raw_events);           // raw input → engine buffer; window/quit → engine callbacks
    accumulator += real_dt;
    u32 steps = 0;
    while (accumulator >= FIXED_DT_SECONDS && steps < MAX_STEPS) {
        ProduceResult r = input_produce(&producer, world.tick, frames);  // Live/Script/Replay/Network
        if (r == PRODUCE_WAIT) break;                        // lockstep: inputs not confirmed → render again, no tick
        world.input = frames;                                // InputFrame[MAX_PEERS] for this tick
        run_phases_sim(&world);                              // FIRST … LAST; barrier after each phase
        barrier_end_of_tick(&world);                         // flush commands, swap+clear events, prev←current ping-pong
        world.tick += 1;
        accumulator -= FIXED_DT_SECONDS; steps += 1;
    }
    if (steps == MAX_STEPS) accumulator = 0;                 // spiral-of-death cap: DROP time (slowdown, not spiral)
    f64 pending = accumulator;                                // PRODUCE_WAIT can leave several whole ticks stuck in accumulator
    while (pending >= FIXED_DT_SECONDS) pending -= FIXED_DT_SECONDS;  // fractional remainder only - subtraction, not a division:
                                                               // no multiply for -ffp-contract to fuse into an FMA (cross-ISA bit divergence)
    f32 alpha = (f32)(pending / FIXED_DT_SECONDS);           // render-side float, fine
    if (alpha >= 1.0f) alpha = 0.0f;                          // the f64->f32 downcast can still round exactly to 1.0 near the boundary
    run_phases_render(&world, alpha);                        // PRE_RENDER, RENDER — reads sim, never writes
    render_present(&world);                                  // sort + batch + SDL present
    scratch_reset(&world.scratch_main);
}
```

- **The sim sees `H` and `tick`, never `real_dt`** (`FX-PALETTE.md` §2). `accumulator`/`alpha`
  are wall-clock-derived floats that exist only in `app/` and render.
- `FIXED_DT_SECONDS = 1/60` and `MAX_STEPS = 5` are startup constants, never changed mid-run
  (changing dt breaks determinism and `alpha`).
- **Tick gating is the input producer's call.** `PRODUCE_WAIT` is how lockstep stalls
  ("waiting for players…" — which per-peer adaptive delay makes rare, `NETCODE.md` §7.4); Live/
  Script/Replay never wait.
- v0 is **single-threaded**. The job system (`JOBS.md`) fans systems/Alloy passes across workers
  *within* a phase later; a pipelined sim thread is the precision ladder's rung 5 escape hatch,
  not a plan.
- `FreezeSimulation(n)` (hit-stop: consume time, skip sim, keep rendering) is reserved; it is a
  render-side effect on the accumulator, not a sim concept.

---

## 1. Time (DECIDED)

`time.h`: `Clock` over the platform hires timer (`PLATFORM.md` §3), `clock_tick → f64 seconds`
clamped to `[0, 0.25]` (a debugger pause must not spiral). `world.tick: u64` lives in the
registered singleton arena with the seed (it is state — saves and snapshots carry it). Tick →
seconds for display is `tick / 60` computed render/Luau-side. There is no other clock anywhere
in `src/core`/`src/sim` (symbol gate).

**Tick width rule (applies to every doc):** the tick is `u64` in world state, snapshots, saves,
checkpoints, the chain, the archive log and the recorder. `InputFrame.tick` is the **low 32 bits**
of that `u64` (a 76-byte frame, `INPUT.md` §1); every container of frames (the packet header, the
archive segment, `RecordedInput`) carries a `u64 base_tick`, and a frame's full tick is
`base_tick + (frame.tick − u32(base_tick))` with the difference taken mod 2³². A `Persistent`
world can therefore run past 828 simulated days without any wire or file format change.

---

## 2. Phases (DECIDED — position-named, closed)

| Phase | Typical content (convention, not enforced) |
|---|---|
| `FIRST` | net receive (confirmed frames already in `world.input`), input drain + action-map (Luau sim VM), tick bookkeeping |
| `PRE_UPDATE` | spawners, timers/cooldowns, game systems that produce MoveIntents + sim edit commands |
| `UPDATE` | `alloy_step(world.sim, &edit_commands, world.tick)` — the five passes; then bulk gameplay |
| `POST_UPDATE` | reactions to sim events (bridge Alloy ring → `EventQueue`s), damage/despawn marking, transform/hierarchy resolve into `current` |
| `LAST` | determinism checkpoint: per-arena hash; net send; snapshot ring push; record→replay log |
| `PRE_RENDER` | extract: fx → float, interpolation, camera, culling, sort-key prep |
| `RENDER` | all draw systems submit; ImGui (dev) |

Rules: every phase boundary is a command-buffer barrier; intra-phase order is the topo-sorted
registration order (`ECS.md` §3); `FIRST` and `LAST` are the only phases the netcode touches;
`LAST` is where the hash is taken *after* all writes of the tick. Reserved (append-only): `INIT`
(one-shot world build), `FRAME_END`.

**Three layers, not one enum:** frame phases (temporal, above) · render-target/compositing layers
(spatial: world / UI / debug, each with a target — `RENDER2D.md` §4) · the host (fullscreen game vs
editor viewport). Post-processing, viewports, split-screen and multi-window are layer/host
concerns and never phases. The game runs identically regardless of host.

---

## 3. The end-of-tick barrier (DECIDED — order matters, stated once)

1. Apply command buffers (chunk order) — the `GROWS_AT_BARRIER` window.
2. Swap event buffers; clear the new write side (scratch reset covers it).
3. Ping-pong `prev ← current` for every interpolated column (pointer swap, O(1)).
4. Reset worker scratch.

Snapshot push (for the rollback ring) happens in `LAST`, *before* this barrier, so a snapshot is
a fully-written tick. A restore (`MEMORY.md` §5) runs this barrier's step 3 with the snap bit set.

---

## 4. Interpolation (DECIDED — D10, fixed-point edition)

- **Render-only, never written back.** The sim is bit-identical regardless of render rate.
- **Double-buffer the resolved world transform** per entity: `Transform` (fx) is `current`;
  `TransformPrev` (fx, a hidden engine column) is `prev`, ping-ponged at the barrier. Hierarchy is
  resolved **once per tick** in `POST_UPDATE` into `current` (a `Parent` pass, when a consumer
  appears) — never per render frame.
- **Extract** (`PRE_RENDER`): one flat pass converts `prev`/`current` to float (`to_f32`) and
  lerps by `alpha` into the render packet; rotation lerps shortest-arc in turns (a `q_t` → float
  conversion then float math). One SIMD-friendly pass; exact for translation, imperceptibly
  approximate for rotating hierarchies.
- **Snap bit** on the `Transform`: spawn/teleport sets `prev = current` for one frame; the engine
  auto-snaps newly realized entities; `transform_snap(e)` for teleports/camera cuts.
- **Camera** is a render-side float component interpolated the same way, then display-snapped if
  the view has pixel-snap on (`RENDER2D.md` §2).
- Scope: transforms + camera only. Other continuous values (tints, fades) are the game's concern,
  render-side.
- Cubic Hermite (needs velocity) reserved.

---

## 5. Lockstep integration points (DECIDED — the whole netcode touches the loop here and nowhere else)

| Point | What |
|---|---|
| `input_produce` | the Network producer returns confirmed `InputFrame[MAX_PEERS]` or `WAIT` |
| `FIRST` system `net_receive` | drains ENet, feeds the sequencer; no sim access |
| `LAST` system `net_send` | sends this peer's frames + hash digests; reads the per-arena hashes computed by the checkpoint system that runs `before` it |
| rollback | `registry_restore` to a ring slot + re-run ticks with the corrected frames — driven by the producer, not by the loop |

The loop shape does not change between single-player, replay, headless test, and 8-peer lockstep.

---

## 6. Headless mode

The same loop with the headless platform impl (`PLATFORM.md` §6): no window, `pump_events` is a
no-op, the `Script` producer feeds frames, `run_phases_render` runs with a null render queue (or
is skipped entirely for sim-only tests), and the loop runs as fast as the CPU allows
(`accumulator` is forced to `FIXED_DT` per iteration). This is `tests/` and Hovel.

**Recorded deviation (w3-loop-input, 2026-08-27, review round 1 finding 11, `TODO.md` RR-27):**
the forced-`FIXED_DT` accumulator is NOT implemented. `PlatformApi::is_headless` (`PLATFORM.md`
§9.2) is the only headless signal `engine_frame` can see, and it already means something narrower
and already load-bearing: "skip `present()`" - `tests/core/loop.test.cpp`'s own accumulator suite
(`engine_frame_max_steps_cap_drops_time` et al.) sets `is_headless = 1` on its fake platform
specifically so it can drive `engine_frame` with a fake clock and assert on the REAL measured
`real_dt`-based stepping (steps-per-frame, the `MAX_STEPS` cap). Gating this section's
forced-`FIXED_DT` behavior on that same flag was tried and reverted: it made every accumulator
test headless mode already uses see exactly one simulated tick per call regardless of the fake
clock's reading, breaking `engine_frame_max_steps_cap_drops_time` (measured, not argued - the row
failed on the test's own assertion). Closing this needs a mechanism distinct from `is_headless` -
an `Engine`-level opt-in (e.g. a `force_fixed_dt` member alongside `producer`/`interp_pairs`,
set by the driver that wants it) rather than a blanket per-platform behavior every headless caller
inherits whether it wants it or not - which is a design decision, not a bug fix.

---

## 7. Rulings (closed 2026-08-22 — nothing open)

- **R-1 `World` holds `InputFrame[MAX_PEERS]` per tick plus a `PeerSlots` singleton**
  (`live_mask`, local slot, slot→player id). The action-map runs per live slot; the game's script
  reads `PeerSlots.local` to know which slot is "me". One merged frame would lose per-peer
  substitution and per-peer commitment windows.
- **R-2 `alloy_step` is one `UPDATE` system, permanently.** Game code never interleaves between
  passes: anything that must happen "between pass 3 and pass 4" is a command applied at that
  pass boundary inside Alloy (the edit channel already has pass-boundary application semantics,
  `ALLOY.md` §0.4). Splitting the step into five systems would expose pass internals as a seam
  and is rejected, not deferred.

## 8. Implementation specification

### 8.1 Files

`core/loop.h/.cpp` (`Engine`, `engine_init`, `engine_tick_once`, `engine_frame`, `engine_shutdown`),
`core/time.h` (`Clock`), `core/phase.h` (the enum + names), `core/interp.cpp` (the `prev ← current`
ping-pong + snap), `app/main.cpp` (wiring: module registration order, the loop call, CLI flags),
`app/wiring.cpp` (the one registration file: arenas, components, systems, events, actions, in
order).

### 8.2 Init order (`app/wiring.cpp` — this order is the lockstep contract)

1. `platform = platform_sdl3_init(cfg)` (or headless). Vendor allocator hooks are set *before* this
   (`mem_pool` created first from a bootstrap `VMemArena`).
2. Arenas: create every `VMemArena` from the reserve table; `registry_add` in this fixed order:
   `world_singletons` (tick, seed, `PeerSlots`), `entities`, then one entry per component column as
   registered in step 4, then Alloy's pools (`alloy_register_arenas`), then `data_tables`. Scratch
   arenas and the event arena are created but not registered.
3. Interner, cvars, log sinks, profiler.
4. Components: engine components (`Transform`, `TransformPrev`, `Sprite`, `PeerSlots`, …) then
   the Luau sim scripts' `ecs.component` declarations (script init phase runs here, in the sim
   VM). Camera state is NOT registered here: Rafael's D1 ruling (render2d lane, 2026-08-27) took
   `Camera2D`/`CameraPrev`/`CameraFollow` off the ECS entirely - a registered component's f32
   bytes land in `registry_hash_all`, and a camera pan read as a lockstep desync. Camera state
   lives on `RenderQueue` (`camera[MAX_VIEWS]`/`camera_prev[MAX_VIEWS]`/
   `camera_follow[MAX_VIEWS]`/`camera_count`), render2d's own doc and file, outside this barrier.
5. Events: engine event types, then Luau-declared.
6. Systems: engine systems per phase (input fold bookkeeping, `alloy_step`, Alloy→event bridge,
   transform resolve, checkpoint, recorder, `net_receive`/`net_send` when present, render
   systems), then Luau `ecs.system` registrations. `schedule_build`.
7. Action map from the Luau input script; `input_set_producer`.
8. Data tables compiled (`ASSETS-AND-DATA.md` §3); `alloy_init(tables, world_desc)`.
9. `registry_seal`; `session_fingerprint` computed; `TL_LOG_INFO` both fingerprints.
10. Snapshot ring allocated; `guard` armed; tick = 0; if `Origin::Restored`, `registry_restore`
    from the checkpoint before the first tick.

Shutdown is the reverse; nothing is "freed" — arenas are released wholesale.

### 8.3 `engine_tick_once` (the function the loop, the driver and the rollback driver all call)

```cpp
void engine_tick_once(Engine* e, const InputFrame* frames /*[MAX_PEERS]*/) {
    World* w = &e->world;
    guard_tick_begin(&e->guard, w->registry);
    w->input = frames;
    for (Phase p = FIRST; p <= LAST; ++p) { run_phase(w, p); /* run_phase applies commands at its end */ }
    // LAST has run: checkpoint hashed, recorder appended, net_send sent, snapshot ring pushed
    events_swap(w);                  // barrier step 2
    interp_pingpong(w);              // barrier step 3: prev ← current for Transform (any interp pair)
    scratch_reset_all(e);            // barrier step 4 (workers; main scratch resets after render)
    w->tick += 1;
    guard_tick_end(&e->guard, w->registry);
}
```

`engine_frame` is the §0 loop body; `tl_driver` calls `engine_tick_once` directly with
`accumulator` forced. Rollback (`NETCODE.md` §20) restores a ring slot (which sets `w->tick`),
clears both event halves and the command chunks, snaps interpolation, then calls
`engine_tick_once` for each tick up to the present with corrected frames — never from inside a
system.

### 8.4 Tests (`tests/core/loop.test.cpp`)

Accumulator arithmetic with a fake clock (steps per frame, `MAX_STEPS` cap drops time, `alpha` in
[0,1)); `PRODUCE_WAIT` renders without ticking; barrier order observable (a test system emits an
event and a command in `LAST`; next tick sees both); headless forced-accumulator runs N ticks
exactly; restore-then-retick reproduces the hash trace.

*Rev 1 — 2026-08-22.*
