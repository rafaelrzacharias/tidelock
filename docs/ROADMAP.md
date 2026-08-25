# ROADMAP — the dependency graph, and what runs in parallel (tidelock, rev 1)

> 2026-08-22. `TODO.md` is the *queue* (what is next); this doc is the *shape* (what can run at
> the same time). ore/foundry died of serial execution: one lane, one feature at a time, the
> roadmap as a list. Here the roadmap is a DAG cut into **waves**; inside a wave every **lane**
> is independent and runs concurrently — one Opus session per lane, each in its own git worktree,
> merged at the wave boundary. The seams that make this safe already exist: module = static lib,
> doc = contract, `CANON.md` + the doc audit at the boundary.

## 0. Operating rules for parallel lanes

1. **Header first.** A lane's first commit is its `module.h` — contract block, full signatures,
   `static_assert`s, stubs that `TL_FATAL("unimplemented")` — so every dependent lane compiles
   against it from day one. Changing a published header is a cross-lane event: announce it in the
   wave's merge note and run the doc audit.
2. **One lane = one module + its tests + its doc.** A lane never edits another lane's module; a
   needed change is filed as a ruling request in `TODO.md` and picked up by the owner.
3. **Wave boundaries are the only integration points.** Merge, run the full PR lane (audits, unit,
   dual-sim), fix, tag. No lane starts the next wave on an unmerged base.
4. **The critical path is protected.** Lanes on it (marked ★) get the first session of the day;
   everything else fills the remaining sessions. Nothing on the critical path waits for a lane off
   it.
5. **Slice brief per lane** (`CLAUDE.md` doc-integrity protocol) — the brief lists the lane's
   *consumers in other lanes*, which is how a lane learns what interface it must not break.

## 1. The graph

```
W0  skeleton
     |
W1   +-* fx
     +-- mem --- containers
     +-- rng/hash
     +-- platform (headless + sdl3) --- jobs
     +-- runner + driver
     +-- tooling runtimes (log/prof/probe/assert)

W2   +-* gate0            (fx, mem)
     +-- ecs              (mem, containers, rng/hash)
     +-- luau-vm          (fx, mem)                      sandbox, fx bindings, data VM
     +-- net-p1           (containers, rng/hash, WIRE)   encoder, archive, checkpoint, chain
     +-- alloy-substrate  (fx, mem, containers, rng)     pools, broadphase, sdf, cavity, topology,
     |                                                   bond-graph heat flux - everything EXCEPT pass 3
     +-- vendor           (mem, platform)                SDL_ttf, imgui, enet, monocypher + pool hooks

W3   +-* alloy-solver     (gate0 verdict, alloy-substrate)   pass 3 promoted from gate0
     +-- loop+input       (ecs, platform)
     +-- assets+data      (ecs, luau-vm, platform)
     +-- luau-bindings    (ecs, alloy-substrate, luau-vm)    ecs.*, alloy.*, trampolines, reload, luauc
     +-- render2d         (ecs, platform draw, sim/views.h)
     +-- editor           (ecs, render2d, vendor)            inspector, console, log, profiler panels
     +-- net-p2           (net-p1, platform, producer seam)  ENet, sequencer, NetworkProducer
     +-- alloy-fields | alloy-liquids-gases | alloy-chemistry   (alloy-substrate) - 3 lanes
     +-- save             (ecs encoder)

W4   +-* v0-integration   (loop, render2d, luau, assets, editor)   app/wiring.cpp, moving sprite - ONE lane
     +-- hovel-A          (net-p2, loop, mem registry)              3 machines, integer lockstep
     +-- alloy-plants / alloy-agents   (alloy-solver)
     +-- jobs-integration (jobs, alloy-solver, ecs groups)

W5   +-* milestone-2      (render2d simview, alloy)                 dig / flood / melt on screen
     +-- hovel B-E, net-p3..p8
     +-- T-A-01 closure restore (alloy, mem ring)
     +-- worker-sweep gate, cross-ISA nightly, soaks
     +-- game scripts (script/sim, script/ui) on the v0 + M2 engine
```

★ = critical path: `skeleton → fx → gate0 → alloy-solver → v0 → milestone-2`. Everything else
is slack and is scheduled to finish *before* its wave's ★ lane needs it.

## 2. Lanes — scope, model, inputs, done criterion, doc

**Model policy (ruled 2026-08-22).** *Fable 5 high* for the critical path and anything where a
silent mistake is a desync found weeks later (integer/UB/ordering/hashing code). *Opus 5 high* for
deep but well-specified systems. *Sonnet 5* for transcription-plus-tests lanes the specs fully
pin down. **Never low effort on sim or netcode code.** Every lane's merge gets an adversarial
review by a *different, higher-or-equal* model in a fresh context (`CLAUDE.md` rule 5): Sonnet
lanes reviewed by Opus; Opus/Fable lanes reviewed by Fable. Before launching a lane, look up its
model here — the launch prompt names it.

| Wave | Lane | Model | Builds | Depends on | Done (the doc's criterion) |
|---|---|---|---|---|---|
| W0 | **skeleton** | Opus 5 high | CMake tree, presets, pi4 toolchain, `tl_types.h`, `tools/audit`, `tools/fingerprint`, runner stub, CI yaml | — | `BUILD.md` §10.5 |
| W1 | ★ **fx** | Fable 5 high | `fx.h`, `fx_palette.h`, `det_math.h`, `fx_float.h`, `tools/fxcheck` | skeleton | `FX-PALETTE.md` §10.6 |
| W1 | **mem** | Fable 5 high | vmem arena, registry, snapshot ring, scratch, handle, `mem_pool`, alloc shim | skeleton | `MEMORY.md` §8.8 |
| W1 | **containers** | Sonnet 5 | Array/Span/SlotMap/Map/Sorted/Ring/Bitset/sort, StrView/interner/fmt | mem (`vmem_arena.h` header) | `CONTAINERS.md` §8.7 |
| W1 | **rng/hash** | Sonnet 5 | rapidhash vendored, `tl_hash64`, `NameHash`, `rng_for`, `rng_systems.h` | skeleton | `DETERMINISM.md` §9.5 |
| W1 | **platform** | Sonnet 5 | contract, headless impl, sdl3 impl (SDL3 + stb vendored) | skeleton | `PLATFORM.md` §9 |
| W1 | **runner+driver** | Sonnet 5 | `tl_tests` full, `tl_driver` skeleton, harness API stubs | skeleton | `TESTING.md` §9 |
| W1 | **tooling-rt** | Sonnet 5 | `tl_log/prof/probe/assert` headers + runtimes, crash writer | skeleton | `TOOLING.md` §9 (foundation half) |
| W1 | **jobs** | Opus 5 high | atomics, pool, `parallel_for/levels`, shuffle mode | platform thread API header | `JOBS.md` §6.4 |
| W2 | ★ **gate0** | Fable 5 high | `tests/gate0` solver + scenarios + shadow; run PC (arm64 legs via CI since 2026-08-25); rev-2 palette | fx, mem | `GATE0-BENCH.md` §8.5 |
| W2 | **ecs** | Fable 5 high | reflect, columns, schedule, commands, events, encoder, diff | mem, containers, rng/hash | `ECS.md` §10.8 |
| W2 | **luau-vm** | Opus 5 high | Luau vendored, three VMs, sandbox, `fx.*`, data VM | fx, mem | `LUAU-LAYER.md` §10.12 (VM half) |
| W2 | **net-p1** | Opus 5 high | `wire.h`, encoder, archive, checkpoint writer, chain | containers, rng/hash, `TL_WIRE_STRUCT` | `NETCODE.md` §20.8 Phase 1 |
| W2 | **alloy-substrate** | Fable 5 high | pools, broadphase, sdf + carve + redistance, cavity flood, topology/union-find, bond-graph heat flux (the first exact-conservation slice), edit intake | fx, mem, containers, rng/hash | `ALLOY.md` §14.7 steps 1, 3, 4 |
| W2 | **vendor** | Sonnet 5 | SDL_ttf, imgui, enet, monocypher builds; pool hooks; adaptors | mem, platform | links clean on the `CANON.md` matrix (4 legs) |
| W3 | ★ **alloy-solver** | Fable 5 high | pass 3 (from gate0), contacts, colouring, velocity pass | gate0 verdict, alloy-substrate | `ALLOY.md` §14.7 step 5 (solver) |
| W3 | **ci-matrix** | Fable 5 high | `pr.yml` on the `CANON.md` target matrix (4 hosted native legs), 4-way `build_id` gate, `binarch.py`, `targets.py` 4th triple, the target-set ruling across the docs | skeleton | all four legs green on one commit (`BUILD.md` §10.4) |
| W3 | **loop+input** | Sonnet 5 | loop, time, phases, interp, action map, Live/Script/Replay producers, recorder | ecs, platform | `FRAME-LOOP.md` §8.4, `INPUT.md` §9.6 |
| W3 | **assets+data** | Sonnet 5 | asset registry, loaders, data-table compiler, save v1 | ecs, luau-vm, platform | `ASSETS-AND-DATA.md` §8.5 |
| W3 | **luau-bindings** | Opus 5 high | `ecs.*`, `alloy.*`, `input/events/data/log`, trampolines, proxies, reload, `luauc`, binding docs | ecs, alloy-substrate, luau-vm | `LUAU-LAYER.md` §10.12 |
| W3 | **render2d** | Sonnet 5 | camera, extract, queue/sort/batch, sprite, debug draw, sdl backend | ecs, platform, `sim/views.h` | `RENDER2D.md` §9 v0 criterion |
| W3 | **editor** | Sonnet 5 | ImGui shell, inspector, console/cvars, log, profiler, probes, replay panel | ecs, render2d, vendor | `TOOLING.md` §9 v0 panels |
| W3 | **net-p2** | Fable 5 high | ENet transport, sequencer, `NetworkProducer`, hash exchange, 2-peer loopback | net-p1, platform, `InputProducer` seam | `NETCODE.md` §20.8 Phase 2 |
| W3 | **alloy-fields** / **alloy-liquids-gases** / **alloy-chemistry** | Opus 5 high | passes 1, 2+PBF+cavities, 4+fire+transitions | alloy-substrate (+solver for liquids) | `ALLOY.md` §14.7 step 5 per pass |
| W4 | ★ **v0-integration** | Opus 5 high | `app/wiring.cpp`, moving sprite, record→replay of a session | loop+input, render2d, luau-bindings, assets, editor | `ARCHITECTURE.md` §9 v0 |
| W4 | **hovel-A** | Opus 5 high | `tl_hovel`, 3 machines, Milestone A | net-p2, loop, mem | `NETCODE.md` §19.5 A |
| W4 | **alloy-plants / agents** | Opus 5 high | §7, §8.2 | alloy-solver | `ALLOY.md` tests |
| W4 | **jobs-integration** | Fable 5 high | colouring on `parallel_levels`, ecs groups, chunk-keyed merges, the 1/2/8/16 gate | jobs, alloy-solver, ecs | `JOBS.md` §3 |
| W5 | ★ **milestone-2** | Opus 5 high | sim view, toy dig/flood/melt slice | render2d, alloy | `ARCHITECTURE.md` §9 M2 |
| W5 | **hovel B–E**, **net-p3…p8**, **T-A-01**, **soaks**, **game scripts** | per lane: net/T-A-01 Fable high, rest Opus/Sonnet | per their docs | W4 | per their docs |

## 3. What this changes versus the serial TODO

- **Gate 0 no longer blocks the engine.** Only pass 3 waits for its verdict; eight W1/W2 lanes run
  beside it. If the verdict moves a row, one header changes.
- **Netcode starts in W2**, not after Alloy: Phases 1–2 need only the foundation and the producer
  seam, and Hovel A needs no sim.
- **Alloy is five lanes, not one**: substrate in W2, then solver ★ + three pass lanes in W3, then
  plants/agents in W4. The first exact-conservation slice (heat flux) lands in W2.
- **Integration is explicit and singular**: v0 in W4, Milestone 2 in W5 — one lane each, after
  their inputs are merged, never "while we go".

## 4. Throughput rule of thumb

With N concurrent sessions, run the ★ lane plus the N−1 lanes with the most downstream consumers
(`XREF.md` tells you). W1 has 8 lanes, W2 6, W3 10, W4 4 — the plan saturates 4–8 sessions
through W3. A lane that finishes early picks the next unstarted lane in the same wave whose inputs
are merged; it never starts a lane from the next wave.

*Rev 1 — 2026-08-22. Revise at each wave boundary: record what actually ran in parallel and what
blocked, one line per lane, so the next wave's cut is measured, not guessed.*
