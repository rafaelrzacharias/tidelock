# Architecture — the spine (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. Best so far, not final. Pre-code.
> **Lineage:** harvests `../foundry/FOUNDRY-CORE.md` §0/§1/§4/§7 and `ENGINE-DESIGN.md` A1 under the
> ruling in `PIVOT-DESIGN.md`. Where this doc and PIVOT conflict, PIVOT wins; file the conflict.
> **What this doc owns:** the layered DAG, the module policy, the three cross-system channels, the
> seams, and the one binary rule. Every other doc is a node in this graph.

---

## 0. The one rule — agnostic core (DECIDED)

**The engine encodes no game-type, perspective, or gameplay assumption.** The rule that killed Layr
(four perspectives branched in every system → every feature N×M) is the rule here. Concretely:

| The engine KNOWS | The engine must NEVER know |
|---|---|
| entities + POD components, 2D transforms in fx units | a perspective, gravity direction, a collision model |
| the fixed-tick loop, phases, barriers | any specific gameplay verb |
| a draw-command queue of neutral primitives | a tilemap/lighting model |
| inputs as `InputFrame`, events, commands, arenas, handles | what a material *means* |
| **Alloy** — a matter sim with no game meaning (it exposes views + an ordered edit API) | who is a player, an enemy, a resource |

**Litmus for any proposed engine feature:** *if the next game were a different genre on the same
sim, would this belong in the engine unchanged?* No → it is Luau game data/meaning, or a reserved
seam (`RESERVED-SEAMS.md`).

**Alloy is the one deliberate exception in shape, not in spirit.** It is a large engine module with
a physical model (SDF solids, PBF liquids, cavity gases, fields, chemistry, XPBD). It is still
game-agnostic: materials, species, reactions, recipes are *data* the game supplies; Alloy ships
mechanisms. The two-consumer bar (`ALLOY.md` §9) keeps it honest.

---

## 1. The layered DAG (DECIDED)

Dependencies point **down only**. Systems never call systems — they share the world and post
events/commands. One static exe; modules are folders/static libs with disciplined include paths
(`PIVOT-DESIGN.md` §9); registration order is explicit in one wiring file.

```
app/            main(), the wiring file: registers modules in explicit order, owns the loop
  │
script/         Luau — game data + meaning (the game layer; not C++)          LUAU-LAYER.md
  │             two VMs: restricted sim VM · unrestricted editor/UI VM
  │
src/editor/     ImGui tooling, dev builds only (inspector, console, profiler) TOOLING.md
src/net/        lockstep netcode over ENet (FIRST receive, LAST send)        NETCODE.md
src/render/     render2d: camera, sprite batch, streaming texture, sort key  RENDER2D.md
src/sim/        Alloy — the matter sim (own static lib, symbol-audited)      ALLOY.md
  │
src/core/       world/ECS + reflection · events · input · assets · loop/time ECS.md FRAME-LOOP.md
  │                                                                          INPUT.md ASSETS-AND-DATA.md
src/vendor_glue/ the per-library allocator adaptors over mem_pool           MEMORY.md §8.6
  │             (the only folder that may call pool_alloc/realloc/free); sees foundation only
src/foundation/ fx palette + det math · arenas/handles · containers ·        FX-PALETTE.md MEMORY.md
  │             keyed RNG · pinned hash · StrView/interner · jobs            CONTAINERS.md DETERMINISM.md
  │                                                                          JOBS.md
src/platform/   the porting seam: SDL3 (+SDL_ttf), OS entropy, vmem, clock   PLATFORM.md
  │             one impl (sdl3) + one headless impl (tests)
vendor/         SDL3, SDL_ttf, imgui, luau, enet, stb, monocypher, rapidhash — own TUs, exempt
tools/          offline only (cook, fingerprint, symbol audit) — exempt from the C++ subset
tests/          runner, determinism harness, Gate 0 bench, Hovel            TESTING.md GATE0-BENCH.md
```

Rules that keep the graph acyclic as it grows:

1. **`foundation/` is a leaf** — no includes from above it, no platform includes (vmem/entropy reach
   it only through fn-ptr seams injected at boot).
2. **`platform/` is reachable only from `core/`, `render/`, `net/`, `app/`, `editor/`** — never from
   `sim/` or `foundation/`. The symbol-audit gate (`CPP-SUBSET.md` §4) proves `sim/` has no
   clock/entropy/io/alloc symbols; the include-path discipline makes the violation visible before
   link.
3. **`sim/` depends only on `foundation/`.** It owns no rendering, no input, no frame loop, no game
   meaning. It exposes views + an ordered edit API + an event stream. Headless tests link it
   directly.
4. **`render/`, `net/`, `editor/` are peers** — none includes another. They meet only through
   `core/` (world, events, commands, the draw queue, the `InputFrame` producer seam).
5. **`script/` is not C++.** Luau reaches the engine only through registered bindings
   (`LUAU-LAYER.md`); the engine never reaches into a Luau table for authoritative state.

---

## 2. The three channels (DECIDED — D5/D7/D15 carried)

Everything cross-system goes through one of three channels, chosen by coupling requirement:

| Need | Channel | Where specified |
|---|---|---|
| latency-critical, same-tick | **shared component data**, read/written in phase order | `ECS.md` §3, `FRAME-LOOP.md` §2 |
| structural change (spawn/destroy/add/remove, sim edits) | **deferred command buffer**, applied at the next barrier, chunk-ordered | `ECS.md` §4, `ALLOY.md` §9.2 |
| decoupled reaction, one-tick latency OK | **typed double-buffered event queue** — never fire-and-forget (the Layr `Signal<T>` mistake) | `ECS.md` §5 |

"Systems never call systems" is enforceable, not aspirational: a system is a stateless free
function receiving `World*`; there is no handle by which it could reach another system.

**The orchestrator is knowledge-free.** `app/` sequences phases and runs the schedule built once at
startup from registrations; it references zero game types. The game exists as Luau scripts and
data tables it loads by path.

---

## 3. Seams — where expansion happens (DECIDED)

A seam is a stable interface with ONE implementation now and the ≥2-impl A/B available behind it,
never paid for upfront. The seams tidelock ships at v0 and who sits behind them:

| Seam | v0 impl | Reserved second impl | Shape |
|---|---|---|---|
| platform | SDL3 | headless (tests) — built at v0, it *is* the second impl | struct-of-fn-ptrs resolved once at boot (`PLATFORM.md`) |
| render path | SDL_Render | SDL_GPU | same `submit_draw` + sort key; backend swaps under `present` (`RENDER2D.md`) |
| input producer | Live (SDL events) | Script (tests) · Replay · Network — all v0-shaped, Replay + Script built with the harness | one registered `produce(InputFrame*)` fn-ptr (`INPUT.md` §4) |
| sim | Alloy | — (a second sim is a different engine) | `init/step/views/edit/query/events/snapshot` (`ALLOY.md` §9) |
| game layer | Luau | — | bindings + reflection glue (`LUAU-LAYER.md`) |
| transport | ENet | — (LAN-only/relay are rulings, not impls) | channels INPUT/CONTROL/BULK (`NETCODE.md` §5) |
| hash / RNG / fx kernels | rapidhash · splitmix64 mix · FixPointCS ports | — pinned; changing one bumps the fingerprint | `FX-PALETTE.md`, `DETERMINISM.md` |

**Backend-isolation rule (from A1, now enforced by layout):** no backend type (`SDL_*`, `ENet*`,
`lua_State*`, `ImGui*`) appears above its wrap module. Engine code speaks handles and our types.
If engine code needs a backend type to express an algorithm, the wrap is missing a verb — fix the
interface, never move code across the seam. CI greps for backend headers outside their module.

---

## 4. Module policy — the promotion ladder as folder discipline (DECIDED)

The DLL machinery is retired; the *discipline* survives (`PIVOT-DESIGN.md` §9):

- **Pulled, never pushed.** A module earns an engine-side home only when a real consumer exists.
  Something born in a game's Luau (or in a test) is promoted into `src/` after it has a consumer
  and a headless test.
- **Two structurally different consumers before an API is called stable.** Until then it is "best
  so far" and may change freely.
- **No game nouns in a promoted API.** `carve(brush)` yes; `dig_for_ore(...)` no.
- **Every module ships with headless tests** and registers itself in the one wiring file in
  `app/`. Registration order is the load order; there is no discovery.
- **Module = folder = static lib target.** Private headers stay private; a cross-boundary include
  of a private header is a visible violation, a link error at best.

---

## 5. The binary shape (DECIDED)

One static exe per platform; dev and ship differ by build tier, compiled out by absence, never a
runtime `if`:

| Tier | Ships |
|---|---|
| `dev` | ImGui editor/console/profiler/probes, float-shadow solver, impairment shim, Luau Tier-0/1 debugger, asserts at all levels |
| `netcode` (= ship-equivalent, used for Hovel/soaks) | no editor, slim fatal-assert tier, no shadow, no shim |
| `ship` | as `netcode`, plus release flags; identical fingerprint class |

A `netcode`-tier binary is what peers must match on; the fingerprint (`BUILD.md` §5) is computed
over it. Dev tooling must never be able to change sim state outside the tick-stamped command
channel (editor mutations go through the deferred command buffer, `TOOLING.md` §2).

The headless test exe (`tests/`) links `foundation/ + core/ + sim/ + platform-headless` and no
render/editor — which is also the shape the symbol audit runs over.

---

## 6. Data flow per tick (DECIDED — the loop in one picture)

```
platform.pump_events() ──► input.fold (Live/Script/Replay/Network producer) ──► InputFrame (int)
                                                                                      │
FIRST      net receive · input drain · action-map (Luau sim VM) ──► MoveIntents + edit commands
PRE_UPDATE game systems (Luau + C++) read world, write components, emit commands/events
UPDATE     alloy.step(commands)  ── 5 passes ──► views, per-arena hashes, events
POST_UPDATE game reactions to sim events (Luau), transform resolve
LAST       determinism checkpoint (per-arena hash) · net send · snapshot ring push
── barrier: flush commands, swap events ──
PRE_RENDER extract: fx → float, prev/current interpolation, camera          (render-side, float OK)
RENDER     sprite batch + sim view upload + ImGui (dev)                     (never writes back)
```

Render reads sim state through an **extract** step that converts fx to float once; nothing render-
side is ever read back into the sim (D10, INV-6). The full loop and phase semantics: `FRAME-LOOP.md`.

---

## 7. What is deliberately not here

- No scripting VM other than Luau; no second language.
- No general heap allocator (`MEMORY.md` §1 — wanting one is a design smell).
- No render graph / lighting / post at v0 (`RESERVED-SEAMS.md` §11).
- No premade component kit. Components are declared by the game (Luau-declared, reflected) or by
  the engine modules that own them — never a library of "useful" ones.
- No modes. A perspective is a camera + a render path; a physics model is Alloy; a genre is Luau.

---

## 8. Rulings (closed 2026-08-22 — nothing open)

- **R-1 `render/` reads Alloy views directly.** `sim/views.h` is the one public header `render/`
  may include from `sim/`: read-only POD view structs (chunk rasters, particle spans, body poses),
  never pool internals. Going through `core/` handles would add a copy of the largest per-frame
  payload for no seam benefit; the include firewall pins it to that header.
- **R-2 `net/` owns one permanent, non-registered arena** (protocol state, log ring, redundancy
  window, ENet buffers via its pool hook) plus main scratch for per-frame encode/decode. Not
  hashed, not snapshotted — none of it is authoritative. The snapshot ring (registered set) and
  the log ring are therefore sized independently.

## 9. Milestones and done criteria (the build order, with what "done" means)

| Milestone | Builds | Done when |
|---|---|---|
| **Skeleton** | CMake tree, presets, audits, `tl_types.h`, runner stub | empty tree passes every audit on the `CANON.md` target matrix; `build_id.txt` stable (`BUILD.md` §10.5) |
| **Gate 0** | `fx.h`, `fx_palette.h`, `det_math.h`, `fxcheck`, `tl_gate0` | `GATE0-BENCH.md` §8.5; `FX-PALETTE.md` rev 2 committed |
| **Foundation** | vmem/registry/scratch/handles/pools, containers, rng/hash, runner, headless platform, harness | `MEMORY.md` §8.8, `CONTAINERS.md` §8.7, `DETERMINISM.md` §9.5 green; dual-sim of an empty world passes |
| **ECS** | reflection, columns, schedule, commands, events, encoder, loop, input producers, recorder, driver | `ECS.md` §10.8, `FRAME-LOOP.md` §8.4, `INPUT.md` §9.6 green; `tl_driver --record/--replay --verify` round-trips a scripted scene |
| **v0** | SDL3 platform, assets, data compiler, Luau VMs + bindings, render2d sprite path, ImGui shell (inspector/console/log/profiler/replay) | window + a Luau-declared component moved by a Luau system at fixed 60 Hz; clean exit; `--record` of the session replays bit-exact headless; fingerprints logged |
| **Hovel A** | ENet transport, encoder, sequencer, `NetworkProducer`, hash exchange, `tl_hovel` | `NETCODE.md` §19.5 A: 1 h, 3 machines, zero divergence |
| **Milestone 2** | Alloy substrate → topology → solids → solver (promoted from Gate 0) → liquids; sim view | dig / flood / melt a toy slice on screen; `ALLOY.md` §14 build steps 1–5 green; G-05 numbers reproduced in-engine |
| **Hovel B–E, Alloy rest, jobs, T-A-01** | per the per-doc queues | each doc's done criteria; the 10 h soak |

*Rev 1 — written 2026-08-22 from the pivot ruling. Supersedes `FOUNDRY-CORE.md` §0/§1/§4/§7 and
`ENGINE-DESIGN.md` A1 for this engine.*
