# Frame loop, time, phases, interpolation (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §7. Carries foundry CORE §3, D7, D10,
> D11's knobs, FOUNDRY-API §2 into C++ + fixed point.
> **Owns:** `src/core/loop.h`, `time.h`, `phase.h`, `interp.h`; `app/main.cpp` instantiates it.

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
    f32 alpha = (f32)(accumulator / FIXED_DT_SECONDS);       // render-side float, fine
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

---

## 2. Phases (DECIDED — position-named, closed)

| Phase | Typical content (convention, not enforced) |
|---|---|
| `FIRST` | net receive (confirmed frames already in `world.input`), input drain + action-map (Luau sim VM), tick bookkeeping |
| `PRE_UPDATE` | spawners, timers/cooldowns, game systems that produce MoveIntents + sim edit commands |
| `UPDATE` | `alloy_step(world.sim, &edit_commands)` — the five passes; then bulk gameplay |
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

*Rev 1 — 2026-08-22.*
