# Alloy — the unified deterministic matter sim (tidelock, rev 1)

> **Status: rev 1, 2026-08-22 — best so far, not final.** Alloy is the deterministic matter sim
> module of tidelock, living in `src/sim/` as a static lib. §1–§8 mechanisms are carried whole
> from the foundry design; §9–§13 are re-homed onto the C++/Luau/fixed-point stack ruled in
> `PIVOT-DESIGN.md`. Every section is **DECIDED** (lock = best so far, never final); items a
> measurement verifies carry a pre-committed flip condition in the closing "Open items & gates"
> section.
> Where this doc and `PIVOT-DESIGN.md` conflict, PIVOT wins and this doc has a bug.

*Lineage: supersedes `../foundry/ALLOY-DESIGN.md` (2026-07-11, Ore era). Not a live reference.*

---

## §0 Scope & vision

### §0.1 What it is (DECIDED)

A gridless, continuous, deterministic 2D material-and-mechanics simulation:

- **Solids** = rigid/soft bodies carrying body-local SDFs — carvable, fracturable, rotatable,
  movable. Terrain is just big static bodies.
- **Liquids** = PBF particles (XPBD density constraints) — slosh, flow, fill, overflow, mix or
  stratify (immiscibility).
- **Gases** = cavity graph (connected air pockets) holding integer moles per species — compress,
  expand to fill, mix, drive pressure/buoyancy. Wind derived from flow.
- **Fields & laws** = temperature, electricity, magnetism, radiation, force zones — on the one
  neighbor graph, never on a world grid.
- **Chemistry** = reaction rules (pair / threshold / catalyzed) on that same neighbor graph; the
  phase ladder moves matter between the three representations (melt: SDF→particles; freeze:
  particles→SDF; boil: particles→cavity moles; condense: reverse).
- **Mechanics** = the XPBD constraint solver: 7 constraint primitives, motors, force fields,
  fracture, `AgentBody`.

**Why unified**: no seam, no 1-tick cross-system latency, one failure domain, full reactivity —
fire heats rope, rope snaps, load falls in water, splash douses fire, in one tick.

### §0.2 Position in the stack & development path (DECIDED — replaces the game-DLL path)

```
src/foundation (fx palette, det math, arenas, containers, RNG, hash)
  → src/core (ECS + reflection, events)
    → src/sim  (Alloy — this doc; own static lib, symbol-audited)
      → src/net (lockstep, ENet)  ·  script/ (Luau: data + meaning)
```

- Alloy owns **no rendering, no input, no game meaning**. It exposes state views; the engine and
  the Luau game layer draw and decide.
- **There is no game DLL and no hot reload.** Alloy is `src/sim/` from day one, developed
  **headless-first**: the determinism harness (`TESTING.md`) exists before the substrate, and
  headless tests link the static lib directly. No engine wiring until the harness is green.
- **Iteration story = compile time + Luau data reload.** Full rebuild budget < 10 s, incremental
  < 2 s (`PIVOT-DESIGN.md` §2 — a regression is a perf regression). Tuning data (materials,
  reactions, species, agent params) is Luau-authored (§11) and reloadable at runtime in dev
  builds; sim *code* changes are a rebuild. This is the whole story — no migrate(T), no layout
  hash, no DLL seam.
- The promotion-ladder discipline (pulled-never-pushed, two structurally different consumers,
  no game nouns in the promoted API, headless tests) survives as **folder policy only**
  (`PIVOT-DESIGN.md` §9).
- Determinism contract (§10): **fixed point by construction**. No floats on any sim path; integer
  ops are bit-exact on every ISA. Mass carried in **integer quanta** (a particle = N mass units,
  cavities hold integer moles) so Σ-mass is an exact integer test.
- The two-consumer bar survives: the API is proven only when both consumer games' verbs express
  cleanly on it (survival verbs and combat verbs).

### §0.3 Player-facing north stars

Players explore, fight, swim, build mechanical/electrical/fluid contraptions; plant vegetation
that grows and burns. Every feature section answers to these.

### §0.4 The tick — five fixed passes (DECIDED; fixed order = determinism)

1. **Fields** — transport scalar/vector state (temp, charge, species…) over the neighbor graph
   and the cavity graph. Globally coupled (no island structure — `NETCODE.md` §3).
2. **Forces** — force fields, magnetism, buoyancy/drag from cavity pressure and liquid density,
   gravity.
3. **Solve** — the XPBD substep loop (8 substeps, §8.1): contacts, the 7 primitives, fluid
   density constraints, motors.
4. **Chemistry** — reaction rules on the neighbor graph; buffered edits applied at the pass
   boundary, never mid-solve.
5. **Topology** — the union-find layer: carving, fracture/re-island, cavity re-flood, freeze/melt
   re-bake, settle/disturb, sleep management, particle-pool compaction.

Each pass is a stateless free function over registered arenas (`ECS.md` system shape).
Per-tick bookkeeping (event-ring merge in chunk order, arena-offset guard check) runs after
pass 5. **The per-arena hash is not taken inside the step**: it is the engine's `LAST`-phase
checkpoint over the registry (CANON "Phases and the barrier"); `alloy_snapshot_hash` wraps the
same registry call for headless tests (see §14.3). Pass 0 = command intake (see §14.4.0).

### §0.5 Status / design log

- 2026-07-11 — foundry design: §1–§13 decided; Crucible + Anvil fully absorbed.
- 2026-08-21 — `PIVOT-DESIGN.md`: Ore retired; fixed point; world constants; Gate 0 frozen.
- 2026-08-22 — **this rev**: re-homed onto tidelock. §1–§8 mechanisms unchanged. New: §1.3 C++
  mapping, §10 fixed-point edition, §11 Luau data path, §12 world-gen ownership, §13 arena
  commit, closing gates section. Next: Gate 0 (`GATE0-BENCH.md`), then the build queue.

---

## §1 Substrate — the unified body/particle model

### §1.1 Two pools, one identity space (DECIDED)

Alternatives weighed: (A) one unified SoA array with optional cold columns, (B) two pools under
one reference space, (C) pure-FleX rigid-as-particle-clusters. **C rejected** (shape-matched
clusters give mushy contacts — kills the contraptions north star). **A rejected** (at ~10–50k
particles vs ~1–5k bodies the pools want different layouts; optional columns tax the dense
loop). **B chosen**:

- **`Particle` pool** — dense SoA, the high-N citizens: liquid particles, rope/cloth points,
  granular debris/dust, embers. Hot row target ~24–32 B: `pos` (2×`pos_t`), `prev_pos`
  (2×`pos_t`; XPBD velocity is implicit), `inv_mass` (`invmass_t`), `species_id` (u16),
  `flags` (u16), `mass_quanta` (i32 — exact Σ-mass), `temp` (i16 quanta; hot-row vs warm
  column is a bench, §1.4).
- **`Body` pool** — small-N rich records: rigid + soft bodies. Adds rotation (`theta: angle_t`
  turns, `omega: omega_t`), inertia (`invinertia`: `invmass_t` row — same range argument),
  shape/SDF handle, material, island id, sleep state. Terrain = big static bodies here.
- **Identity**: bodies, constraints, plants, agents, cavities/basins are `Handle<Tag,IDX,GEN>`
  citizens in `SlotMap`s (`MEMORY.md`, `CONTAINERS.md`). **Particles are plain `u32` indices
  with tick-scoped validity** (`PIVOT-DESIGN.md` §4): the particle pool is compacted only at the
  pass-5 boundary, which emits an old→new remap applied in the same pass to every referrer
  (constraint endpoints, island membership, pending events). Inside a tick an index is stable.
  Persistent references from the game layer target bodies/constraints/agents (handles), never
  particle indices; a Luau-side particle reference is `{index, tick}` and is rejected if stale.
- **`CarrierRef`** = `{kind:2, id:30}` (particle | body-region | cavity | basin) is the one
  tagged reference that constraints, reactions, events and queries use for "either kind"
  (bit layout and tick-scoped validity rule: see §14.2).
- Survives from the rigid-body lineage: *rope/cloth points are particles* (points with
  constraints, not rotating bodies). Dies: liquids/rigids sharing one array.
- **Cavities are not entities** — they are regions (a graph maintained by pass 5) with their own
  small store; they hold integer moles per species (§4).

### §1.2 Broadphase, sort & neighbor graph (DECIDED)

**Tiered uniform spatial hash** (rejected: single-tier grid — large bodies smear across
thousands of cells; SAP+grid hybrid — incremental sort state is extra desync surface):

- **Fine tier**: uniform hash grid at particle-interaction radius; serves particle–particle
  (PBF neighbors, chemistry pairs). Rebuilt from scratch every tick — cell = pure function of
  position, contents **ID-sorted**; no incremental state to desync.
- **Coarse tier**: body AABBs in a coarser grid; body–body candidate pairs from cell overlap,
  pair list ID-sorted. Particle–body: body AABB → fine-tier cell range walk.
- Rebuild cadence: broadphase once per tick with a support margin covering max substep travel
  (`V_MAX_WORLD × h × 8`); substeps reuse the pair list (standard XPBD practice — DECIDED; the
  tunneling test at `V_MAX_WORLD` verifies, flip condition in the closing table).

**Sort = bespoke LSD radix on integer `(cell_key: u32, index: u32)` keys — DECIDED.** There is
no stdlib to bench against (§2 of PIVOT bans it); the only alternative is our own comparison
sort:

| Axis | LSD radix (chosen) | Own comparison sort (heap/merge) |
|---|---|---|
| Determinism | Bit-exact by construction (integer keys, stable passes) | Same |
| Performance | O(n) passes over a bounded key width; counting buffers from the sim arena once; SIMD-friendly histograms; sequential access | O(n log n); heap's `2i+1` sift stride is cache-hostile at 50k |
| LOC / cognitive | ~80 lines | ~60 lines |
| Compile / rebuild | Nil either way | Nil |
| Correctness / test surface | Property test vs naive reference; stable by construction | Same |
| Iteration cost | n/a | n/a |

Radix sits on the per-tick broadphase path where the constant factor is the whole point. The
counting buffers are arena-resident (zero per-tick alloc — the arena-offset guard enforces it).

**Neighbor determinism discipline**: every neighbor list is sorted by stable id before any
accumulation. With fixed point, the *order* of an integer sum no longer affects the result — the
sorted order is **kept as the rule anyway**: it is cheap, and it keeps any future widened or
non-commutative combine safe (`JOBS.md` reduction rule, PIVOT §12a).

### §1.3 tidelock mapping (DECIDED — replaces the Ore mapping)

| Concern | Mechanism |
|---|---|
| Pools | Own `Array<T>` columns on `VMemArena` (stable bases forever — transient raw pointers are safe within a pass); all pools in the **registered arena set** (`MEMORY.md`) |
| Identity | `Handle<Tag,IDX,GEN>` per domain; particles = plain indices, tick-scoped (§1.1) |
| Zero per-tick alloc | Arena-offset guard: registered arena offsets recorded at tick start, only scratch may move by tick end (debug-fatal); counting shim on the global allocator |
| Parallel dispatch | `JOBS.md` job system: `parallel_for(range, grain)`; outputs keyed by **chunk id, never worker id**; per-chunk command buffers applied at the barrier in chunk order; colored Gauss-Seidel = colors as sequential levels, `parallel_for` within a level |
| Arithmetic | fx palette (`FX-PALETTE.md`) + det math (`DETERMINISM.md`): FixPointCS-ported kernels, turns not radians, `mul<R>` with widened RNE intermediates; `<math.h>` banned |
| State hash | Pinned rapidhash per registered arena per tick over `[base, used)` (never capacity) |
| RNG | Stateless keyed `rng_for(seed, tick, system_id, carrier_id)`; no sequential generator |
| Effect ban | Sim compiles into its own static lib; CI `llvm-nm` gate fails on any undefined symbol outside the allowlist (no malloc/libm/clock/io) — the `@deterministic` replacement |
| Iteration order | Explicit sorted iteration (`SlotMap` walks `0..slot_cap()`, id-sorted lists); `Map` never walked in sim code — the `@iterable` replacement |
| Padding / layout | Explicitly padded hashed structs, zero-filled arenas; `static_assert(trivially_copyable)` at every pool |

`@fp_pin` has no replacement: nothing to pin.

### §1.4 Substrate rulings (closed 2026-08-22)

- **Substeps = 8** (PIVOT §3.1a). Gate 0's 4/8/16 sweep verifies; a move is a recorded constant
  change.
- **Particle budget = 20k active at nominal load** (§11.2); G-05 verifies on PC and Pi.
- **Hot row = 32 B, `temp` in the hot row:** `pos` 8 · `prev_pos` 8 · `inv_mass` 4 ·
  `mass_quanta` 4 · `species_id` 2 · `flags` 2 · `temp` 2 · `_pad0` 2. Two rows per 64-byte line.
  `temp` stays hot because pass 1 and pass 4 both read it per particle every tick; a warm column
  would add a second stream to the two hottest passes. Field *order* may be tuned by a cache bench;
  the width and membership are decided.

---

## §2 Solids — SDF bodies, carving, terrain, fracture

### §2.1 Shape = body-local sampled SDF (DECIDED)

Alternatives weighed: analytic CSG trees (rejected as the general rep — carve ops grow the tree
unboundedly; kept as a **possible fast path for un-carvable machine parts**, parked) and polygon
boundaries + clipping (rejected — robust booleans are a correctness hazard even in fixed point).
**Chosen: every solid body carries a body-local discretized SDF**:

- **Texel = integer**: quantized signed distance (`i8` or `i16`, fixed-point texel units —
  width is the §2.4 bench) + `material_id` channel. Carving is integer arithmetic — the grid
  design's structural-determinism moat survives *inside* body-local shapes.
- **Bodies rotate/translate freely**; the SDF lives in body space. World queries transform
  world→body (`pos_t` + `angle_t` sin/cos table, `mul<R>`) and bilinearly sample with integer
  weights. Sampled distance is normalized to `q_t` (q = r/h_kernel) before any kernel.
- **Collision**: particle–solid = sample the SDF (distance + gradient = contact normal), one
  XPBD contact constraint. Body–body = sample each body's surface points against the other's
  SDF (deepest-point contacts), candidate pairs from the coarse broadphase tier.
- **Carving = local CSG writes** (min/max against a brush SDF) over a texel window. The op
  **returns removed quanta per material** — mechanism here, meaning in the game (dig → inventory,
  explosion → debris particles, melt → liquid particles). Solid mass is texel quanta; exact.
- After a carve: local **redistancing sweep** (bounded window) + dirty-flag the body's
  connectivity for pass 5.

### §2.2 Terrain = chunked static SDF (DECIDED)

Rejected: one monolithic terrain body (no locality for carve/stream/fracture); everything-islands
from world-gen (thousands of touching static bodies stress broadphase for no visible gain).
**Chosen**:

- Terrain is a tiled set of **static SDF chunks of 128² texels = 8 m × 8 m** (`TEXEL` = 1/16 m,
  PIVOT §3.1a), logically welded — no constraint solve between them; the immovable background
  body class. World extent ±4,096 m → 1024 × 1024 chunks addressable; resident set is §13's.
  Chunks are **not `Body` rows**: they have no transform and are sampled world-aligned (see
  §14.2 `ChunkHeader`).
- **Streaming is chunk-local** (§13); the unit of paging is fixed here.
- **Fracture = sever→re-island→promote**: carving dirty-flags chunks; pass 5 runs incremental
  connected components (union-find) over solid texels; a component cut from the static weld is
  **promoted** to a dynamic Body-pool body — its texels are copied into a new body-local SDF,
  and it is thereafter an ordinary carvable body. Fragments below a quanta threshold emit
  **granular debris particles** instead of bodies, preserving mass. Rejected (kept so they're
  not re-proposed): runtime Voronoi shatter (hard to make bit-exact; tiny-body spam) and
  pre-authored debris meshes (not emergent).
- **Debris momentum inheritance**: body-impact fracture distributes the impact impulse across
  promoted pieces weighted by proximity to the contact manifold, conserving momentum with the
  impactor (which sheds momentum and plows through); explosion fracture applies the radial
  blast impulse (falloff × direction); pure gravity collapse is the degenerate zero-impulse case.
- **Crack-pattern knob**: fracture may sever extra bonds beyond ground zero in a deterministic
  keyed-RNG radial crack pattern; crack-propagation distance is the data knob for piece
  count/size ("shatters like glass" vs "breaks in two").
- Rendering is not Alloy's: it exposes SDF/material views per chunk/body.

### §2.3 Structural load & collapse = stress on the bond graph (DECIDED)

Rejected: connectivity-only (1-texel bridges hold mountains) and per-texel FEM-lite (compute +
tuning burden; overkill). **Chosen** — the mid-fidelity tier:

- World-gen and body creation **pre-seam** solids into bonded regions along material-aware
  fracture seams; regions within a weld are connected by **bonds** with integer strength (from
  the material table).
- Pass 5 (only where dirty): route supported **integer mass-quanta load** from regions toward
  static ground through the bond graph; a bond whose routed load exceeds strength **severs** →
  union-find re-islands → promote/fall (§2.2). Gradual failure = seams give way one bond at a
  time; games tune drama via material data.
- Integer loads on a small graph: deterministic by construction, cheap, testable (Σ load exact).
- **Load routing is anisotropic**: compression (downward toward support) routes cheaply;
  shear/bending is capped by material `strength`; diagonal in between. Arches, cantilevers and
  spans *emerge*; `strength=0` gives the compression-only dry-stacked-stone floor. **Anchor =
  bedrock sentinel** (max-strength load sink).
- **Two failure paths, one bond graph**: (a) routed static load exceeds bond strength; (b)
  **impact fracture** — each solved bond constraint tracks its carried λ; λ over threshold at
  end-of-step severs. Same downstream.
- **Corrosion couples here**: a corrosive adjacency erodes bond strength / dissolves region
  quanta per tick at `corrosion_rate`; `corrosion_rate=0` marks immune materials.
- **Intact assemblies sleep as one island**: internal bonds of an unstressed assembly are not
  solved — an unbroken crate is nearly free until stressed.
- Pre-fail *telegraphing* (creak events at X% load) is an event-stream feature.

### §2.4 Solids rulings (closed 2026-08-22)

- **Texel = 1/16 m** (PIVOT §3.1a). **SDF distance = `i16` with 4 fractional bits** (1/16 texel,
  ±2,048 texels) — `FX-PALETTE.md` §9 R-2: contact normals need the sub-texel gradient; memory is
  ~2 MB resident. `i8` rejected.
- **Redistancing = integer Chamfer two-pass** (3-4 weights scaled to the 4-bit fraction, forward
  then backward sweep over the dirty window + 1-texel halo). Exact integer arithmetic, bounded
  cost, order-fixed by construction. Fast-sweeping rejected: its accuracy advantage is a float
  property and its per-texel divergence behaviour is a correctness surface.
- Analytic-primitive fast path for machine parts — **rejected until a contraption design names a
  part the sampled SDF cannot represent at 1/16 m**; carvable machine parts are the norm.

---

## §3 Liquids — PBF particles + settle-to-bulk

### §3.1 Motion = PBF (DECIDED)

Liquids are particle-pool citizens under an XPBD **density constraint** (Position Based Fluids)
— the same solver as everything else. Slosh, flow, fill, overflow, pouring, splashes emerge.
Per-species rest density, viscosity (XSPH), cohesion/surface tension as data. Neighbor lists
from the fine tier, ID-sorted. Kernels (poly6/spiky) evaluate on `q_t` = r/h_kernel ∈ [0,1]:
normalize once per pair, polynomial in `q_t`, scale back once — kernel precision is
world-scale-independent (PIVOT §3.1b). Density accumulates in an i64 and rounds once.

### §3.2 Resting liquid = settle-to-bulk (DECIDED)

Rejected: pure PBF (budget burned on still lakes) and PBF+sleeping (memory stays at particle
count; wake cascades are a perf cliff). **Chosen** — the third representation transition:

- A basin whose particles are all asleep (below a quantized energy epsilon for K ticks, K
  fixed) **converts to bulk**: a per-basin record — container region, integer **mass-quanta per
  species**, fill surface (piecewise level line in `pos_t`). Particles are deleted; mass exact.
- **Disturbance re-particle-izes locally**: a body entering, a carve opening the basin floor,
  an impulse, or chemistry — bulk emits particles in the disturbed window only.
- Bulk liquids still participate: Archimedes buoyancy on immersed bodies (analytic — cheaper
  and stabler than particle pressure), fill/overflow via level vs container lip, stratify
  immiscible layers by density, appear in chemistry as a volume-at-interface reagent.
- **Hydrostatic depth pressure**: `P = P_cavity + depth × fluid_weight` from the fill surface —
  consumers: crush damage vs structural margin, gas-envelope compression, orifice-flow rates at
  submerged openings. `P ≥ 1` floor always (ideal-gas divisor).
- Determinism: thresholds are integer tick counts + quantized energy; emission positions from
  `rng_for(seed, tick, SYS_BASIN, basin_id)` + slot.
- **The particle↔bulk boundary is the hardest liquid code** — property tests first: Σ-mass
  exact across round-trips, run-twice, disturb-settle-disturb cycling.

### §3.3 Mixing = discrete species (DECIDED)

Rejected: per-particle composition vectors (+8 B/particle, per-pair diffusion cost, explicit
immiscibility logic) and the solvent+solute hybrid (parked). **A particle IS one species.**

- **Immiscibility is free** (rest-density stratification). **Miscible blending is statistical**
  (interleaved particles); true conversion is a pass-4 pair rule swapping species ids.
- In **bulk**, species are exact integer quanta per basin layer — dilution ratios live there.
- **Parked**: solvent + one solute slot, re-opened only if a game needs smooth in-flight
  concentration.

### §3.4 Erosion, sediment & authored flow (DECIDED)

- **Erosion**: sustained fast current over soft material carves it (a slow §6 rule: flow speed
  × softness → quanta removal).
- **Suspended sediment**: eroded quanta ride as granular particles; **deposit where current
  slows** (particle sleeps → re-bake, §11.1).
- **Authored flow**: coarse flow-map zones (a §5.4 field kind) for designed rivers, with
  source-at-head / sink-at-mouth.

### §3.5 Liquid rulings (closed 2026-08-22)

- **Uniform particle radius, world-wide:** rest spacing = 2 texels (0.125 m), kernel radius
  `h_kernel` = 4 texels (0.25 m) = the fine-tier cell size. A `LiquidSpecies` row cannot change it
  (per-species radii would make the fine tier species-dependent). A 1 m² pool ≈ 64 particles.
- **Viscosity = XSPH** (velocity smoothing over neighbours, weight `q_t` per species); a
  constraint-based viscosity is rejected — XSPH is one extra neighbour sweep with no λ, and its
  feel range covers water through honey. Higher viscosities are bulk behaviour (§3.2).
- Basin identification shares cavity machinery (§4.1) — decided there.

---

## §4 Gases — the cavity graph

### §4.1 Representation (DECIDED)

Gases are **not particles**. Authoritative state = the **cavity graph**: connected air pockets,
each holding **integer moles per species** + a temperature; pressure = ideal gas `P = nT/V`
(integer division, `V ≥ 1`, `P ≥ 1`). Outdoors = one infinite-reservoir cavity. Compression /
expansion falls out of V-tracking. Gas mixing inside a cavity is instant mole-fraction
bookkeeping (well-mixed by definition).

**Topology detection**: a coarse, *transient* occupancy sampling per dirty region (rebuilt,
never authoritative, lives in scratch) feeds flood-fill → cavity identify/split/merge in pass 5.
Liquid basins (§3.2) are the same machinery: a basin is a cavity's liquid-occupied partition.
The flood's *result* — a per-chunk coarse cell → cavity/basin label map — is kept in the chunk
store so floods stay chunk-local and `cavity_at` is O(1) (see §14.2 `ChunkTexels`, §14.4.5).

**Body-attached cavities (sealed envelopes — DECIDED)**: a sealed gas volume that *moves*
(balloon, submarine hull, diving bell, pressure soft-body) is a cavity record **attached to a
body** — same store, same `P=nT/V`. For a pressure soft-body the envelope is particles + an
area constraint whose `rest_area ← nT/P`; valves/pumps add or vent moles; depth pressure
compresses it — buoyancy at depth emerges. Breach = merge into the surrounding cavity (pass 5).

### §4.2 Flow = rate-limited through openings (DECIDED)

Rejected: instant equalization (gas stops being a phenomenon) and a coarse Eulerian gas grid
(reintroduces the world grid + advection cost). **Orifice-model flow on the cavity graph**:

- Each adjacency edge carries an **opening width** (from the same coarse sampling). Per tick,
  integer moles transfer ∝ ΔP × width (integer rate math, clamped, conservation exact).
- Yields hissing leaks, slow room-fills, pressure doors, bellows, mine-gas creep, breach gusts.
- Species flow together by source mole fraction (bulk flow); slow diffusion without ΔP is a
  cheap additive term.

### §4.3 Wind = flow-derived + zone fields (DECIDED)

Rejected: authored-zones-only (no emergent drafts) and a persistent coarse velocity field (extra
authoritative state; parked as a possible non-authoritative visual layer). **Chosen**:

- Wind force near an opening = derived from that opening's current flow rate (direction along
  the opening normal, falloff into the cavity). Along-tunnel currents = chained openings.
  **No separate wind state exists.**
- Heat updrafts: per-cavity buoyancy from temperature differences.
- Authored wind (weather, fans, spells) = ordinary §5.4 zone fields; machine-driven fans /
  bellows inject flow at an opening.

### §4.4 Gas rulings (closed 2026-08-22)

- **Coarse sampling = 4-texel cells (0.25 m), re-sampled only over dirty regions.** A 1-texel
  breach cannot be missed because breaches are never *discovered* by sampling: every carve,
  fracture and phase transition reports the texels it emptied (§2.1 returns removed quanta per
  texel window), and pass 5 marks those cells' cavities for re-flood the same tick. The bound is
  therefore **one tick**, by construction, not by sampling resolution; the coarse grid only
  decides merge/split *extent* and opening width.
- Sound/shockwave through cavities — **ruled as a query, not state:** `alloy_cavity_path(a, b) →
  (hops, min_opening)` over the cavity graph; games derive audibility/pressure-wave effects from
  it in Luau. No propagation state exists.

---

## §5 Fields — temperature, electricity, magnetism, radiation

Fields live on the graphs that already exist (neighbor, bond, contact, cavity) — **never on a
world grid**. All field state is plain integer quanta (carrier `temp`: i16; accumulators i32;
moles/charge: i32/i64). Transport runs in pass 1. Quanta-path coefficients (reciprocal thermal
mass, conductance, rate multipliers) are **`scalar_t`** (`FX-PALETTE.md` §9 R-5) — one row,
explicit `mul<R>` at every use, never an ad-hoc shift.

### §5.1 Temperature (DECIDED)

Carriers: **particle**, **cavity**, **solid = per bond-region**. Rejected: per-texel temp field
(re-adds the stencil cost the grid exit was for; parked as opt-in for special bodies e.g. forge
ingots) and one-temp-per-body (torching a wall heats the whole wall).

- Conduction along bond edges, persistent contacts (body↔body, body↔particle), PBF neighbors,
  region↔cavity at exposed surface, cavity↔cavity through openings (advected with flow). All
  integer transfer ∝ ΔT × coupling. Per-edge conductivity = **geometric mean `isqrt(kᵢ·kⱼ)`**
  (det integer sqrt); **precompute reciprocal thermal mass** so `ΔT = q·recip >> SHIFT` is a
  multiply-shift, never a per-carrier divide; flux written **antisymmetrically** (what leaves
  one carrier enters the other, to the quantum) — that makes Σ-energy *exact* (the
  multiply-shift is the sanctioned `quanta_mul(i32, scalar_t)` helper; regions and cavities
  carry a `heat_res` remainder and particles have thermal capacity 1 by definition, so the
  energy ledger is exact per carrier too — see §14.4.1).
- Region granularity = seam density, so heat fidelity is data-tunable per material.
- Melt/ignite thresholds trigger **per region** — the pass-4/5 hook.

### §5.2 Electricity = sources + resistance flow (DECIDED)

Rejected: binary powered/unpowered (no overload/dimming/shorts) and charge-quanta packets
(per-hop latency). **Integer Kirchhoff-lite on the conductivity graph**:

- **Circuit graph** = wire bodies + bodies in *persistent contact* whose materials conduct +
  liquid bridges (conductive species) + player-built wires as data. Persistent-contact tracking
  feeds edge creation; edges carry integer conductance.
- Sources push integer current; loads consume; current splits by conductance — iterative
  integer relaxation per dirty circuit, fixed iteration count, ordered by stable circuit id.
- **Couplings**: I²R heating → §5.1 (fuses melt, overloads ignite); electrolysis consumes
  current + water at electrodes → cavity H₂/O₂ moles; motor/generator = constraint motor whose
  torque budget ↔ circuit load; lightning = transient source along a raycast path.

### §5.3 Magnetism & radiation (DECIDED)

- **Magnetism**: signed scalar potential, monopole sources, bounded falloff with softening floor
  (`k·qᵢqⱼ/(r²+ε²)·r̂` — never raw 1/r²); applies to particles too (ferrofluid free). **Dipole =
  composition, not a tensor**: a bar magnet is one rigid body carrying two opposite offset
  pole-points — rigidity converts opposing forces into net force + emergent torque.
  Electromagnets = a source gated by §5.2 current.
- **Radiation**: raycast emission on the broadphase (occluded by solids, attenuated by
  material), deposits energy → §5.1 / §6 triggers. **Decay chains**: a radioactive material
  transforms into `decay_into` by keyed-RNG probability from half-life (no per-carrier timers);
  **contamination is a material transform**. Schema: `radioactivity` (emission, half_life,
  decay_into) + `radiation_absorption`.
- **Lightning**: scheduled weather event (keyed-RNG timing from WorldDesc climate), target by
  deterministic heuristic (tallest / most conductive / most exposed), delivering a transient
  current source + heat spike + optional strike reaction payload.

### §5.4 Force fields — taxonomy & falloff discipline (DECIDED)

Four kinds, fixed eval order, one per-carrier force accumulator (i64, rounded once):
**uniform** (gravity), **grid-sampled** (authored flow maps), **radial point** (explosions,
attract/repel), **zone** (fans, updrafts, traps). Falloff is **never raw 1/r²** — three bounded
profiles evaluated in `q_t` (r/R): linear `1−q`, smoothstep `(1−q)²`, inverse-square-with-floor
`1/(1+(r/r₀)²)`. Fans/turbines = cone with two-axis falloff. **Occluded explosions**: a
transient one-substep radial impulse, gated per in-range target by a raycast in stable-id order.

### §5.5 Wetness (DECIDED)

A **u8 scalar on solid bond-regions and agents**: deposited by liquid contact, decays by
evaporation under heat. Gates ignition, raises effective conductivity (puddle near live wire),
feeds §7 soil moisture. One byte buys three couplings.

### §5.6 Game-registered scalars (DECIDED)

The propagation primitive is exposed: a game may register its own integer scalar field on
Alloy's carrier graphs with data-defined transport coefficients (e.g. *noise* through open
cavities, muffled by rock), stepped in pass 1. Same rules; hashed with sim state. Registered
from Luau data at `init()` (§11), never at runtime.

---

## §6 Chemistry, fire & phase transitions

### §6.1 Reaction rules (DECIDED)

Three rule shapes over **carrier pairs** (pass 4, buffered edits at the pass boundary):

1. **Contact-pair**: (species A, species B, context) → products + heat, over any adjacency the
   graphs provide — particle↔particle, particle↔region, region↔cavity, bulk↔anything.
2. **Threshold**: single carrier crosses a field threshold → transform (ignite, melt, decompose).
3. **Catalyzed**: pair rule gated on a third species in the same neighborhood.

Rules are **game data** (§11). Rates are integer quanta/tick; stochastic rules draw
`rng_for(seed, tick, rule_id, carrier_id)` — a unit draw derived straight into an fx row.

**Arbitration**: the **lower stable-id carrier owns the pair and fires it exactly once** — no
double-consume. Consciously dropped shapes: *graduated-blend* (parked with the solvent slot) and
*time-gated* (covered by per-tick integer rates accumulating to thresholds).

### §6.2 Phase transitions (DECIDED — the unification crux)

Matter moves between representations only in pass 5, always in integer quanta, Σ-mass exact:

- **melt**: hot region → carve texels, emit liquid particles at surface
- **freeze**: cold particles near solid → consumed into CSG-union re-bake
- **boil**: hot particle (or bulk surface layer) → moles into its cavity
- **condense**: supersaturated cool cavity → droplet particles at cold surfaces
- **settle/disturb**: particles ↔ bulk (§3.2)
- **dissolve/precipitate**: solid region ↔ solute species in adjacent liquid (data-gated)

**Hysteresis (mandatory)**: a transition fires only ~3 quanta *past* its threshold, and the
product rebounds ~1.5 quanta back — integer, kills ice⇄water strobing. The §11 validator
enforces the paired gap rule: a product's opposite threshold must sit ≥ offset from the
source's, else the pair strobes by construction — caught at init.

**Field carryover rule**: across any transition, **temperature is the only field that
carries; everything else is a fresh instance** (fresh bonds, full integrity — melt + refreeze
*repairs* a half-carved wall; structural load recomputes). Accepted approximation: carrying
temperature rather than energy leaves a small discontinuity where thermal mass differs; the
proper fix is latent heat, deferred.

**Re-bake conflict rule**: settling granular/liquid re-bakes only into empty space; if solid
blocks the target, **no re-bake** (stay a particle, retry at next rest). Solid is never
overwritten; no matter destroyed.

### §6.2b Explosions — a composed verb (DECIDED)

Not a primitive: five mechanisms fired together — (a) radial carve with falloff, resisted by
`strength`/`hardness`, so **crater shape is emergent**; (b) heat spike; (c) the occluded radial
impulse launching debris with inherited momentum; (d) **chain reactions** — a triggered
explosive detonates its own event next tick, in id order; (e) **structural knock-on** — the hole
re-routes load, §2.3 collapses what the blast didn't touch. Two trigger paths: material-driven
and the edit-API region-impulse command. **Netcode constraint (T-A-06)**: the detonation tick
is fixed at throw/trigger time and carried in the command — the island merge is *scheduled, not
discovered*; effects that can merge more than `AOE_ISLAND_LIMIT` islands need a telegraph period
≥ the confirmation horizon (`NETCODE.md` §3).

### §6.3 Fire = burning state + spark carriers (DECIDED — hybrid)

Rejected: flame-particle fire (double bookkeeping) and pure carrier-state fire (gap-jumping and
wind-driven spread reduce to conduction hacks). **Hybrid**:

- **Burning is a state on the fuel carrier** (region, particle, or flammable gas mix): per tick
  consumes own fuel quanta + cavity O₂ moles, emits heat, product moles/smoke, light events. No
  O₂ → smolder/extinguish; water contact → temp drop kills it. Flammable-gas cavities over
  ignition temp **deflagrate** (the firedamp scenario).
- **Plus a cheap `ember` particle species**: short-lived, wind-blown (§4.3), spawned by burning
  carriers under a keyed-RNG budget, carries an ignition payload on landing. Fire **jumps gaps
  and travels downwind emergently**. Ember count budget-capped per region.
- Visual flames are render-side, derived from burning-state + ember views.

---

## §7 Vegetation = grown solid bodies (DECIDED)

Rejected: decorative fuel-maps (no chopping/farming depth) and a dedicated L-system layer (a
second world to keep deterministic). **Plants are ordinary sim citizens the sim grows**:

- A plant = articulated solid: **segments = small SDF bodies** joined by compliant bonded joints
  (§2.3 bonds carry break/chop semantics; joint compliance = sway).
- **Growth is a pass-5 rule per species** (data): consume rooted water (bulk/soil moisture via
  §6 rules), light budget (occlusion-aware raycast query), nutrients → extend a segment / add a
  branch / fruit. Threshold-driven integer quanta, `rng_for(seed, tick, SYS_GROWTH, plant_id)`.
- Free by construction: burns, chops (sever bond → wood bodies fall), collapses under snow
  load, sways in wind, withers without water (growth reverses).
- Grass/brush may use the particle pool with a rooted pin constraint — bench-gated.

---

## §8 Mechanics — constraints, motors, agents

### §8.1 Constraint layer (DECIDED)

**The seven compliant primitives** (per-constraint compliance α stored in data, α̃ = α/h² as
`stiff_t` precomputed at init; damping *mandatory* — an undamped compliant constraint oscillates
forever):

| # | Primitive | C (violation) | Modes / notes |
|---|---|---|---|
| ① | distance | `\|pᵢ−pⱼ\|−rest` | bilateral / **max-only = slack rope** / min-only |
| ② | positional pin | point-to-point / point-to-body | free (revolute) or axis-restricted (**prismatic**) |
| ③ | angular | relative-angle target (turns) | **limits + motor** |
| ④ | contact | `penetration ≥ 0` | Coulomb friction (position-level) + restitution (velocity pass) + **surface-velocity term** (conveyor) |
| ⑤ | area | `area(poly)−rest_area` | rest_area static, or gas-driven `← nT/P` (§4.1) |
| ⑥ | coupled length (pulley) | `l₁+ratio·l₂−total` | ratio = mechanical advantage |
| ⑦ | angular coupling (gear) | `ratioₐθₐ−r_bθ_b−phase` | sign = mesh vs sprocket |

**Motor** = a driver on ① or ③: position-drive or speed-drive, always with a **max-force
clamp** (stall is a feature — §5.2 circuits brown out). **Anchor** = a body with `inv_mass = 0`
exactly (costs no range).

**Composition table** (kept verbatim so nothing is re-derived): cloth = grid of
①(structural+shear)+③(bending)+corner ②; steel cable = stiff ① chain + load; bow/catapult =
compliant ③/① stores draw energy, release = drop the constraint; conveyor = ④
surface-velocity; gears = ⑦ exact-ratio, slipping belt = rope-loop ①+④; crank/piston/engine =
② revolute + ② prismatic + connecting rod ①; water wheel / windmill = ③ motor driven *by*
fluid/wind forces (generator mode → §5.2); hydraulic ram = §3.2 depth pressure at piston face →
② prismatic force; traps = pressure-plate ④ · tripwire = breakable ① · collapsing floor = §2.3
bonds · drawbridge = ⑥ + motor. Cross-domain energy chains (burn → boil → pressure → piston →
gear → generator → current → electrolysis) compose with no special cases.

**Substep loop** (8 per tick, `H = dt_t(1/480)` a rounded fx constant, `INV_H = 480` a plain
int; `h²` is never a runtime operand — α̃ is precomputed at init, `FX-PALETTE.md` §3.1): save
`x_prev/θ_prev` → integrate external forces → predict → reset λ → project constraints
color-by-color → implicit velocity `v = (x−x_prev)/h` → **separate velocity pass for friction +
restitution** → writeback. **Fixed-point discipline inside the loop** (precision ladder, PIVOT
§3.1b): solver-local positions/velocities stay i64 across the sweep and round to `pos_t`/`vel_t`
**once per substep**; λ accumulators widen likewise; every `mul<R>` rounds to nearest even; the
`w₁+w₂+α̃` denominator is bounded by construction via `MASS_RATIO_CLAMP` applied per pair
(effective inv-mass spread saturates at 4096:1; statics 0 exactly). Residual carry (rung 3) is
a standing bench variant.

**Solver kernel = colored Gauss-Seidel — DECIDED** (Gate 0 verifies; sequential GS disqualified
on perf; Jacobi needs ordered scatter-add — no longer a determinism problem in fixed point, only a
convergence one; CGS gives *structural* thread-invariance). Coloring: deterministic greedy in
stable-id order, lowest free color; **persistent constraints color once and cache, contacts
recolor each tick**. Hosted on the job system: colors are sequential levels; within a level
`parallel_for` over the color's constraint list in stable-id chunks (`JOBS.md`). Known wrinkle +
escape hatch: few colors can starve SIMD lanes — swappable to Jacobi/hybrid without touching the
skeleton. Fluid density constraints are one more type in the same solve.

Cross-couplings now internal: motor↔circuit load, pump/bellows↔cavity flow, pulley↔fracture.

### §8.2 Agent bodies — players AND animals (DECIDED)

Rejected: game-side controllers on raw bodies (both games rewrite the same ground-snap/swim
code; fluid coupling reaches into solver internals) and kinematic KCCs (lie to the sim).
**Alloy ships one `AgentBody` mechanism**:

- **A real dynamic body** (capsule, ~1.8 units tall for a human) in the solve — honest mass, so
  explosions shove it, currents drag it, collapses crush it, cargo weighs it down.
- **Driven by a `MoveIntent`** supplied per tick through the edit channel (§9.2): player input
  or game-side AI. **Alloy never decides behavior.**

```cpp
struct MoveIntent {            // WIRE_STRUCT: explicitly padded, offsets static_asserted
  Handle<AgentTag,22,10> agent;
  vel_t   desire_x, desire_y;  // desired velocity / thrust
  u16     mode_hints;          // jump, crouch, dive, glide... (bitset)
  u16     commit_ticks;        // T-A-04: window during which later intents cannot alter state
  u32     _pad;
};
```

- **`commit_ticks` (T-A-04)**: an action declares a commitment window; during it subsequent
  `MoveIntent`s are buffered but cannot alter state. Netcode-critical: misprediction probability
  is exactly 0 for the window (`NETCODE.md` §7). **Coupling recorded**: tuning the window trades
  responsiveness against rollback frequency, not just feel — never tune it blind (§8.3).
- **Three locomotion media, one mechanism** (data-tuned per species): **Ground** (contact/ground
  detection, slope + step, jump impulse budget); **Water** (§3 coupling free — buoyancy vs
  density, PBF drag in particles, analytic immersion in bulk; swim = thrust while immersed);
  **Air** (flight = sustained thrust vs gravity + drag; gliding = drag anisotropy). Mode
  transitions are *emergent from immersion/contact state*.
- Creature articulation beyond one capsule is render/game-side; a multi-segment sim creature is
  bodies + §8.1 constraints.

### §8.3 Mechanics rulings (closed 2026-08-22)

- **The entire AgentBody tuning surface is data** — `AgentSpecies` rows (§11.1): ground accel /
  decel curves (piecewise `q_t` tables), step height, jump impulse budget, coyote grace (ticks),
  swim/fly thrust, drag anisotropy, and **`commit_ticks` per action class** with engine defaults
  **0 for movement, 6 for committed actions** (= `CONFIRMATION_HORIZON_TICKS`, so a committed
  action can never be mispredicted). Game-side code only *produces* intents. The feel pass tunes
  rows, never code; the rollback coupling is recorded in the row's comment.
- **Ragdoll handoff** — ruled as a composition, not a feature: on death the game replaces the
  agent's capsule body with N segment bodies + §8.1 constraints via ordinary edit commands. No
  Alloy mechanism is added.

---

## §9 State, API & netcode posture

### §9.1 Snapshot-capable state (DECIDED)

Lockstep is the network model, but **all authoritative state is snapshot-capable**: flat POD
columns in registered arenas, **no interior pointers — handles/indices only**, so save/restore =
memcpy of the registered arena set (`MEMORY.md`). Rejected: lockstep-only (forgoes near-free
replay / save-anywhere / rollback option / desync bisection for zero savings).

- Every pool/graph (particles, bodies, SDF stores, bond/cavity/circuit graphs, constraint lists,
  bulk basins, plant/agent records, the wake queue) lives in registered arenas. Transient
  scratch (broadphase, coarse sampling, color lists, sort buffers) is in per-worker scratch
  arenas, explicitly NOT snapshotted, derivable from authoritative state.
- Desync bisection: hash per arena per tick; on divergence, binary-search the pass × pool that
  split (`DETERMINISM.md`).
- No static mutable state in any sim TU (two worlds run in one process in the run-twice test).

### §9.2 API surface (DECIDED shape; entity-agnostic, multi-mutator)

```cpp
Result<AlloyWorld*> alloy_init(ArenaRegistry* reg, VMemArena* perm, Scratch* scratch, Jobs* jobs,
                               const CompiledTables* tables, const WorldDesc* wd, u64 seed);
void                alloy_step(AlloyWorld*, const EditBuffer* cmds, u64 tick);   // the only mutator
```
(Full signatures, command and query records: §14.3. `tick` is `u64` per CANON; the world type is
`AlloyWorld` per `ECS.md` §7; the registry is `ArenaRegistry` per `MEMORY.md` §1.2.)

- **`init`**: tables are the §11 validator's POD output (compiled from Luau); fail-loud
  `Result` with a named `ErrCode`, no partial states.
- **`step(cmds, tick)`**: the tick's **ordered edit channel** — a tick-stamped command buffer.
  Producers: Luau (players' `MoveIntent`s + game/AI edit commands via bindings) and the
  netcode's confirmed `InputFrame`s mapped through the game's action mapping (Luau) into the
  same command shapes. Order key = `(tick, source_slot, seq)` where `source_slot` is peer
  index then AI channel — a pure function of the confirmed log, never of arrival. Agents and
  players mutate through one channel — no single-mutator assumption.
- **Views (read-only)**: SDF/material views per chunk/body, particle views, burning/ember
  views, cavity/basin summaries, POD state views, and **per-arena hashes at per-arena
  granularity** (T-A-05 — never one world hash; the pass × pool bisection grid and
  splice-one-arena recovery depend on it).
- **Edit commands**: carve/stamp brushes, spawn/despawn (bodies, particles, agents, plants),
  region impulse (with a scheduled detonation tick, §6.2b), swept-capsule queries, apply-heat /
  current sources, constraint create/break, `MoveIntent`.
- **Queries**: raycast/shapecast, connectivity (supported? same cavity? circuit live?),
  immersion/medium at point, light/occlusion budget. Pure reads; callable from Luau bindings.
- **Event stream**: severed, promoted, ignited, extinguished, deflagrated, phase-changed, creak,
  grew, withered, current-tripped — mechanisms emit, games assign meaning. Structure: bounded
  `RingBuffer<Event>`, overwrite-oldest, zero-alloc, drained after `step()`; **per-chunk rings
  merged in chunk order** at the barrier (the `JOBS.md` rule — never per-worker); **events are
  notifications, never authoritative, never hashed**. Continuous data is read from views, not
  events.
- **Snapshot API**: `save(ArenaSet*)` / `restore(ArenaSet*)` (whole-arena memcpy, build-hash
  stamped); `hash(tick) → per-arena hashes`. Closure-scoped restore is T-A-01.
- **Disk persistence (≠ snapshot)**: versioned save = header (format version, build
  fingerprint, data-table hash, seed, `WorldDesc`, **material/species name table**) + body of
  dirty chunks/pools. Encoding is the **ECS reflection encoder** (`ECS.md`): name-keyed fields,
  renames via alias entries, added fields via declared defaults, materials remapped by name on
  load. Every checkpoint struct is a **`WIRE_STRUCT`** (explicitly padded, `static_assert` on
  `sizeof` and every `offsetof`). Fail-loud `ErrCode` on load, no partial loads. Undirtied
  chunks are not stored — regenerated from seed (§12).
- The **two-consumer bar** applies to this whole surface.

---

## §10 Determinism & test obligations — fixed-point edition (DECIDED, Gate 0 pending)

**The contract: no floats on any sim path; every authoritative value is an fx palette row or a
plain integer; every operation is bit-exact on every ISA by construction.** Float
canonicalization, signed-zero, denormal policy, strict-FP flags, `rsqrt` estimate bans,
`@fp_pin` — all VOID. Nothing to pin.

**The re-litigated question — "why float in the solver".** The old argument: the XPBD
denominator `w₁+w₂+α̃` and the normalize `√` span feather→boulder mass ratios and
sub-texel→world-extent lengths in one expression; a moving exponent is structural; any
fixed-point scale overflows the big end or quantizes the small correction to zero. **The
answer (PIVOT §0, §3.1a/b)**: the dynamic range is *bounded by design*, then *split by domain*:
- `MASS_RATIO_CLAMP` 4096:1 as an effective per-pair inv-mass clamp (statics exactly 0) bounds
  the denominator; `invmass_t` fx<i32,18> holds it with 2× headroom.
- Per-domain Q-formats: `pos_t` fx<i32,18> (±8,192 m, 3.8 µm), `vel_t` fx<i32,20>,
  `invmass_t` fx<i32,18>, `stiff_t`/`q_t` fx<i32,30>, `angle_t` fx<i32,30> turns, `omega_t`
  fx<i32,20>. The sub-texel-correction vs world-extent spread is the `pos_t` vs correction
  split; the correction quantum (3.8 µm = 1/16384 texel) is what G-01 measures.
- Kernel normalization `q = r/h_kernel` makes PBF/SDF kernel precision world-scale-independent.
- Widened i64 intermediates, round once per substep (ladder rung 1) and RNE in `mul<R>`
  (rung 2) — truncation's systematic downward bias is the convergence killer, not the width.
- The grid's integer arguments still don't transfer to the iterative solve — which is why this
  is **decided by Gate 0 measurement** (G-01..G-06, `GATE0-BENCH.md`), not argument. Response
  to a convergence failure: climb the precision ladder rung by rung. The **pre-committed
  fallback** is pinned-toolchain float, x86-64 only, cross-ISA written off at that moment.

**Rules that stand:**
- Integer quanta are authoritative for everything conserved: Σ mass-quanta (per species),
  Σ moles, Σ charge, Σ load on the bond graph, energy bookkeeping per material — exact tests.
- **Saturating, never wrapping** in all quanta paths; the sanctioned helpers are the only
  arithmetic there. Solver paths use widened intermediates and explicit narrowing.
- Ordering is a pure function of state: id-sorted neighbor lists (§1.2), fixed pass and
  substep order, chunk-keyed job outputs folded in chunk order, keyed RNG everywhere, explicit
  sorted iteration (no `Map` walks in sim code).
- **Hash-region integrity**: mutating a transient buffer must NOT move any arena hash; mutating
  any authoritative pool MUST. Hash covers `[base, used)`; padding is explicit and zeroed.
- **slept == stepped**: sleep/dirty scheduling vs a full-scan reference → identical hash.
  Wake-completeness is a correctness property.
- **First implementation slice = a path with an exact conservation invariant** (heat flux on
  the bond graph): proves the flux+hash+merge spine before any drift-carrying code.
- **Hashes**: pinned rapidhash per arena per tick in debug/netcode builds; the pass × pool
  bisection grid is the standard desync workflow.

**The headless harness ships FIRST** (`TESTING.md`), before the substrate:
- run-twice (two worlds, one process, identical hash trace);
- worker-count invariance 1 / 2 / 8 / 16 (a blocking release gate) + one mixed-pair run;
- cross-ISA: PC x86-64 ↔ Pi 4 aarch64, Pi binary cross-compiled from the PC, identical traces;
- record→replay over the command log;
- property/fuzz per pass; round-trip tests for **every §6.2 transition** (disturb-settle-
  disturb, melt-freeze, boil-condense, dissolve-precipitate) with Σ-mass exact;
- conservation oracles (Σ quanta/moles/charge/load) on every scene;
- container property tests vs naive references (sort, SlotMap, RingBuffer).

**New instruments:**
- **FLOAT-SHADOW diagnostic build**: the solver is written over palette typedefs, so a dev-only
  config compiles it once more over `double`. Running fx and shadow side by side localizes
  precision loss to "row X, constraint Y, pass Z". **Never in netcode/ship builds; never
  authoritative**; its numbers are never compared for equality, only for drift.
- **UB discipline**: UBSan + ASan jobs in the determinism CI; Clang everywhere, pinned. Fixed
  point makes codegen differences unable to change results *except through UB* — therefore
  **a G-06 divergence is UB by definition** and is hunted with the sanitizers, not with the
  palette. No static mutable state; no uninitialized padding; no signed-overflow outside the
  saturating helpers.
- **Symbol-audit gate**: `llvm-nm` over `libsim` — any undefined symbol outside the allowlist
  fails CI (no malloc/free/new, no libm, no clock/entropy, no io). A grep line covers
  intrinsics that never become symbols.

---

## §11 Data schema — the game-data contract

### §11.0 The data path: Luau-authored, validator-compiled (DECIDED)

The game layer brings data + meaning (`LUAU-LAYER.md`). Three candidates for where Alloy's
tables are authored:

| Axis | **A. Luau tables → C++ validator → POD arenas (chosen)** | B. C++ `constexpr` tables in headers | C. External JSON/TOML parsed by C++ |
|---|---|---|---|
| Determinism | Compiled once at `init()` into id-indexed POD; table hash joins the build fingerprint — every peer provably runs identical data. Luau runs only at init (not per tick); f64 → fx conversion is validated integer-exact | Bit-exact trivially | Parser is a second number path (text→fx) to make exact; same fingerprint need |
| Performance | Zero runtime cost after init (POD, id-indexed, arena-resident) | Same | Same |
| LOC / cognitive | Validator ~600 lines; schema declared once via the §6 X-macro kind tables (reflection drives the Luau→POD walk) | Lowest, but every material tweak is C++ | Validator + a parser + a schema mirror |
| Compile / rebuild | Data changes never rebuild | Every tweak rebuilds (< 10 s, but blocks the reload loop) | Data changes never rebuild |
| Correctness / test | One validator, one fail-loud path; derivations (α̃ from α, recip thermal mass) computed in C++, never in script | `static_assert` can check ranges; mass balance across tables is awkward | Two validators (syntax + semantics) |
| Iteration | Reload at runtime in dev builds (re-run validator, swap tables at a tick boundary); Luau expressions for derived rows (ladders, ratios) | None | Reload yes; no expressions/derivation |

**A chosen.** Mechanism: Luau data files run in the throwaway **data VM** (`ASSETS-AND-DATA.md`
§3 — authored, not simulated; only its *output* is hashed; tables are built in array-part order
with explicit `id` fields) and return plain tables; the C++ validator walks them by the reflection field
tables, converts each field to its declared palette row or integer width (**rejecting any
value outside the row's range, or not integer-exact in quanta**), resolves name → id
references, and writes POD rows into a registered arena. The **compiled tables' hash joins the
build fingerprint** (`NETCODE.md` build hash: compiler, flags, source, sim bytecode, data).
Dev-build reload = re-run + re-validate + swap at a tick boundary, logged as a command so
replays reproduce it.

**Identity model (DECIDED)**: per-phase species tables linked by **into-references**, not a
unified cross-phase "substance" row. Rejected: unified substance (ice/water/steam as one row
with three phase blocks) — coherent for water, can't express the asymmetric majority (wood →
charcoal + smoke; stone → generic lava; two liquids freeze to one solid). Phase ladder = data
links (`low_into`/`high_into`), validated for mass balance.

### §11.1 The tables (quantities: palette row or integer width)

- **`SolidMaterial`** — density (i32 quanta/texel; must represent real solids exactly —
  nothing clamps at the osmium end); **hardness** (u8 ordinal tool-tier gate + dig/carve
  resistance u16); bond strength (i32 quanta) + seam recipe (§2.3); thermal (conductivity u16,
  heat capacity u16 ≥ 1); transitions (melt_temp i16 → melt_into + optional decompose_into);
  combustion (ignition_temp i16, burn_rate i16 quanta/tick, fuel yield, products); `blast_strength`
  (i32); `corrosion_rate` (u16); `radioactivity` (emission u16, half_life u32 ticks, decay_into)
  + `radiation_absorption` (u16); electrical conductance (u16); magnetic response (i16);
  contact (friction `q_t`, restitution `q_t`); grind_into; **`v_max`** (`vel_t`, T-A-02).
- **`LiquidSpecies`** — rest density (i32 quanta per unit area); viscosity (`q_t`); cohesion /
  surface tension (`stiff_t`); particle mass quanta (i32); freeze (temp → freeze_into solid);
  boil (temp → boil_into gas); conductance (u16); combustion; `v_max`.
- **`GasSpecies`** — molar mass (u16); condense (temp → condense_into liquid); combustion
  (flammability window as mole-fraction bounds LEL/UEL in `q_t`, O₂ stoichiometry, products).
  No toxicity/breathability — game meaning read off composition queries.
- **`GranularSpecies`** (sand, rubble, dust, ash + built-in `ember`) — particle mass quanta;
  friction/repose (`q_t`); settle rule (**sleep → re-bake into a SolidMaterial**, symmetric
  with settle-to-bulk); ember ignition payload + lifetime (u16 ticks); `v_max`.
- **`ReactionTable`** — rule shape; applicability mask (carrier pairs, u8); reagents → products
  (refs across any table, quanta-balanced); rate (i32 quanta/tick); heat delta (i32); optional
  probability (`q_t`); optional catalyst. Electrolysis is a threshold rule on local current at
  a conducting-solid↔liquid adjacency (the "electrode site").
- **`PlantSpecies`** — segment shape/size ladder; growth params (extend/branch/fruit
  thresholds in quanta, tropism bias `q_t`); input rates; wither rule; wood = SolidMaterial
  ref; fruit/seed = body or granular ref.
- **`AgentSpecies`** — shape (capsule dims `pos_t`), mass (i32 quanta), per-medium locomotion
  (ground accel/step/jump, swim thrust, fly thrust, drag anisotropy — `vel_t`/`q_t`), buoyancy
  density; default `commit_ticks` per action class (u16); `v_max`.
- **`ForceFieldKind`**, **`MotorKind`**, **`ProjectileKind`** — each declares `v_max` (T-A-02).
- **`WorldDesc`** — chunk layout within the ±4,096 m extent, gravity (`vel_t` per tick),
  atmosphere (outdoor composition, pressure, temp), climate/weather params (storm scheduling),
  world-gen seed + `GenRecipe` ref (§12), and the **boundary taxonomy**: per-edge `bedrock`
  (anchor + zero-flux) | `open sink` (outflow tracked so `Σ + outflow = const`) | `seam`
  (streaming continuation, §13); sky = ambient thermal drain with per-side horizon height.

**Validator rules** (all fail-loud at `init()`, `ErrCode` names the table/row/field):
dangling refs; every transition/reaction balances mass in quanta; **divisor floors** — heat
capacity ≥ 1, density ≥ 1 (vacuum is a sentinel), `P ≥ 1`; **hysteresis gap** (§6.2); **fx
range per palette row** (a value outside its row is rejected, integer-exactness enforced); the
**mass-ratio rule** (documents that pairs beyond 4096:1 behave as clamped — content is never
refused); **`v_max` fold** (T-A-02): every material/projectile/motor/force-field row states a
max propagation speed, `init()` folds them to the world `v_max` ≤ `V_MAX_WORLD` = 512 m/s and
rejects a row that cannot state one; plus a debug assert in the integrator on per-tick
displacement vs `v_max`; every persistent field has a declared default (the §9.2 disk rule).

### §11.2 Tick budget (targets; G-05 re-derives them for fixed point)

60 Hz, ~8 cores, nominal load (~20k active particles, ~2k awake bodies, ~500 dirty regions):
pass 1 fields ~0.5 · pass 2 forces ~0.5 · pass 3 solve ~4.0 (dominant by design) · pass 4
chemistry ~1.0 · pass 5 topology ~1.0 amortized · hashing/bookkeeping ~0.5 → ~8 ms sim budget.
This budget is per peer and independent of peer count. **G-05 is the measurement**: 20k
particles + 2k bodies **≤ 4 ms PC and ≤ 12 ms Pi 4** passes; > 8 ms PC at 20k is
**pivot-level** (fixed point cannot hold the primary platform — the fallback ladder fires); a
Pi-only miss **redraws min-spec** (Pi becomes a stretch peer) and never triggers the float
fallback. If 20k doesn't fit, the budget moves (counts, substeps), not the verdict. Any pass
2× over target triggers a design review, not a micro-opt hunt.

### §11.3 Resolved-by-design sweep

Electrolysis site = threshold rule on current (§11.1); region temp advects with promoted
islands (regions are body-local — trivial, test anyway); granular settle = re-bake symmetry;
contact hysteresis = a persistent contact survives an N-tick separation grace (N data-tunable)
before circuit/conduction edges drop — flicker-free by construction.

---

## §12 World-gen — ownership in three layers

Ore `noise` / `hash.coord2` no longer exist. The primitives must be re-homed.

| Axis | A. Pure C++ recipe, Luau parameters | B. Pure Luau recipe (per-texel loops in script) | **C. Hybrid: C++ kernels, Luau composition (chosen)** |
|---|---|---|---|
| Determinism | Bit-exact by construction | Deterministic in the sim VM (f64 integer range, det-math bindings) but per-texel script arithmetic is a wide surface to keep integer-exact | Kernels bit-exact in C++; the recipe only composes whole-chunk ops and integer parameters |
| Performance | Best; SIMD-able kernels | 16k texels × N layers per chunk through the interpreter — tens of ms per chunk; streaming stalls | Kernel cost ≈ A; per-chunk script overhead is a few dozen calls |
| LOC / cognitive | Every new biome is C++ | Smallest C++; recipe logic lives where designers are | ~10 C++ kernels + a small binding table; recipes in Luau |
| Compile / rebuild | Every recipe change rebuilds | Never | Kernel changes rebuild; recipe changes reload |
| Correctness / test | One language; exhaustive kernel tests | Hard to fuzz; script errors surface at chunk load | Kernels exhaustively/property tested; recipes replay-tested by chunk hash |
| Iteration | Parameters only | Full | Full composition freedom at reload speed |

**C chosen — DECIDED:**

- **C++ owns the deterministic kernels** (`src/sim/gen/`): integer value / gradient (Perlin
  class) / Worley noise over the pinned keyed hash, outputs in `q_t` or plain integers; whole-
  chunk ops — fill by threshold, stamp SDF brush, seam (pre-fracture into bond regions, §2.3),
  flood cavities, scatter (plant/ore placement by keyed RNG). Kernels are exhaustively tested
  against pinned reference traces and cross-ISA compared.
- **The game registers a `GenRecipe`**: a Luau function `recipe(seed, cx, cy, chunk)` running
  in the restricted sim VM (deterministic by construction — `LUAU-LAYER.md`; its bytecode is
  in the build fingerprint). It composes kernel calls on the chunk's buffers and sets material
  ids, seams, cavities, plant placements. It cannot reach `math`, `os`, or hash-part iteration.
- **Alloy owns *when* it runs**: chunks generate on demand at the streaming seam (§13); any
  **undirtied chunk is never stored** — regenerated bit-identically from seed.
- **Own keyed RNG family**: `gen_hash(seed, cx, cy, channel)` over the pinned hash — fully
  separate from per-tick `rng_for`; generating a chunk at tick 0 or tick 10⁶ yields identical
  content. Generation is a pure function; it runs in scratch and writes the chunk only at the
  pass-5 boundary.
- Gate: a chunk-hash replay test (generate every chunk of a test seed on PC and Pi; compare).

---

## §13 Large worlds — streaming + the analytic-idle model (design-recorded, build-deferred)

The unit of paging is the §2.2 terrain chunk (8 m, 128² texels) + its dependent stores
(regions, bonds, cavities, basins, resident particles/bodies).

- **Residency = `VMemArena` commit/decommit per chunk** (`MEMORY.md` — the arena keeps commit
  granularity explicit for exactly this): reserve address space for the full 1024² chunk grid
  once; commit pages for active chunks; decommit on serialize/unload. Stable bases mean
  nothing relocates. Only dirty chunks persist (§12 regenerates the rest).
- **Idle ≠ frozen.** A slow process (acid eating a support, load creeping toward failure, a
  gas pocket bleeding out, a basin filling) moves nothing *this* tick yet is pending. Freezing
  it is wrong; simulating it resident is waste.
- **Scheduled analytic wake events**: at idle-out, each region computes the earliest tick its
  closed-form trajectory crosses a discrete event (`T = ⌈hp/corrosion_rate⌉`, support below
  threshold, overflow `T = V/inflow`, scheduled storm — all integer) and enters a deterministic
  wake queue keyed `(tick, stable-id)` in a registered arena. It re-materializes *before* the
  event — **no discrete change ever fires across a frozen seam**.
- **Conserved-quantity relaxation**: idle regions relax heat/moles/volume to the equilibrium
  fixed by conserved totals — O(1) to evaluate at any later tick, exact.
- **Quiescence predicate**: idle-eligible ⇔ everything happening is analytically integrable
  with a computable next-event tick, and no fast/chaotic dynamics. **Graceful degradation**: a
  process that fails the predicate *stays resident* — degrade to full-sim, never to wrong.
- Thrash control: minimum wake horizon + dwell hysteresis.
- Netcode note: idle/resident status is a pure function of state, so it is identical on all
  peers; the wake queue is hashed like any arena.

---

## §14 Implementation specification

> For the implementer. Everything below is in the C++ subset (`CPP-SUBSET.md`): no STL, no
> exceptions, no inheritance; `Result<T>`; handles; arenas. Where §0–§13 left a detail open,
> §14 decides it and the owning section carries a "(see §14.x)" cross-ref. Constants named here
> and not owned by CANON live in `sim/alloy_consts.h` as `constexpr`. Arithmetic helpers named
> at each site are the `FX-PALETTE.md` §1/§4.2 and `CPP-SUBSET.md` §5 set: `mul<R>`, `div<R>`,
> `to<R>`, `sat_add/sub/mul`, `wrap_*`, `mul_widen`, `sqrt<R>`, `isqrt<R>`, `sincos`, `normalize`.
> `rne_shr(i64 x, int n)` (round-to-nearest-even right shift, i64 → i64) is added to `fx.h` by
> this spec; it is the only new foundation symbol §14 needs. `quanta_mul(i32 q, scalar_t k) →
> i32` = `sat_narrow_i32(rne_shr(mul_widen(q, k.v), 16))` is defined in `alloy_consts.h`.

### §14.1 File layout — `src/sim/`

| File | Contents |
|---|---|
| `sim/alloy.h` | the module header (`CPP-SUBSET.md` §6): `alloy_init/step/snapshot_*/events/post_restore`, `AlloyCmd`, `EditBuffer`, every query (§14.3). Includes `views.h` and `pools.h`. |
| `sim/views.h` | **the ONLY sim header render may include.** Includes only `foundation/tl_types.h`, `fx_palette.h`, `handle.h`. Defines the handle tags, `CarrierRef`, the `Particle` row (X-macro + `TL_POOL_ROW` — render reads it through `ParticleSpan`), every `*View`, `AlloyEvent`, `AlloyEventKind`. |
| `sim/pools.h` | every other pool row (§14.2), `AlloyWorld`, the arena id table in registration order, `enum AlloyErr : u16`. Each pool header states its hashing ruling and its reuse-zeroing rule (`MEMORY.md` §1.1). |
| `sim/alloy_consts.h` | grains, caps, thresholds, opening width, `quanta_mul`. |
| `sim/rng_systems.h` | `enum RngSystem : u32` — closed (§14.5). |
| `sim/solver_kernels.h` | header-only per-constraint projection formulas over the palette typedefs (§14.4.3). The FLOAT-SHADOW unit; the only solver file Gate 0 shares (§14.6). |
| `sim/alloy.cpp` | `alloy_init`, `alloy_step` (pass sequencing, arena-offset guard hooks, event-ring merge), snapshot wrappers, `alloy_post_restore` (cache rebuild). |
| `sim/tables.cpp` | Luau table → POD validator (§11), derived constants (α̃, `recip_cm`, `k_cache`, rule index, `v_max` fold, hysteresis gap). |
| `sim/broadphase.cpp` | cell keys, `sort_u32_kv` driver, fine/coarse tiers, neighbour + candidate-pair lists. |
| `sim/fields.cpp` | pass 1. |
| `sim/forces.cpp` | pass 2. |
| `sim/solver.cpp` | pass 3: contact generation, colouring, the substep loop, velocity pass, `PContact` update. |
| `sim/chemistry.cpp` | pass 4. |
| `sim/topology.cpp` | pass 5 driver: structural apply, texel union-find, promote/debris, load routing, islands/sleep, wake queue, compaction + remap. |
| `sim/sdf.cpp` | chunk/body SDF sample + bilinear gradient, carve/stamp CSG, Chamfer redistance. |
| `sim/cavity.cpp` | coarse occupancy, cavity flood/split/merge, openings, basins, orifice flow. |
| `sim/plants.cpp` | growth rules; light-budget query. |
| `sim/agent.cpp` | `MoveIntent` intake/commit window, medium detection, locomotion forces. |
| `sim/edit.cpp` | `AlloyCmd` validation, pass-0 routing, pass-5 structural application. |
| `sim/query.cpp` | raycast, shapecast, connectivity, immersion, cavity queries. |
| `sim/hash.cpp` | `ArenaHashView` writer, `alloy_snapshot_hash`. |
| `sim/gen/noise.cpp` · `gen/ops.cpp` · `gen/recipe.cpp` | §12 kernels, whole-chunk ops, the recipe call-out seam (`gen_hash`). |

**`tl_sim`** = every `.cpp` above. It links only `tl_foundation_det`; `llvm-nm --undefined-only`
over it must match the allowlist in §14.5. `tests/sim/*` link `tl_sim` directly. No `.cpp` in
`src/sim/` may include anything outside `src/foundation/` and `src/sim/`.

### §14.2 Pools

**Registered arenas, in registration order** (`registry_add` calls in `alloy_init`; this order
is the lockstep contract, `MEMORY.md` §1.2). Flags: H = `HASHED`, S = `SNAPSHOT`, G =
`GROWS_AT_BARRIER`. "Grows" = `used` may move only inside pass 5 (the one Alloy grow window,
§14.4.0).

| # | Arena id | Row | Flags | Grows | Identity |
|---|---|---|---|---|---|
| 0 | `"alloy.hdr"_id` | `AlloyHeader` (singleton) | H S | no | — |
| 1 | `"alloy.particle"_id` | `Particle` | H S G | yes | `u32` index, tick-scoped |
| 2 | `"alloy.body"_id` | `Body` | H S G | yes | `BodyHandle = Handle<BodyTag,22,10>` |
| 3 | `"alloy.constraint"_id` | `Constraint` | H S G | yes | `ConstraintHandle = Handle<ConstraintTag,22,10>` |
| 4 | `"alloy.caux"_id` | `u32` (constraint particle lists) | H S G | yes | run `[first, first+count)` |
| 5 | `"alloy.pcontact"_id` | `PContact` | H S G | yes | sorted array, key `(a,b)` |
| 6 | `"alloy.region"_id` | `BondRegion` | H S G | yes | `RegionHandle = Handle<RegionTag,22,10>` |
| 7 | `"alloy.bond"_id` | `Bond` | H S G | yes | sorted array, key `(region_a, region_b)` |
| 8 | `"alloy.chunkhdr"_id` | `ChunkHeader` | H S G | yes | resident slot `u16`; `chunk_id = cy<<10 \| cx` |
| 9 | `"alloy.chunktex"_id` | `ChunkTexels` | H S G | yes (per-chunk commit) | same slot as #8 |
| 10 | `"alloy.bodysdf"_id` | `BodySdf` (3 size classes) | H S G | yes | slot per class |
| 11 | `"alloy.cavity"_id` | `Cavity` | H S G | yes | `CavityHandle = Handle<CavityTag,16,16>` |
| 12 | `"alloy.cavedge"_id` | `CavityEdge` | H S G | yes | sorted array, key `(a,b)` |
| 13 | `"alloy.basin"_id` | `Basin` | H S G | yes | `BasinHandle = Handle<BasinTag,16,16>` |
| 14 | `"alloy.plant"_id` | `Plant` | H S G | yes | `PlantHandle = Handle<PlantTag,16,16>` |
| 15 | `"alloy.segment"_id` | `PlantSegment` | H S G | yes | `u32` slot, owned by a `Plant` |
| 16 | `"alloy.agent"_id` | `Agent` | H S G | yes | `AgentHandle = Handle<AgentTag,22,10>` |
| 17 | `"alloy.island"_id` | `IslandSet` (`parent[]` + `Island[]`) | H S G | yes | island id = root body slot |
| 18 | `"alloy.wake"_id` | `WakeEvent` | H S G | yes | sorted array, key `(tick, carrier)` |

Not registered (Alloy-owned permanent arena `perm`, never hashed/snapshotted, rebuilt by
`alloy_post_restore`): `chunk_slot_of: u16[CHUNK_GRID²]` (`0xFFFF` = not resident), the
`ArenaHashView` backing, the event ring backing. Never registered: scratch (broadphase, neighbour
lists, contacts, colour lists, sort buffers, coarse occupancy, accumulators, pending edits).

**Pool arena layout rule.** A handle pool is one `SlotMap<T,H>` whose three columns live in ONE
registered arena as arena-pushed fixed-capacity arrays, laid out `[gen: u16 × CAP] [free_list:
u32 × CAP] [slots: T × slot_cap]`, with `arena.used = offset(slots) + slot_cap × sizeof(T)`.
`slot_cap` (the high-water slot count) grows at the barrier; `gen`/`free_list` are at `CAP` from
init (6 B/slot, zeroed pages). Hash covers `[base, used)`: gen, free list, and all issued slots
including dead ones (zeroed on remove, `CONTAINERS.md` §2). Sorted-array pools (`PContact`,
`Bond`, `CavityEdge`, `WakeEvent`) are one `Array<T>` with `used = count × sizeof(T)`; they
are rebuilt by memmove insert/delete in pass 5 only. `CAP` per pool comes from the reserve table
(T-A-03); exceeding it is `TL_FATAL` naming the arena. Every pool: `TL_POOL_ROW(Name)` →
`static_assert(__is_trivially_copyable)`, `sizeof == Σ fields`, named `_padN` zeroed at
construction. Rows reused after `slotmap_remove` are zeroed by `slotmap_insert` (memset).

**`CarrierRef`** (in `views.h`): `u32 bits`; `kind = bits >> 30` (0 particle, 1 region, 2
cavity, 3 basin), `id = bits & 0x3FFFFFFF` = particle index or pool *slot index* (no
generation). A `CarrierRef` is valid within the tick it was produced; persistent references use
the full handle. Ordering of carriers = ordering of `bits` as `u32` (kind-major) — the pass-4
ownership rule compares `bits`.

```cpp
// views.h — the 32-byte hot row (§1.4). Two per 64-byte line. Hashed over [0, count).
#define TL_FIELDS_Particle(X, XA, XH) \
  X(pos_t, x) X(pos_t, y) X(pos_t, px) X(pos_t, py) X(invmass_t, inv_mass) \
  X(i32, mass_quanta) X(u16, species_id) X(u16, flags) X(i16, temp) X(u16, _pad0)
TL_POOL_ROW(Particle)   // sizeof == 32
// flags bits 0..7: PF_LIQUID 1, PF_GRANULAR 2, PF_EMBER 4, PF_CLOTH 8, PF_ASLEEP 16,
// PF_BURNING 32, PF_DEAD 64 (tombstone until pass-5 compaction), PF_PINNED 128.
// bits 8..15: age8 — rest-tick counter for non-embers (saturating, reset on motion), age for embers.
// species_id indexes the unified species id space (§14.4.4): [solid | liquid | gas | granular].

// pools.h
#define TL_FIELDS_Body(X, XA, XH) \
  X(pos_t, x) X(pos_t, y) X(pos_t, px) X(pos_t, py) X(angle_t, theta) X(angle_t, ptheta) \
  X(invmass_t, inv_mass) X(invmass_t, inv_inertia) X(i32, mass_quanta) \
  X(u16, material_id) X(u16, flags) X(u32, island) X(u32, sdf_slot) X(u8, sdf_class) X(u8, kind) \
  X(u16, sleep_ticks) X(pos_t, origin_x) X(pos_t, origin_y) X(pos_t, half_x) X(pos_t, half_y) \
  X(u32, region_head) X(vel_t, surface_v) X(u32, _pad0)
TL_POOL_ROW(Body)   // sizeof == 80
// kind: BK_RIGID 0, BK_SOFT 1 (particles + ⑤, no SDF), BK_AGENT 2, BK_SEGMENT 3, BK_STATIC 4.
// flags: BF_ASLEEP 1, BF_DIRTY_SDF 2, BF_DIRTY_TOPO 4, BF_CONDUCTS 8, BF_HAS_CAVITY 16, BF_DEAD 32.
// (origin_x, origin_y) = body-space offset of SDF texel (0,0); half_x/half_y = body-space AABB half extents.
// sdf_slot indexes alloy.bodysdf class sdf_class (0: 32², 1: 64², 2: 128² texels); 0xFFFFFFFF = none.
// region_head: first BondRegion slot owned by this body (BondRegion.next_in_owner chain), 0xFFFFFFFF = none.

#define TL_FIELDS_Constraint(X, XA, XH) \
  X(u8, kind) X(u8, flags) X(u8, color) X(u8, _pad0) X(u32, a) X(u32, b) \
  X(stiff_t, alpha_tilde) X(q_t, damping) X(lambda_t, lambda) X(lambda_t, break_lambda) \
  XA(u32, payload, 5)                                           // sizeof == 48
TL_POOL_ROW(Constraint)
// kind: CK_DISTANCE 1, CK_PIN 2, CK_ANGULAR 3, (4 = contact: never stored; see Contact), CK_AREA 5,
//       CK_PULLEY 6, CK_GEAR 7.   flags: CF_A_PARTICLE 1, CF_B_PARTICLE 2 (else body slot),
//       CF_MIN_ONLY 4, CF_MAX_ONLY 8, CF_PRISMATIC 16, CF_MOTOR 32, CF_BROKEN 64, CF_DEAD 128.
// payload (20 B, little-endian u32 words, read via memcpy into the per-kind struct in solver_kernels.h):
//   ① Distance { pos_t rest; vel_t motor_rate; lambda_t motor_max; u32 _p[2]; }
//   ② Pin      { pos_t ax, ay, bx, by; angle_t axis; }            // body-local anchors; axis used iff CF_PRISMATIC
//   ③ Angular  { angle_t target, lo, hi; omega_t motor_speed; lambda_t motor_max; }
//   ⑤ Area     { i64 rest_area /* fx<i64,36> */; u32 cavity_slot; u32 aux_first; u16 aux_count; u16 _p; }
//   ⑥ Pulley   { q_t ratio; pos_t total; u32 c, d; u32 _p; }        // endpoints a–b and c–d
//   ⑦ Gear     { q_t ratio_a, ratio_b; angle_t phase; u32 _p[2]; }
// lambda = λ carried at the end of the last substep (impact-fracture test, §2.3 path b).

#define TL_FIELDS_PContact(X, XA, XH) \
  X(u32, a) X(u32, b) X(u16, age) X(u16, grace_left) X(u16, conductance) X(u16, flags)  // 16
TL_POOL_ROW(PContact)   // persistent contact, a < b as CarrierRef bits; flags: PC_A_PARTICLE 1, PC_B_PARTICLE 2

#define TL_FIELDS_BondRegion(X, XA, XH) \
  X(u32, owner) X(i32, mass_quanta) X(i32, load) X(u32, next_in_owner) X(i16, cx) X(i16, cy) \
  X(u16, material_id) X(i16, temp) X(u8, wetness) X(u8, flags) X(i16, heat_res) X(i32, potential) // 32
TL_POOL_ROW(BondRegion)
// owner: body slot (RF_OWNER_CHUNK clear) or chunk resident slot (RF_OWNER_CHUNK set). cx, cy in owner texels.
// flags: RF_OWNER_CHUNK 1, RF_BURNING 2, RF_DIRTY 4, RF_ANCHOR 8 (bedrock sentinel), RF_DEAD 16, RF_UNPROMOTABLE 32.

#define TL_FIELDS_Bond(X, XA, XH) \
  X(u32, region_a) X(u32, region_b) X(i32, strength) X(i32, load) X(u16, k_cache) X(u16, conductance) \
  X(u8, flags) X(u8, _pad0) X(u16, _pad1)                      // 24
TL_POOL_ROW(Bond)   // region_a < region_b; k_cache = isqrt<u16>(k_a·k_b) at creation; flags: BF_SEVERED 1, BF_JOINT 2

#define TL_FIELDS_ChunkHeader(X, XA, XH) \
  X(u32, chunk_id) X(u16, flags) X(u8, dirty_x0) X(u8, dirty_y0) X(u8, dirty_x1) X(u8, dirty_y1) \
  X(u16, _pad0) X(u32, region_head) X(u32, edit_seq) X(i32, outflow_quanta) X(u32, _pad1) X(u32, _pad2)
TL_POOL_ROW(ChunkHeader)   // sizeof == 32
// flags: CH_RESIDENT 1, CH_GENERATED 2, CH_DIRTY_SDF 4, CH_DIRTY_TOPO 8, CH_DIRTY_CAVITY 16,
//        CH_EDGE_BEDROCK 32, CH_EDGE_SINK 64, CH_EDGE_SEAM 128. Dirty window inclusive, texels 0..127.

struct ChunkTexels {          // 51200 B, 64-aligned; one per resident slot; hashed whole
  i16 dist[128*128];          // signed distance, 4 frac bits, negative = inside solid; row-major, y up
  u8  mat[128*128];           // material_id low byte (solid materials are ids < 256 by validator rule)
  u16 cav[32*32];             // coarse 4-texel cell → cavity slot (bit 15 set → basin slot); 0xFFFF = solid
};

struct BodySdf { i16 dist[N*N]; u8 mat[N*N]; };   // N ∈ {32, 64, 128}; three SlotMap-free-list pools

#define TL_FIELDS_Cavity(X, XA, XH) \
  XA(i32, moles, 8) X(i32, volume) X(i32, pressure) X(u32, body) X(u32, edge_head) \
  X(i16, temp) X(i16, heat_res) X(u16, flags) X(u16, _pad0)     // 56
TL_POOL_ROW(Cavity)   // MAX_GAS_SPECIES = 8; volume in coarse cells (≥ 1); pressure = (Σmoles × temp) / volume, ≥ 1
// flags: CV_OUTDOORS 1 (infinite reservoir: moles/temp held at WorldDesc atmosphere), CV_ATTACHED 2, CV_DIRTY 4, CV_DEAD 8.

#define TL_FIELDS_CavityEdge(X, XA, XH) \
  X(u32, a) X(u32, b) X(i16, cx) X(i16, cy) X(u8, width) X(u8, dir) X(i16, flow)   // 16
TL_POOL_ROW(CavityEdge)   // a < b; (cx, cy) opening centre in coarse-cell world coords; dir 0..3; flow = moles a→b last tick

#define TL_FIELDS_Basin(X, XA, XH) \
  X(u32, cavity) XA(i32, quanta, 4) X(pos_t, level_y) X(pos_t, floor_y) X(pos_t, x0) X(pos_t, x1) \
  X(i16, temp) X(i16, heat_res) X(u16, rest_ticks) X(u16, flags)  // 48
TL_POOL_ROW(Basin)   // MAX_LIQUID_SPECIES = 4; quanta[] ordered by species rest density, densest first (layers)

#define TL_FIELDS_Plant(X, XA, XH) \
  X(u16, species_id) X(u16, flags) X(u32, root_segment) X(u16, segment_count) X(u16, stage) \
  X(i32, water) X(i32, nutrient) X(i32, light) X(u32, chunk_id) X(i32, growth_acc)   // 32
TL_POOL_ROW(Plant)
#define TL_FIELDS_PlantSegment(X, XA, XH) \
  X(u32, body) X(u32, plant) X(u32, parent_seg) X(u32, joint)   // 16; body is a BK_SEGMENT Body slot, joint a Constraint slot
TL_POOL_ROW(PlantSegment)

#define TL_FIELDS_Agent(X, XA, XH) \
  X(u32, body) X(u16, species_id) X(u8, medium) X(u8, flags) X(vel_t, intent_x) X(vel_t, intent_y) \
  X(u16, mode_hints) X(u16, _pad0) X(u32, commit_until_lo) X(u8, coyote) X(u8, jump_budget) \
  X(u8, wetness) X(u8, _pad1) X(q_t, immersion) X(q_t, ground_nx) X(q_t, ground_ny) \
  X(vel_t, pending_x) X(vel_t, pending_y) X(u16, pending_hints) X(u16, _pad2) X(u32, _pad3)
TL_POOL_ROW(Agent)   // sizeof == 56; medium: AM_GROUND 0, AM_WATER 1, AM_AIR 2; commit_until_lo = low 32 bits of the commit-end tick

#define TL_FIELDS_WakeEvent(X, XA, XH) \
  X(u64, tick) X(u32, carrier) X(u8, reason) X(u8, _pad0) X(u16, _pad1)   // 16
TL_POOL_ROW(WakeEvent)

struct IslandSet {            // arena #17: [parent: u32 × BODY_CAP][Island × BODY_CAP]; island id = root body slot
  u32*    parent;             // parent[i] == i ⇔ root; BF_DEAD bodies are singleton roots
  Island* islands;            // valid only at roots
};
#define TL_FIELDS_Island(X, XA, XH) \
  X(u32, root) X(u32, member_count) X(u16, sleep_ticks) X(u16, flags) X(u32, min_chunk_id)   // 16
TL_POOL_ROW(Island)   // flags: IF_ASLEEP 1, IF_DIRTY 2, IF_ANCHORED 4 (contains a BK_STATIC body or touches terrain)

#define TL_FIELDS_AlloyHeader(X, XA, XH) \
  X(u64, tick) X(u64, seed) X(vel_t, g_substep) X(vel_t, rest_vel_min) X(u32, particle_count) \
  X(u32, resident_chunks) X(u32, wake_head) X(u32, cmd_seq_echo) X(u32, flags) X(u32, _pad0)  // 48
TL_POOL_ROW(AlloyHeader)
```

**Scratch-only rows** (never registered; in `solver.cpp` / `broadphase.cpp`):

```cpp
struct Contact {              // 56 B, per tick, chunk-keyed lists
  u32 a, b;                   // CarrierRef-style: flags tell particle vs body slot (vs chunk: b = 0x80000000 | chunk slot)
  pos_t ax, ay, bx, by;       // lever arms in body space (0 for particles/chunks)
  q_t nx, ny;                 // contact normal, a→b, unit
  pos_t depth;                // penetration ≥ 0 at generation (margin-expanded: may be negative up to MARGIN)
  lambda_t lambda_n, lambda_t_;
  q_t friction, restitution;  // min over the material pair (contact table); restitution = max
  vel_t surface_v;            // conveyor term, b's surface velocity along the tangent
  u16 flags; u16 _pad0;       // CT_A_PARTICLE 1, CT_B_PARTICLE 2, CT_B_CHUNK 4
};
struct CellRun { u32 key; u32 begin; u32 end; };      // sorted by key; binary-searched
```

### §14.3 Public API

```cpp
// alloy.h — all tl_* C-ABI exported; every pointer argument non-null (TL_CHECK)
Result<AlloyWorld*> alloy_init(ArenaRegistry* reg, VMemArena* perm, Scratch* scratch, Jobs* jobs,
                               const CompiledTables* tables, const WorldDesc* wd, u64 seed);
void     alloy_step(AlloyWorld*, const EditBuffer* cmds, u64 tick);     // TL_CHECK(tick == hdr.tick + 1 || hdr.tick == 0)
u32      alloy_arena_count(const AlloyWorld*);                           // == 19
void     alloy_snapshot_hash(const AlloyWorld*, u64 out[19]);            // registry_hash_all restricted to Alloy's entries, in order
void     alloy_snapshot_save(const AlloyWorld*, Snapshot*);              // thin wrapper over registry_snapshot (Alloy entries)
ErrCode  alloy_snapshot_restore(AlloyWorld*, const Snapshot*);           // registry_restore + alloy_post_restore
void     alloy_post_restore(AlloyWorld*);                                // rebuilds chunk_slot_of, marks every island IF_DIRTY
Span<const AlloyEvent> alloy_events(const AlloyWorld*);                  // this tick's merged ring; valid until next alloy_step

struct EditBuffer { const AlloyCmd* cmds; u32 count; u32 _pad0; };      // sorted by (source_slot, seq) by the producer; alloy_step TL_CHECKs strict order

enum AlloyCmdKind : u8 { AC_NONE = 0, AC_CARVE, AC_STAMP, AC_SPAWN_BODY, AC_SPAWN_PARTICLES, AC_SPAWN_AGENT,
  AC_SPAWN_PLANT, AC_DESPAWN, AC_REGION_IMPULSE, AC_HEAT_SOURCE, AC_CURRENT_SOURCE,
  AC_CONSTRAINT_CREATE, AC_CONSTRAINT_BREAK, AC_SET_MOTOR, AC_MOVE_INTENT, AC_DATA_RELOAD, AC_COUNT };

// TL_WIRE_STRUCT adds the leading u32 format_version; payload is a fixed 48-byte little-endian union.
#define TL_FIELDS_AlloyCmd(X, XA, XH) \
  X(u8, kind) X(u8, source_slot) X(u16, seq) XA(u32, payload, 12)       // 4 + 4 + 48 = 56
TL_WIRE_STRUCT(AlloyCmd)
// payload shapes (each a WIRE_STRUCT of ≤ 48 B, memcpy'd in/out; unused tail zero):
//  CmdCarve      { u8 brush /*0 circle, 1 box*/; u8 _p; u16 material; pos_t x, y, rx, ry; i32 strength; u32 body /*0 = terrain*/; }
//  CmdStamp      = CmdCarve (material written where brush < 0; existing solid is never overwritten)
//  CmdSpawnBody  { pos_t x, y; angle_t theta; vel_t vx, vy; u16 material; u16 shape /*ShapeId in tables*/; u8 kind; u8 _p[3]; }
//  CmdSpawnParts { pos_t x, y; vel_t vx, vy; u16 species; u16 count /*≤ 256*/; pos_t spread; }
//  CmdSpawnAgent { pos_t x, y; u16 species; u16 _p; }
//  CmdSpawnPlant { pos_t x, y; u16 species; u16 stage; }
//  CmdDespawn    { u32 handle_bits; u8 domain /*0 body,1 agent,2 plant,3 constraint*/; u8 _p[3]; }
//  CmdImpulse    { pos_t x, y, radius, carve_radius; vel_t peak_dv; i32 heat; u64 detonate_tick; u8 falloff; u8 _p[3]; }
//  CmdHeat       { u32 carrier /*CarrierRef bits, or 0 → at point*/; pos_t x, y, radius; i32 quanta; }
//  CmdCurrent    { u32 body; i32 current; }
//  CmdConstraint { /* a Constraint row minus color/lambda: */ u8 kind, flags; u16 _p; u32 a, b; stiff_t alpha_tilde; q_t damping; lambda_t break_lambda; u32 payload[5]; }
//  CmdBreak      { u32 constraint_bits; }
//  CmdMotor      { u32 constraint_bits; vel_t rate; omega_t speed; lambda_t max_force; }
//  CmdMoveIntent = MoveIntent (§8.2, 20 B)
//  CmdDataReload { u64 table_hash; }   // dev only: tables swapped at pass 0; logged so replays reproduce it
// Spawn results are reported as EV_SPAWNED carrying {source_slot, seq} and the new handle (commands are deferred — no
// handle is returned synchronously).

// Queries — pure reads, callable between steps and from Luau bindings.
struct RayHit  { pos_t x, y; q_t nx, ny; pos_t t; u32 hit /*CarrierRef bits; 0 = none*/; u16 material; u16 _pad0; }; // 32
bool  alloy_raycast  (const AlloyWorld*, pos_t x0, pos_t y0, pos_t x1, pos_t y1, u32 mask, RayHit* out);
bool  alloy_shapecast(const AlloyWorld*, pos_t x0, pos_t y0, pos_t x1, pos_t y1, pos_t radius, pos_t half_len, u32 mask, RayHit* out); // swept capsule
// mask bits: QM_SOLID 1, QM_BODY 2, QM_PARTICLE 4, QM_LIQUID_BULK 8
bool  alloy_supported   (const AlloyWorld*, BodyHandle);                   // island IF_ANCHORED
bool  alloy_same_cavity (const AlloyWorld*, pos_t x0, pos_t y0, pos_t x1, pos_t y1);
bool  alloy_circuit_live(const AlloyWorld*, BodyHandle);                   // |potential| > 0 on any owned region
struct MediumSample { u8 kind /*0 solid,1 liquid particles,2 gas,3 bulk*/; u8 _pad0; u16 species; i32 pressure; i16 temp; u16 _pad1; u32 cavity_bits; }; // 16
MediumSample alloy_medium_at(const AlloyWorld*, pos_t x, pos_t y);
q_t   alloy_immersion(const AlloyWorld*, BodyHandle);                      // Agent.immersion / body immersed-area fraction
CavityHandle alloy_cavity_at(const AlloyWorld*, pos_t x, pos_t y);         // O(1): ChunkTexels.cav
struct CavityPath { u16 hops; u16 min_opening; };                          // hops = 0xFFFF → unreachable within max_hops
CavityPath alloy_cavity_path(const AlloyWorld*, CavityHandle a, CavityHandle b, u16 max_hops); // BFS, neighbours in CavityEdge slot order
i32   alloy_light_budget(const AlloyWorld*, pos_t x, pos_t y, u8 n_rays);  // Σ over n_rays upward rays at turns i/(2n)+1/4: 256 per unoccluded ray, attenuated by material

// views.h
struct ChunkView    { u32 chunk_id; const i16* dist; const u8* mat; const u16* cav; u32 edit_seq; };
struct BodyView     { pos_t x, y; angle_t theta; u32 handle_bits; u16 material; u8 kind; u8 flags; const i16* dist; const u8* mat; u16 w, h; pos_t ox, oy; };
struct ParticleSpan { const Particle* rows; u32 count; };
struct CavityView   { u32 handle_bits; i32 pressure; i16 temp; u16 species_count; const i32* moles; i32 volume; };
struct BasinView    { u32 handle_bits; pos_t level_y, floor_y, x0, x1; const i32* quanta; i16 temp; u16 _pad0; };
struct BurnView     { u32 carrier; pos_t x, y; u16 intensity; u16 material; };
struct ArenaHashView{ u32 count; const NameHash* ids; const u64* hashes; };      // 19 entries, registration order
bool alloy_view_chunk (const AlloyWorld*, u32 chunk_id, ChunkView*);
bool alloy_view_body  (const AlloyWorld*, BodyHandle, BodyView*);
ParticleSpan alloy_view_particles(const AlloyWorld*);
bool alloy_view_cavity(const AlloyWorld*, CavityHandle, CavityView*);
bool alloy_view_basin (const AlloyWorld*, BasinHandle, BasinView*);
u32  alloy_view_burning(const AlloyWorld*, BurnView* out, u32 cap);             // regions (slot order) then particles (index order)
ArenaHashView alloy_view_hashes(const AlloyWorld*);                               // last LAST-phase values, written by hash.cpp

enum AlloyEventKind : u8 { EV_NONE = 0, EV_SPAWNED, EV_DESPAWNED, EV_SEVERED, EV_PROMOTED, EV_DEBRIS, EV_IGNITED,
  EV_EXTINGUISHED, EV_DEFLAGRATED, EV_PHASE_CHANGED, EV_CREAK, EV_GREW, EV_WITHERED, EV_CURRENT_TRIPPED,
  EV_SETTLED, EV_DISTURBED, EV_CAVITY_MERGED, EV_CAVITY_SPLIT, EV_DETONATED, EV_WOKE, EV_COUNT };
struct AlloyEvent { u8 kind; u8 source_slot; u16 seq; u32 a; u32 b; pos_t x, y; i32 amount; };  // 24 B; a/b = CarrierRef or handle bits
```

`ERR_ALLOY_*` occupies `0x0A00–0x0AFF` of `ErrCode`: `ALLOY_ERR_TABLE_DANGLING_REF`,
`_TABLE_RANGE`, `_TABLE_NOT_INTEGER`, `_TABLE_MASS_BALANCE`, `_TABLE_DIVISOR_FLOOR`,
`_TABLE_HYSTERESIS_GAP`, `_TABLE_VMAX_MISSING`, `_TABLE_TELEGRAPH_SHORT`, `_TABLE_SPECIES_CAP`,
`_ARENA_RESERVE`, `_CMD_ORDER`, `_CMD_KIND`, `_CMD_RANGE`, `_SNAPSHOT_FINGERPRINT`.

### §14.4 Per-pass algorithms

Grains (per-call-site constants, `JOBS.md` R-1): `GRAIN_PARTICLE = 1024`, `GRAIN_BODY = 64`,
`GRAIN_CONSTRAINT = 256`, `GRAIN_CONTACT = 256`, `GRAIN_REGION = 128`, `GRAIN_CHUNK = 1`,
`GRAIN_CELLRUN = 256`. "Chunk-keyed" below always means the `parallel_for` chunk index, never a
terrain chunk; per-chunk outputs are scratch arrays indexed `[chunk]` and folded `0..chunk_count`.
A loop's order is stated once in brackets: `[index ↑]`, `[slot ↑]` (0..slot_cap, dead skipped),
`[key ↑]`, `[command order]`.

**§14.4.0 `alloy_step` skeleton**

```
alloy_step(w, cmds, tick):
  hdr.tick = tick; guard_begin(reg)                      // record used of every registered arena
  pass0_intake(cmds)          [command order]            // validate (ERR → TL_FATAL in debug, skip + EV_NONE in release);
                                                         // AC_MOVE_INTENT → Agent (commit window, §14.4.2); AC_HEAT/AC_CURRENT → scratch source lists;
                                                         // AC_REGION_IMPULSE with detonate_tick == tick → scratch impulse list, else → wake queue (pass 5);
                                                         // structural kinds (carve/stamp/spawn/despawn/constraint/motor/data_reload) → scratch deferred list (copied)
  broadphase_build()                                     // §14.4.B
  pass1_fields(); pass2_forces(); pass3_solve(); pass4_chemistry()
  guard_window_open(reg)      pass5_topology()   guard_window_close(reg)     // the ONLY grow window
  merge_event_rings()         [chunk ↑, record ↑]        // per-chunk scratch rings → perm ring, overwrite-oldest
  guard_end(reg)                                         // only scratch moved outside the window; TL_FATAL otherwise
```
Structural edits therefore land at the end of the tick they were issued in; the next tick's
solve sees them. Chemistry edits (pass 4) apply at the head of pass 5, after the command list.

**§14.4.B Broadphase** (`broadphase.cpp`, all scratch)

```
fine_key(pos_t x, y):  cx = u32((x.v >> 16) + 32768); cy = u32((y.v >> 16) + 32768)   // 4-texel cell = raw 1<<16; arithmetic shift (C++20)
                       return (cy << 16) | cx                                            // row-major, u32; u16 each by the world extent
coarse_key(x, y):      cx = u32((x.v >> 21) + 1024); cy = ...; return (cy << 16) | cx   // 8 m cells (= chunk size)

1. keys[i] = fine_key(P[i].x, P[i].y) for live particles [index ↑]   (parallel_for GRAIN_PARTICLE; PF_DEAD → key 0xFFFFFFFF)
2. sort_u32_kv(keys, idx, n, scratch)   // LSD radix base 256, 4 passes, stable: idx was pushed [index ↑] ⇒ ID-sorted within a cell for free
3. runs = scan sorted keys [k ↑] → CellRun{key, begin, end}; drop key 0xFFFFFFFF
4. neighbour lists, two phases, both parallel_for(n, GRAIN_PARTICLE) with outputs keyed by particle index:
   count phase: for particle i: for (dy, dx) in [(-1,-1),(-1,0),(-1,1),(0,-1),(0,0),(0,1),(1,-1),(1,0),(1,1)]:
       run = bsearch(runs, key + dy·65536 + dx); for j in run [sorted ↑]: if j != i && dist2(i,j) < H_KERNEL²: cnt[i]++
       dist2 = dot<fx<i64,36>>(d, d) with d = (P[j].x − P[i].x, ...) plain i32 sub (|world| ≤ 4096 m ⇒ no overflow)
       H_KERNEL² = fx<i64,36> constant (4 texels)²;  cnt[i] = min(cnt[i], MAX_NEIGHBOURS = 64)
   prefix sum sequential [index ↑] → nbr_begin[i]; fill phase repeats the walk, writes nbr[], then insertion-sorts each list by j ↑
5. body AABBs [slot ↑]: world AABB from (x, y, theta, half_x, half_y) via sincos + |rotate| bounds, expanded by MARGIN
   MARGIN = pos_t(V_MAX_WORLD × H × 8) = pos_t(8.534 m) rounded up to a texel multiple; coarse keys for every cell the AABB covers
   → (key, body slot) pairs, sort_u32_kv, runs; body–body candidates: for each run, pairs (a < b) [a ↑, b ↑]; pairs
   written as u64 (a << 32 | b), sort_u64_kv, unique → candidate list [key ↑]
6. body–particle candidates: for body [slot ↑]: fine-cell range of its AABB [cy ↑, cx ↑] → runs → particle indices [↑]
7. chunk–particle and chunk–body need no pairs: the chunk SDF is sampled directly in contact generation
```

**§14.4.1 Pass 1 — fields** (`fields.cpp`). Carrier accumulators `acc[c]: i32` in scratch,
indexed by carrier kind/id (particles `[index]`, regions `[slot]`, cavities `[slot]`, basins
`[slot]`), zeroed per tick.

```
Heat edges, evaluated in this fixed order, each edge once:
  E1 bonds          [Bond slot ↑]           carriers (region_a, region_b), k = bond.k_cache
  E2 PContacts      [PContact ↑]            (a, b), k = isqrt<u16>(mul_widen(k_a, k_b)) computed at PContact creation in pass 5 and stored in PContact.conductance (thermal; the electrical conductance is the material-pair table lookup at use)
  E3 PBF neighbours [index ↑, nbr ↑, j > i] (particle i, particle j), k = species table k_pair[si][sj] (precomputed u16)
  E4 region↔cavity  [region slot ↑]         exposed regions only (RF flag set by pass 5 when the region owns a surface texel adjacent to a non-solid coarse cell); k = k_surface[material]
  E5 cavity↔cavity  [CavityEdge ↑]          k = edge.width × K_OPENING, plus advection: moles moved last tick carry temp (§14.4.5)
  E6 sky drain      [region slot ↑ with RF exposed and y above horizon]  k = K_SKY, T_sky from WorldDesc
Per edge:   dT  = i32(T_j) − i32(T_i)                                       (i16 → i32, no overflow)
            q   = quanta_mul(dT, to<scalar_t>(k))                            // k as scalar_t: u16 conductance / 2^16 precomputed in tables as scalar_t
            acc[i] = sat_add(acc[i], q); acc[j] = sat_sub(acc[j], q)        // antisymmetric, to the quantum: Σ_edges q == 0 exactly
Apply [carrier kind ↑, id ↑]:
  particle:  T += i16(clamp(acc, −32768, 32767))                            // particle capacity = 1 by definition (species heat capacity scales its k_pair at init)
  region/cavity/basin:  e = acc + heat_res; T += i16(floor_div(e, cap)); heat_res = i16(e − cap·floor_div(e, cap))   // cap = material heat_capacity (u16 ≥ 1) → exact energy ledger
  clamp T to the i16 range with saturation; an overflow is a validator bug (rates are bounded by the tables)
Electricity (dirty circuits only; a circuit = an island whose bodies have BF_CONDUCTS, processed [island root slot ↑]):
  nodes = regions of the island's conducting bodies [region slot ↑]; edges = bonds with conductance > 0, PContacts with conductance > 0
  sources = scratch current list [command order] (CmdCurrent → region_head of the body)
  CIRCUIT_ITERS = 16 Jacobi sweeps, each [node ↑] reading old potentials into new_pot (scratch):
      num = Σ_j mul_widen(g_ij, pot_j) + (i64(I_src_i) << 16);  den = Σ_j g_ij (u32, ≥ 1 by construction: isolated nodes skipped)
      new_pot_i = i32(rne_div(num, i64(den)));  then pot ← new_pot                            // RNE, never floor/truncate (FX-PALETTE.md §9 R-6)
  edge current I = i32(rne_shr(mul_widen(g_ij, pot_i − pot_j), 16)) [edge ↑]; I²R heat = quanta_mul(sat_mul(I, I), r_ij) into acc (a second, ordered accumulate → applied with the same Apply rule);
  |I| > trip threshold → EV_CURRENT_TRIPPED; loads (motors) read the edge current as their torque budget (stored on the Constraint ③ motor_max via min())
Registered game scalars (§5.6): the same edge walk E1–E5 over the game's coefficient table, one scalar per carrier in a parallel column (`alloy.hdr` declares the count; the columns live in arena #6/#1 as extra fields only when declared — v0: none).
Wetness/decay/radiation: [region slot ↑]: wetness −= evaporation(T) (u8 saturating); radioactive materials: rng_q(rng_for(seed, tick, SYS_DECAY, region_slot)) < p_tick → queue transform edit.
```

**§14.4.2 Pass 2 — forces** (`forces.cpp`). Per-carrier Δv accumulators `adv[c]: vec2<i64>` in
`fx<i64,36>` (= `vel_t` raw `<< 16`), scratch, zeroed. Field kinds in this fixed order:

```
F1 uniform (gravity):    every non-static particle/body: adv += (0, i64(G.v) << 16)          // G = hdr.g_substep (per substep), mass-independent
F2 grid-sampled flow maps [map ↑]: carriers inside the map's AABB: v_map = bilinear(q_t weights) → adv += i64(mul<vel_t>(flow_gain_q, v_map).v) << 16
F3 radial point [impulse list, command order] then [radial table rows ↑]:
     r2 = dot<fx<i64,36>>(d, d); if r2 ≥ R² skip; r = sqrt<pos_t>(r2); q = div<q_t>(r, R)
     f = falloff(q) per CmdImpulse.falloff: 0 linear (1 − q), 1 smoothstep mul<q_t>(1−q, 1−q), 2 floor: div<q_t>(ONE, ONE + mul<q_t>(k, k)) with k = div<q_t>(r, r0)
     occlusion: if the row is flagged occluded, alloy_raycast(centre → carrier) [in-range targets walked by CarrierRef bits ↑]; hit ⇒ skip
     dv = mul<vel_t>(to<scalar_t>(inv_mass), mul<vel_t>(f, peak_dv))                        // invmass_t → scalar_t RNE narrow (range ±8192 fits)
     n = normalize(d) (r == 0 ⇒ n = (0, 1));  adv += (i64(mul<vel_t>(n.x, dv).v) << 16, ...)
F4 zone [zone ↑]: cone/box membership test in q_t, same dv form with the zone's direction
Magnetism [pole source ↑, target carriers ↑]: k·qᵢqⱼ/(r²+ε²): den = r2 + EPS2 (fx<i64,36>), num = K_MAG·qᵢqⱼ as fx<i64,36>; f = q_t(i32(rne_div(num * (i64(1) << 30), den))) clamped to ±ONE, then as F3   // FX-PALETTE.md §9 R-6: one rne_div on raw bits; needs |num| < 2^33 raw, which the K_MAG validator bound must guarantee (alloy-fields lane asserts it)
Buoyancy/drag:
  body in basin b [body slot ↑]: imm = immersed-area fraction (q_t; capsule/AABB vs level_y analytic); ratio = scalar_t density_ratio[material][species] (tables)
      adv.y −= i64(mul<vel_t>(ratio, mul<vel_t>(imm, G)).v) << 16;  drag: v = mul_int<vel_t>(x − px, INV_H) (tick-level implicit velocity); adv −= (mul<vel_t>(drag_q × imm, v).v) << 16
  particle drag in gas: adv −= (mul<vel_t>(DRAG_GAS_Q, v).v) << 16 for PF_EMBER/PF_GRANULAR only; wind: opening flow → v_wind at the carrier (falloff F3-style from each CavityEdge with |flow| > 0, [edge ↑])
  cavity buoyancy on bodies with an attached cavity: Δρ from P, T vs surrounding cavity (integer), applied as F1-style uniform within the body
Agent locomotion [agent slot ↑] (agent.cpp): medium from immersion/ground contact (previous tick's PContact set);
  desired accel = table curve lookup (piecewise q_t) on (intent − v), capped per medium; adv += ...; jump: one-shot budget → adv.y += jump_dv
  commit window: intent fields are replaced by a new MoveIntent only if tick ≥ commit_until (mod 2³² compare); otherwise stored in pending_* and applied at window end
Round once [carrier ↑]: dv_ext[c] = vec2<vel_t>(rne_shr(adv.x, 16), rne_shr(adv.y, 16))   // scratch, consumed by the substep predict
```

**§14.4.3 Pass 3 — solve** (`solver.cpp`, kernels in `solver_kernels.h`)

```
Contact generation (once per tick, reused by all 8 substeps — §1.2):
  C1 particle–chunk   [index ↑]: d = sdf_sample(chunk_at(x, y), x, y) (bilinear, i16/16 texel → pos_t via exact shift: 1 texel = raw 1<<14, 1/16 texel = 1<<10);
                                 if d < MARGIN: normal = bilinear gradient (q_t, normalize of two i32 differences), depth = −d → Contact(CT_B_CHUNK)
  C2 particle–body    [body slot ↑, candidate particle ↑]: world→body: p' = rotate(p − pos, −theta) via sincos + rotate; sample BodySdf; as C1, lever arm b = p' − origin
  C3 body–chunk       [body slot ↑]: sample the body's surface points (texels with |dist| < 1 texel, [texel row-major ↑], stride SURF_STRIDE = 2) against the chunk SDF; keep the DEEPEST_K = 4 deepest
  C4 body–body        [candidate pair ↑]: a's surface points against b's SDF and b's against a's; deepest 4 each way
  All contacts written to per-chunk scratch lists keyed by the parallel_for chunk; folded [chunk ↑] into one array; then stable-sorted by (a, b) via sort_u64_kv
Colouring: persistent constraints: cached Constraint.color (recoloured only when CF_DIRTY_COLOR is set by pass 5); contacts: every tick.
  greedy [constraint slot ↑ then contact ↑]: colour = lowest colour not used by any constraint sharing a carrier (carrier→colour bitmask u64 in scratch; > 64 colours ⇒ TL_FATAL — a content bug)
  level lists: Level[c] = array of (kind, index) in the same order; parallel_levels(jobs, n_colors, levels, project_chunk, ctx) with grain GRAIN_CONSTRAINT
Substep loop, s = 0..7:
  S1 predict [particle index ↑ ∥ body slot ↑] (parallel_for):
       xs = x (pos_t, saved as x_start in scratch);  v = mul_int<vel_t>(x − px, INV_H) + dv_ext (plain vel_t add; validator bounds |v| < 2·V_MAX)
       xl = fx<i64,30>(i64(x.v) << 12) + (i64(mul<pos_t>(v, H).v) << 12)        // solver-local widened position
       bodies also: θl = i64(theta.v) + i64(mul<angle_t>(omega + dω_ext, H).v), omega = mul_int<omega_t>(theta − ptheta, INV_H) (the angle subtraction wraps by policy)
       px = xs  (prev ← start of this substep; velocity is now implicit in xl − xs)
  S2 reset λ: every constraint/contact lambda = 0 (parallel_for over the level lists)
  S3 project, colour by colour (parallel_levels); within a colour each constraint reads/writes only its own carriers' xl/θl — the colouring guarantees no two constraints in a level share a carrier:
     generic XPBD step (solver_kernels.h), all i64 at frac 30 unless stated:
       C      : violation (pos_t, frac 18) from the kernel (distance: sqrt<pos_t>(dot<fx<i64,36>>(d,d)) − rest, d from xl rounded to pos_t for the kernel input only)
       grad   : ∇C per carrier as vec2<q_t> (unit) and, for bodies, the angular term r×n via cross<pos_t>(r_world, n) → pos_t
       w_eff  : pair clamp — wa' = max(wa, wb >> 12), wb' = max(wb, wa >> 12) unless the raw w is 0 (static stays 0)   // MASS_RATIO_CLAMP = 2¹²
                 body: w += rne_shr(mul_widen(i32(rne_shr(i64(rn.v) * rn.v, 18)), inv_inertia.v), 18)  // (r×n)² at frac 18, × invmass frac 18 → frac 18
       den    : i64 = i64(wa'.v) * 4096 + i64(wb'.v) * 4096 + i64(alpha_tilde.v)                  // frac 30; < 2^45; never 0 unless both static (skipped). Multiplies, never << of a signed value (CPP-SUBSET.md §5)
       num    : i64 = −i64(C.v) * 4096 − rne_shr(i64(alpha_tilde.v) * i64(lambda.v), 16)             // frac 30: |C|·2^12 < 2^43, stiff(30)×lambda(16) >> 16 < 2^46 ⇒ |num| < 1.125·2^46
       dλ     : lambda_t = lambda_t(i32(rne_div(num * (i64(1) << 16), den)))                        // ONE rne_div on the raw i64 bits, RNE (FX-PALETTE.md §9 R-6); num·2^16 < 2^63 by the bound above; |dl| ≤ 2^31 asserted
       unilateral kinds (contact, max-only, min-only, density): dl = max(dl, −lambda) ⇒ λ ≥ 0
       lambda += dl (i32 add; range ±32768 asserted in debug)
       Δ      : per carrier: mag = rne_shr(mul_widen(w'.v, dl), 4)  // invmass(18)×lambda(16) = 34 → frac 30
                 xl += (rne_shr(mag * n.x.v, 30), rne_shr(mag * n.y.v, 30));  bodies also θl += rne_shr(mag_ang * ..., 30) with the angular share
       damping: the §8.1 mandatory term: dl_damp = −rne_shr(mul_widen(damping.v, i32(rne_shr((xl − xs·2^12)·n, 12))), 30) folded into num before the divide (XPBD β form)
     kernels (solver_kernels.h, one inline fn each, signature `void project_<kind>(const KindPayload*, CarrierRefs, SolverLocal*, Scratch*)`):
       ① distance: C = |d| − rest; CF_MAX_ONLY: skip if C ≤ 0; CF_MIN_ONLY: skip if C ≥ 0; motor: rest += mul<pos_t>(motor_rate, H) clamped by motor_max on λ
       ② pin: C = |pa_world − pb_world| with anchors rotated by sincos; prismatic: project only the component ⟂ axis
       ③ angular: C = wrap_sub(θb − θa, target) as angle_t (turns, masked); limits lo/hi unilateral; motor target += mul<angle_t>(motor_speed, H)
       ⑤ area: shoelace over the aux particle run [aux ↑] in fx<i64,36> (Σ cross<fx<i64,36>>); C = area − rest_area (fx<i64,36> → pos_t-scaled via >> 18); ∇C_i = perp(x_{i+1} − x_{i−1}) in q_t; rest_area ← (nT/P) when cavity_slot valid (integer, computed in pass 5)
       ⑥ pulley: C = l1 + mul<pos_t>(ratio, l2) − total; four carriers; ∇ = ±n1 on a,b, ±ratio·n2 on c,d
       ⑦ gear: C = mul<angle_t>(ratio_a, θa) − mul<angle_t>(ratio_b, θb) − phase; angular-only
       ④ contact (from Contact rows): C = −depth_now where depth_now = −sdf re-evaluated from xl? NO — depth_now = depth − dot<pos_t>(Δx_rel, n) (linearised; the SDF is not re-sampled inside substeps); unilateral
           friction (position level, after the normal step): Δp_t = tangential part of ((xl_a − xs_a) − (xl_b − xs_b)) − mul<pos_t>(surface_v, H)·t
                 lim = mul<pos_t>(friction, |Δx_n this step|); if |Δp_t| ≤ lim: correct −Δp_t fully (static); else correct −Δp_t · div<q_t>(lim, |Δp_t|) (dynamic); split by w'
       density (PBF, one "constraint" per liquid particle i, coloured as a contact; its carriers = i ∪ nbr[i] — conflicts resolved by colouring on the owner i only; neighbours are READ at their xs (start-of-substep) positions, so levels are Jacobi w.r.t. neighbours — deterministic):
           q_ij = div<q_t>(r_ij, H_KERNEL) with r_ij = sqrt<pos_t>(dot<fx<i64,36>>(d, d)); W = (1−q²)³ in q_t (three mul<q_t>); ∇W = (1−q)² (spiky) × n_ij
           rho = Σ_j mul_widen(kw[si][sj].v, W.v) (i64, frac 60 → rne_shr 30 → frac 30) + self term kw[si][si]  [nbr ↑]
           C = max(q_t(rho − ONE), 0)   (unilateral, ρ/ρ₀ − 1 with kw normalised so rho == ONE at rest)
           ∇C_i = Σ_j kw·∇W·n_ij (i64 frac 30, rne_shr once); ∇C_j = −kw·∇W·n_ij;  den = Σ_j w_j|∇C_j|² + w_i|∇C_i|² + α̃ (i64 frac 30: |∇C|² frac 60 → >>30, × w frac 18 → >>18)
           dλ as generic; Δx_i = mul<pos_t>(∇C_i, mul<pos_t>(w_i, dλ)) applied to xl_i only (owner-only write; the symmetric Δx_j is applied when j is the owner — the standard PBF λ_i + λ_j form, realised over two constraints)
  S4 writeback, single round [index ↑ ∥ slot ↑] (parallel_for):
       x = pos_t(i32(rne_shr(xl, 12)))   // THE one round per substep
       v = mul_int<vel_t>(x − px, INV_H)  // px == xs
  S5 velocity pass (parallel_for over contacts by colour again, then XSPH):
       restitution: vn = dot<vel_t>(v_a − v_b, n); vn_prev from the pre-solve velocities saved in S1; if vn_prev < −rest_vel_min: Δvn = −vn + max(mul<vel_t>(restitution, −vn_prev), 0); split by w' → v (i64 accumulate in scratch `vacc`, frac 36)
       XSPH [index ↑, nbr ↑]: vacc_i += Σ_j mul_widen(W.v, (v_j − v_i).v) × c_visc[si] — reads v (not vacc): Jacobi; i64 then one rne_shr(…, 30 + 16)
       v_final = vel_t(rne_shr(vacc, 16)) per carrier; validator v_max assert (debug) per species/material
       px = x − mul<pos_t>(v_final, H)    // velocity re-encoded implicitly; x untouched
       bodies: ptheta likewise from ω_final
After the loop: PContact update: merge this tick's sorted Contact list with the PContact array [key ↑] — present: age = sat_add(age, 1), grace_left = N_grace; absent: grace_left −= 1, drop at 0 (memmove in pass 5 — pass 3 only marks); new: inserted in pass 5 (PContact grows at the barrier). Constraint.lambda ← last substep λ; CF flags for |λ| > break_lambda set here, severed in pass 5.
Sleep: particle age8 += 1 if |v| < SLEEP_V (vel_t) else 0; body sleep_ticks likewise; islands sleep when every member ≥ SLEEP_TICKS = 30 (pass 5 decides; sleeping islands' constraints are skipped in S3 by level-list exclusion — the exclusion is a pure function of IF_ASLEEP, so `slept == stepped` holds only if waking is complete: any contact/force/edit touching a sleeping island clears IF_ASLEEP in the same tick before S3 (pass 2 and contact generation both mark)).
```

**§14.4.4 Pass 4 — chemistry** (`chemistry.cpp`). Unified species id space: `sid = [solid 0..S) ∪
[liquid S..S+L) ∪ [gas ..) ∪ [granular ..)`, `MAX_SPECIES_TOTAL = 256`. `rule_of[256][256]: u16`
(0 = none) built at init from contact-pair rules; `threshold_rules[sid]` a run of rule ids.

```
Pair walk, owner = lower CarrierRef bits, each pair exactly once, outputs into per-chunk scratch edit lists (parallel_for over owners):
  P1 particle–particle [index ↑, nbr j > i ↑]           pair (sid_i, sid_j)
  P2 particle–region via Contact rows with CT_B_CHUNK/body [contact ↑]: region = region_of(texel) (ChunkTexels/BodySdf mat + region lookup through the owner's region chain by cx/cy nearest — precomputed per chunk in pass 5 as a 32×32 `u16 region_cell[]` in scratch)
  P3 region–cavity [region slot ↑ with exposed flag]     pair (material sid, dominant gas sid by moles)
  P4 basin–anything: basin surface layer vs the cavity above it [basin slot ↑]; basin vs immersed region [PContact ↑ with a basin carrier]
  rule = rule_of[sa][sb]; if rule == 0 continue; check rule.min_temp ≤ T_owner; catalyzed: scan nbr[owner] [↑] for catalyst sid;
  stochastic: rng_q(rng_for(seed, tick, SYS_CHEM, owner_bits, draw++)) < rule.p   (draw is a local counter per owner within the tick)
  emit ChemEdit { kind, owner, other, rule } to the owner's chunk list [record ↑]
Threshold walk [carrier kind ↑, id ↑]: for each rule in threshold_rules[sid]: fire if T ≥ rule.t_high + HYST_UP (3 quanta) or T ≤ rule.t_low − HYST_UP; product temperature rebounds by HYST_BACK (1.5 quanta → stored as 3 half-quanta: product T = threshold ∓ 2 after integer rounding toward the threshold — the validator's paired-gap rule guarantees no strobing)
Burning carriers [kind ↑, id ↑]: consume min(burn_rate, fuel, O₂ moles available in the cavity × stoich) — all sat ops; emit heat edit, product moles edit, smoke/ember spawn edits under the region's ember budget (rng_for(seed, tick, SYS_EMBER, carrier_bits)); no O₂ → EV_EXTINGUISHED edit
Gas deflagration [cavity slot ↑]: mole fraction (q_t = div<q_t>(moles_f, Σmoles)) within [LEL, UEL] and T ≥ ignition → EV_DEFLAGRATED edit (consumes fuel+O₂ moles, heat spike, radial impulse at the cavity centroid next tick via the wake queue)
Edits are never applied in pass 4. The lists are folded [chunk ↑, record ↑] into one scratch array consumed by pass 5 step 2.
ChemEdit kinds: CE_SWAP_SPECIES, CE_MASS_DELTA, CE_HEAT_DELTA, CE_EMIT_PARTICLES, CE_EMIT_MOLES, CE_CARVE_TEXELS, CE_TRANSITION(melt/freeze/boil/condense/dissolve/precipitate), CE_IGNITE, CE_EXTINGUISH, CE_SEVER_BOND.
```

**§14.4.5 Pass 5 — topology** (`topology.cpp`, `sdf.cpp`, `cavity.cpp`). Sequential order of
sub-steps is fixed; each sub-step may `parallel_for` internally with chunk-keyed outputs.

```
T1 structural commands [command order]: carve/stamp (sdf.cpp: CSG min/max over the brush window on chunk or body texels; returns removed quanta per material → EV + region mass_quanta sat_sub; marks dirty window ∪, CH_DIRTY_*), spawn (SlotMap insert; EV_SPAWNED{source_slot, seq, handle}), despawn (BF_DEAD/PF_DEAD marks; constraints referencing the handle → CF_DEAD), constraint create/break, motor set, data reload (swap tables pointer; fingerprint log)
T2 chemistry edits [chunk ↑, record ↑]: apply; CE_TRANSITION executes the §6.2 moves in integer quanta:
     melt: carve the region's texels above melt (window = region bbox) → liquid particles of melt_into at the surface texels [texel row-major ↑], N = quanta / particle_mass, remainder stays as region mass
     freeze: particle marked PF_DEAD, its quanta → CSG-union stamp of freeze_into at its cell (re-bake conflict rule: if the target texel is solid, no re-bake — the particle stays)
     boil: particle PF_DEAD → moles = quanta / molar_mass into cav[cell]; remainder stays
     condense: cavity moles → droplet particles at cold surface texels of exposed regions [region slot ↑, texel ↑], positions rng_for(seed, tick, SYS_CONDENSE, cavity_slot, draw)
     dissolve/precipitate: region quanta ↔ basin quanta[species] (sat ops)
     temperature carries; everything else is a fresh instance (§6.2)
T3 settle/disturb (cavity.cpp): basin b with all particles PF_ASLEEP for K = SETTLE_TICKS (60): delete particles (PF_DEAD), Σ quanta per species → Basin.quanta (exact); level_y from the occupancy column sums over the basin's coarse cells; EV_SETTLED
     disturb: any Contact/carve/impulse/chem touching a basin's footprint → emit particles in the disturbed window only: count = window quanta / particle_mass, positions rng_for(seed, tick, SYS_BASIN, basin_slot, draw) + slot grid; Basin.quanta sat_sub; EV_DISTURBED
     granular re-bake: PF_GRANULAR particle asleep ≥ SETTLE_TICKS with an empty target texel → stamp, PF_DEAD; else stays
T4 redistance (sdf.cpp) [dirty chunk slot ↑, then dirty body slot ↑]: Chamfer 3-4 two-pass over the dirty window + 1 halo: forward sweep [row ↑, col ↑] then backward [row ↓, col ↓]; weights 3·16 (axial) and 4·16 (diagonal) in 1/16-texel units, i16 saturating; sign from the material channel (mat == 0 → positive); a halo crossing a chunk border extends into the resident neighbour (marks it dirty; the neighbour is re-swept in the same T4 pass if its slot is higher, else next tick)
T5 texel union-find [dirty region slot ↑] (4-connectivity; diagonal-only contact never welds): parent[] over the region's texels in its dirty window (scratch, row-major index), scan [row ↑, col ↑] uniting with left and up solid neighbours of the SAME region; > 1 component ⇒ split: the component containing the region's (cx, cy) keeps the slot, others become new BondRegion slots [component label ↑ by first texel]; bonds re-attached by texel adjacency [bond ↑]
T6 load routing + sever [dirty island ↑ by root slot]: region-graph over bonds; loads = mass_quanta per region; route iteratively toward anchors: ROUTE_ITERS = 8 sweeps [region ↑]: each region pushes its unsupported load to bonded neighbours with lower cy (compression, cost 1) or equal cy (shear, capped at bond.strength) — integer, Σ load exact; bond.load > bond.strength ⇒ BF_SEVERED + EV_SEVERED; CF flagged constraints (λ over break) ⇒ CF_BROKEN + EV_SEVERED; creak: load > 3/4 strength (`(strength >> 2) * 3`) ⇒ EV_CREAK once per bond (flag)
T7 region union-find over bonds [bond ↑, unsevered]: components not reaching an RF_ANCHOR region (bedrock sentinels are regions with RF_ANCHOR owned by CH_EDGE_BEDROCK chunks, plus every BK_STATIC body's regions) ⇒ promote [component ↑ by lowest region slot]:
     bbox ≤ 128² texels: new Body (BK_RIGID) with BodySdf of the smallest fitting class, texels copied [row ↑, col ↑], chunk texels cleared (mat 0, dist +MAX), regions re-owned, mass/inertia from quanta (inv_mass = div<invmass_t>(UNIT, quanta·unit_mass) at creation, i64), momentum inheritance from the impact list weighted by proximity (integer weights = 1/(1 + texel distance), Σ exact), EV_PROMOTED
     bbox > 128²: RF_UNPROMOTABLE on the component's regions, EV_CREAK (recorded v0 limit); quanta < DEBRIS_QUANTA ⇒ granular particles instead, EV_DEBRIS
     crack pattern: extra severs along rng_for(seed, tick, SYS_FRACTURE, region_slot) radial pattern, distance from material data
T8 cavity flood (cavity.cpp) [dirty chunk slot ↑]: occupancy occ[32×32] per chunk in scratch from ChunkTexels (solid if the cell's centre texel dist < 0; liquid if a basin footprint or ≥ LIQUID_CELL_MIN particles by the fine-tier runs); opening cells: empty cells whose left&right or up&down neighbours are solid with a run length ≤ OPENING_MAX_CELLS = 2;
     BFS [cell row-major ↑, 4-conn, never through opening cells, crossing into resident neighbour chunks whose cells are reachable] labels components; old labels (cav[]) of the component's cells: one old label ⇒ same cavity (volume updated); several ⇒ merge into the lowest slot (moles Σ exact, temp mass-weighted, EV_CAVITY_MERGED); an old label spanning several new components ⇒ split: the largest keeps the slot, others new slots with moles ∝ cells (integer division, remainder to the keeper — exact), EV_CAVITY_SPLIT
     openings: each opening-cell run between two distinct labels ⇒ CavityEdge (a < b) with width = run length, dir, centre; rebuilt for the dirty chunks [cell ↑], merged into the sorted array
     cells outside every component but empty and touching the sky row ⇒ CV_OUTDOORS cavity (slot 0, never deleted)
     basins: liquid cells flooded the same way within a cavity [cell ↑] → Basin slots; level_y = highest liquid cell row × 4 texels + fill fraction from quanta; floor_y = lowest
     orifice flow [CavityEdge ↑]: dP = P_a − P_b (i32); moles = clamp(quanta_mul(dP, K_FLOW) × width, −moles_a, moles_b) split per species by source mole fraction (integer division, remainder to the largest species); sat ops both sides; edge.flow stored; diffusion term ± DIFF_RATE·width when dP == 0
     hydrostatic P for basins = P_cavity + depth_quanta × fluid_weight (i32); P ≥ 1 everywhere; area constraints (⑤) get rest_area ← (n·T/P) << shift (fx<i64,36>)
     region exposure flags refreshed for dirty chunks [region ↑]
T9 islands + sleep: rebuild parent[] for dirty islands from persistent constraints (CF alive) and PContacts (both body endpoints) [constraint slot ↑, then PContact ↑]; path compression only inside find() (deterministic); Island rows at roots: member_count, IF_ANCHORED, min_chunk_id; sleep: all members sleep_ticks ≥ SLEEP_TICKS ⇒ IF_ASLEEP; wake: IF_DIRTY or any member touched this tick ⇒ clear; EV_WOKE
T10 plants (plants.cpp) [plant slot ↑]: inputs (water from basin/soil moisture via §6 rules already applied, light = alloy_light_budget at the crown, nutrient); growth_acc += rate; ≥ threshold ⇒ extend/branch/fruit (spawn segment body + ② joint + bond), EV_GREW; wither if water < 0 for W ticks: EV_WITHERED, segment removal
T11 wake queue: pop every WakeEvent with tick ≤ now [array ↑ from wake_head]: re-materialise (mark region/cavity/basin dirty, re-evaluate its analytic process); insert new events for idle-out candidates (regions/cavities/basins with no activity this tick and a computable next-event tick: corrosion T = ⌈hp/rate⌉, overflow T = V/inflow, scheduled detonation, decay) by memmove insert keyed (tick, carrier)
T12 particle compaction + remap [index ↑]: stable compaction removing PF_DEAD: new_index[i] = prefix count of live; memmove rows; remap applied in this order: Constraint a/b with CF_*_PARTICLE, alloy.caux runs, PContact a/b with PC_*_PARTICLE, pending events (CarrierRef kind 0), Agent/Plant none; then spawned particles appended [command order, then chem edit order] (new indices ≥ old); hdr.particle_count = live
T13 sorted-array maintenance: PContact insert/delete, Bond insert (from promote/plant), CavityEdge, WakeEvent — memmove, [key ↑]; dead SlotMap rows zeroed; dirty flags cleared; chunk_slot_of updated for newly resident/unloaded chunks (gen/recipe.cpp writes newly generated chunks here, at the end of T13, from the scratch buffers §12 produced earlier in the tick)
```

### §14.5 Determinism checklist and the symbol gate

Per pass, the implementer checks every box before the pass is merged:

- **Order**: every loop in §14.4 runs in its bracketed order; no loop iterates a `Map`; `SlotMap`
  walks are `0..slot_cap` skipping dead; sorted arrays are walked `[key ↑]`; neighbour lists are
  insertion-sorted by id before any accumulation; pair lists are `sort_u64_kv`'d and uniqued.
- **Chunk-keyed outputs**: every `parallel_for` writes to `[chunk]`-indexed scratch or to a
  per-chunk list; folds run `0..chunk_count(n, grain)`; grains are the §14.4 constants; nothing
  reads a worker id; per-worker scratch is used only for allocation.
- **Arithmetic**: every product/quotient is a named helper with a stated result row; every
  widened intermediate is `i64` with its frac stated; the single round per substep is S4 and the
  single round of every accumulator is its "Round once" line; quanta paths use `sat_*` only;
  plain `+`/`-` on signed values carries a range comment.
- **RNG**: every draw is `rng_for(seed, tick, system_id, carrier_bits, draw)` with `system_id`
  from `rng_systems.h` — `SYS_BASIN 1, SYS_GROWTH 2, SYS_CHEM 3, SYS_EMBER 4, SYS_FRACTURE 5,
  SYS_DECAY 6, SYS_WEATHER 7, SYS_CONDENSE 8, SYS_DEBRIS 9, SYS_PROMOTE 10` — and `draw` is a
  local counter; no draw result is stored across ticks.
- **Hash-region integrity**: the pass mutates registered arenas only where §14.2 says the row is
  authoritative; scratch never aliases a registered arena; pool growth happens only in pass 5
  between `guard_window_open/close`; `arena.used` never covers capacity.
- **State hygiene**: no `static` mutable, no `thread_local`, no pointer stored in any row, no
  pointer comparison other than equality, no reads of `0xDD`-poisoned scratch (ASan job).
- **Wake completeness**: any code path that touches a sleeping carrier/island (contact, force,
  edit, chem, wake event) clears the sleep flag in the same tick before the solve's level lists
  are built (`slept == stepped`).
- **Events**: emitted into the current chunk's scratch ring; never read back by sim code; never
  hashed.

**Symbol-gate allowlist for `tl_sim`** (`llvm-nm --undefined-only libtl_sim.a`): `memcpy`,
`memset`, `memcmp`, and symbols defined by `tl_foundation_det` (`tl_*`: fx/det math, `rng_for`,
`rapidhash`, arena/registry/scratch, `sort_u32_kv`/`sort_u64_kv`, SlotMap/Array/RingBuffer/
Bitset helpers, `TL_FATAL`'s `tl_fatal_impl`, `jobs` API). Everything else fails CI. The
same-job grep over `src/sim/` bans the tokens `float`, `double`, `std::`, `thread_local`,
`asm`, `__builtin_ia32_`, `rdtsc`, `static` mutable (per `CPP-SUBSET.md` §4), and `#include <`
other than `<stdint.h> <stddef.h> <string.h> <limits.h>`.

### §14.6 Tests — `tests/sim/`

| File | What it proves |
|---|---|
| `t_pools.cpp` | every row: `sizeof`, every `offsetof`, zeroed padding after construct/remove; pool arena layout rule; `CarrierRef` ordering |
| `t_hash_region.cpp` | per pool (all 19 arenas): mutate a transient buffer → no hash moves; mutate each authoritative row → that arena's hash and only it moves |
| `t_radix.cpp` | `sort_u32_kv`/`sort_u64_kv` property vs a naive stable reference on random keys incl. duplicates, 0/1/n, all-equal; stability |
| `t_broadphase.cpp` | `fine_key`/`coarse_key` at the world edges and negative coords; neighbour lists vs O(n²) reference; pair list uniqueness; ID-sorted within cells |
| `t_fields_heat.cpp` | **first slice**: Σ `q` over edges == 0 every tick; energy ledger `Σ cap·T + heat_res` constant to the quantum on a closed bond graph; run-twice; worker sweep 1/2/8/16; shuffle mode |
| `t_fields_circuit.cpp` | Kirchhoff: Σ node currents == Σ sources; fixed-iteration convergence vs reference; order independence (run-twice) |
| `t_forces.cpp` | falloff profiles at q ∈ {0, ½, 1}; occlusion gating vs reference raycast; buoyancy: neutral density → zero net Δv; Round-once: accumulator vs i64 oracle |
| `t_solver_kernels.cpp` | each kernel ①②③⑤⑥⑦ + contact + density: C → 0 on a two-carrier rig within N substeps; λ unilateral never < 0; mass-ratio clamp at 10⁶:1 behaves as 4096:1; FLOAT-SHADOW drift bound per kernel (dev only) |
| `t_solver_conservation.cpp` | Σ momentum (i64 Σ mass·v) of a closed free-body scene conserved to within the per-substep rounding bound; stacking: 20-box pyramid rests (G-01 jitter at the `pos_t` quantum recorded) |
| `t_tunneling.cpp` | a particle and a body at `V_MAX_WORLD` toward a 1-texel wall never cross it with the pair list reused across 8 substeps (flip condition owner) |
| `t_coloring.cpp` | greedy colouring: no two same-colour constraints share a carrier; persistent cache invalidation; > 64 colours → fatal hook |
| `t_chemistry.cpp` | pair ownership: each pair fires once (count oracle); Σ quanta across every rule == 0; stochastic rules reproducible by key; threshold hysteresis: an ice⇄water rig at the threshold never strobes over 1000 ticks |
| `t_transitions.cpp` | round-trips with Σ-mass exact per species: disturb→settle→disturb, melt→freeze, boil→condense, dissolve→precipitate; re-bake conflict rule (solid never overwritten) |
| `t_sdf.cpp` | carve returns removed quanta per material equal to the texel delta; Chamfer redistance vs brute-force distance within 1/16 texel over random windows; chunk-border halo continuity |
| `t_union_find.cpp` | texel UF vs a reference BFS on random masks (4-conn); region split determinism; region UF reaches anchors iff the reference does |
| `t_fracture.cpp` | sever → promote: mass and momentum Σ exact between welded and promoted states; debris threshold; >128² refusal flag; crack pattern reproducible by key |
| `t_cavity.cpp` | flood property vs reference BFS on random occupancy; merge/split mole Σ exact; opening width rule at 1/2/3 cells; orifice flow conserves moles; `P ≥ 1`; `cavity_at` O(1) matches the flood |
| `t_basin.cpp` | level from quanta; stratification order; hydrostatic P; settle K-tick threshold; basin↔cavity partition invariants |
| `t_islands_sleep.cpp` | `slept == stepped`: a scene run with sleeping vs a full-scan reference → identical hash trace |
| `t_wake_queue.cpp` | sorted insert/pop property; every analytic wake fires ≥ 1 tick before its event; thrash hysteresis |
| `t_compaction.cpp` | compaction preserves order; every referrer remapped (constraints, caux, PContact, events); spawned indices ≥ old |
| `t_edit_cmds.cpp` | every `AlloyCmdKind`: happy, malformed (`ALLOY_ERR_CMD_*`), out-of-order buffer → check; spawn echo event; detonate_tick scheduling; commit window buffering |
| `t_queries.cpp` | raycast/shapecast vs reference marching; connectivity; `medium_at` across all four kinds; `cavity_path` hop/min-opening; light budget occlusion |
| `t_agent.cpp` | medium transitions from immersion/contact; commit window: identical state under any intent stream inside the window |
| `t_plants.cpp` | growth thresholds; chop (sever joint) → segment falls; wither reversal; light budget feed |
| `t_gen.cpp` | chunk-hash replay for a test seed at tick 0 and 10⁶; PC↔Pi trace compare (infra-gated) |
| `t_snapshot.cpp` | save/restore round-trip → identical next-tick hash; `alloy_post_restore` rebuilds `chunk_slot_of`; fingerprint mismatch → `ALLOY_ERR_SNAPSHOT_FINGERPRINT` |
| `t_harness.cpp` | run-twice (two `AlloyWorld`s in one process), record→replay over the command log, worker sweep 1/2/8/16 + shuffle, UBSan/ASan variants — over the toybox scene |
| `t_budget.cpp` | G-05 scene (20k particles, 2k bodies, 500 dirty regions): per-pass ms and Σ `used` per arena written to CSV (T-A-03) |

**Gate 0 hand-off.** `tl_gate0` (the disposable solver, `GATE0-BENCH.md`) may share exactly:
`sim/solver_kernels.h`, `sim/rng_systems.h`, `sim/alloy_consts.h`, and the `Contact` struct
definition (copied into the bench, not included). It may not include `pools.h`, `alloy.h`, or any
pass `.cpp`. Its measurements (G-01..G-04) are recorded against `solver_kernels.h` so a kernel
change after Gate 0 re-runs the bench, not the whole queue.

### §14.7 Build order (expands the build queue; "done" is the merge criterion)

| Step | Files created | Tests that must pass | Measurement recorded |
|---|---|---|---|
| 1. Harness | `tests/sim/t_harness.cpp`, `t_budget.cpp` skeletons over an empty `AlloyWorld`; `sim/alloy.h`, `views.h`, `pools.h` (rows + asserts only), `alloy_consts.h`, `rng_systems.h` | `t_pools`, `t_hash_region` (19 empty arenas), `t_harness` on a no-op step | Σ `used` at init per arena |
| 2. Gate 0 | `sim/solver_kernels.h` (①, contact, density), `tl_gate0` bench | `t_solver_kernels`, G-01..G-04, G-06 | the Gate 0 CSVs; palette rows stamped DECIDED or ladder rung recorded |
| 3. Substrate | `broadphase.cpp`, `tables.cpp`, `edit.cpp` (intake + spawn/despawn), `alloy.cpp` (skeleton with guard hooks), `fields.cpp` (heat only) | `t_radix`, `t_broadphase`, `t_fields_heat` (the first slice, incl. worker sweep), `t_edit_cmds` (spawn/despawn), `t_compaction` | broadphase ms at 20k; heat pass ms at 500 regions |
| 4. Topology core | `sdf.cpp`, `topology.cpp` (T1, T4–T7, T9, T12, T13), `cavity.cpp` (T8 flood/openings, no flow) | `t_sdf`, `t_union_find`, `t_fracture`, `t_cavity` (flood/merge/split), `t_islands_sleep`, fuzz on synthetic carve sequences (10⁴ random carves, Σ-mass exact, run-twice) | pass 5 ms amortised over the fuzz |
| 5a. Solids + solver | `solver.cpp` (contacts C1–C4, colouring, substep loop, velocity pass, PContact) | `t_coloring`, `t_solver_conservation`, `t_tunneling`, `t_pools` (PContact) | pass 3 ms at 2k bodies; G-01 jitter on the stack scene |
| 5b. Liquids + gases | density kernel wired, XSPH, `cavity.cpp` (basins, orifice flow, hydrostatics, T3 settle/disturb), `forces.cpp` (buoyancy/drag) | `t_basin`, `t_cavity` (flow), `t_transitions` (disturb→settle), `t_forces` (buoyancy) | pass 3 ms at 20k particles; pass 5 settle cost |
| 5c. Fields | `fields.cpp` complete (circuits, wetness, decay, sky), `forces.cpp` complete (F1–F4, magnetism, wind) | `t_fields_circuit`, `t_forces` | pass 1 + pass 2 ms |
| 5d. Chemistry / fire / transitions | `chemistry.cpp`, T2 transitions, burning, deflagration, ember species | `t_chemistry`, `t_transitions` (all four round-trips) | pass 4 ms at 20k |
| 5e. Vegetation | `plants.cpp`, T10, light budget query | `t_plants`, `t_queries` (light) | growth cost per plant |
| 5f. AgentBody | `agent.cpp`, `MoveIntent` intake, commit window | `t_agent`, `t_edit_cmds` (intent) | — |
| 5g. Queries + wake | `query.cpp`, T11 wake queue, `hash.cpp` views | `t_queries`, `t_wake_queue`, `t_snapshot` | — |
| 6. T-A-01 / T-A-03 | snapshot-ring prototype in `tests/sim/t_rollback.cpp` | restore round-trip hash identity at depth 6; closure-restore vs whole-arena cost | restore ms vs island count; Σ `used` per arena on the G-05 scene → reserve table commit |
| 7. Engine wiring | `gen/*.cpp`, Luau bindings (outside `tl_sim`), render over `views.h`, desync harness integration, the toybox scene | `t_gen`, `t_harness` over the toybox incl. cross-ISA, the two-consumer scene scripts | G-05 final: ≤ 4 ms PC / ≤ 12 ms Pi at nominal load |

Each step's "done" = its tests green under debug + UBSan/ASan, the worker sweep identical, the
measurement committed to `GATE0-BENCH.md`/`docs/measurements/`, and no new undefined symbol.

---

## Gates & rulings ledger (nothing open — closed 2026-08-22)

### Netcode-driven (carried; `NETCODE.md` §3/§7)

**T-A-01 — closure-scoped arena restore prototype. THE GATE for speculation — GATED, response pre-committed.**
A full-world resim at depth 6 is ~48 ms vs a 16.7 ms frame, so restore must be scoped to the
island **closure** (islands that merged/split with the target over the window). Alloy's pools
are global SoA — restoring one closure is a scatter, not a memcpy. Three candidates:

| Approach | Determinism | Performance | LOC / cognitive | Correctness surface |
|---|---|---|---|---|
| Whole-arena restore, accept the cost | Trivially safe | ~48 ms at depth 6 — unaffordable | Lowest | Smallest |
| Per-island arena partitioning | Safe, but merge/split moves memory between arenas — pass 5 does exactly that every tick | Restore is a memcpy again | High — allocation becomes topology-dependent | Wide: every re-island must relocate correctly |
| **Handle-indexed scatter restore** (most likely) | Safe | Scatter ∝ closure size, not world size; likely fine at ~50 awake islands | Medium | Medium — needs an island→member-set index pass 5 maintains (it already tracks membership and sorts by id) |

Prototype headless on the snapshot ring; measure restore cost vs island count **and closure
size vs awake-set size and merge rate** under dense load. Pass 1 (fields) resims globally at
full depth regardless (~0.5 ms × depth) and is counted in the budget. If closure restore is
unaffordable, the netcode collapses to **delay-only lockstep with no rollback** — smaller,
simpler, shippable, and what §9.1 already assumes. Fine outcome; discovering it late is not.

**T-A-02 — `v_max` in the validator. DECIDED; coverage ACCEPTED.** §11.1. The debug assert on
per-tick displacement is the net for a code path that outruns its data; in release an overrun
degrades to a late frame handled by the quorum fold, never a divergence (`NETCODE.md` §17 R2).

**T-A-03 — arena-set size. DECIDED method, number pending the first stress scene.** Budget
reserves are the `MEMORY.md` §6 table until then; the measurement (20k particles / 2k awake
bodies / 500 dirty regions, `registry_hash_all` reporting Σ used) replaces the table in one
commit and sizes the snapshot ring. Until measured, the ring is sized at the reserve table.

**T-A-04 — `commit_ticks` on `MoveIntent`. DECIDED** (§8.2); defaults 0 / 6 ticks per action
class, tuned as `AgentSpecies` data (§8.3) with the rollback-frequency coupling recorded.

**T-A-05 — per-arena hashes at per-arena granularity on the view surface. DECIDED** (§9.2).
Confirm-and-shape when the view surface lands; never collapse to one world hash.

**T-A-06 — island-merge telegraph / `AOE_ISLAND_LIMIT`. DECIDED** (§6.2b: detonation tick fixed
at trigger time, carried in the command). **Engine defaults: `AOE_ISLAND_LIMIT = 4`, minimum
telegraph = `CONFIRMATION_HORIZON_TICKS` = 6 ticks**; the validator rejects any area-effect row
whose telegraph is shorter. A game may raise both, never lower them; the rule is therefore
enforced at `init()`, not left to combat design to remember.

*(T-A-07, the sort bench, is void — §1.2 decided.)*

### Decided defaults that a measurement verifies (the flip condition is pre-committed)

| Item | Decided | Verified by | Pre-committed response if it fails |
|---|---|---|---|
| Palette rows (esp. `pos_t` fx<i32,18>) | `FX-PALETTE.md` §3 values | Gate 0 G-01..G-04 | climb the ladder rung by rung; rung 4 (wide state, narrow math); then the float fallback |
| Substeps | 8 | Gate 0 sweep 4/8/16 | move the constant (recorded) |
| Particle budget | 20k active; Pi = reference peer | G-05 | PC miss = pivot-level; Pi-only miss = Pi becomes stretch peer |
| Cross-ISA / run-twice / worker traces | identical | G-06, then the harness forever | it is UB: sanitizers, never the palette |
| Broadphase pair list reused across substeps | yes, margin `V_MAX_WORLD × h × 8` | tunneling test at `V_MAX_WORLD` | rebuild the fine tier every 4 substeps (the margin halves) |
| Solver kernel | colored Gauss-Seidel | Gate 0 convergence + lane utilization | Jacobi-within-color hybrid behind the same skeleton |
| Hot-row field order | §1.4 order | cache bench | reorder fields; width and membership stay |
| Grass/brush as pinned particles | yes, when a plant consumer exists | count bench at that time | promote dense grass to a per-chunk SDF decal (render-only) |

Everything else formerly in this table is ruled in its section (§1.4, §2.4, §3.5, §4.4, §8.3,
§5).

### Parked (recorded so they are not re-proposed)

Analytic CSG fast path for machine parts (§2.1) · solvent+solute hybrid and graduated-blend
(§3.3, §6.1) · per-texel temperature for special bodies (§5.1) · non-authoritative visual wind
field (§4.3) · sound/shockwave through cavities (§4.4) · latent heat (§6.2) · ragdoll handoff
(§8.3) · per-particle composition vectors, FleX clusters, unified SoA, SAP+grid, monolithic
terrain, Voronoi shatter, FEM-lite, Eulerian gas grid, flame particles, L-system layer,
kinematic KCCs, lockstep-only state — all rejected above, with reasons.

### Build queue (headless-first; after `FOUND` + `ECS` exist)

1. Harness + conservation oracles + perf harness (§10) — before any sim code.
2. Gate 0 bench over the palette (no engine).
3. Substrate: pools, radix sort, tiered hash, job wiring (§1). First slice: bond-graph heat
   flux with exact Σ-energy.
4. Pass-5 topology core standalone (the concentrated risk): union-find, coarse sampling,
   cavity/basin identify-split-merge; fuzz on synthetic carve sequences.
5. Solids → Solver → Liquids + Gases → Fields → Chemistry/fire/transitions → Vegetation →
   AgentBody (each with its round-trip/invariant tests first).
6. T-A-01 prototype on the snapshot ring; T-A-03 measurement.
7. Engine wiring: views → render, Luau bindings, desync-harness integration; the toybox scene
   → the two-consumer gate.

---

*Rev 1 — 2026-08-22. Written from `PIVOT-DESIGN.md` rev 1 and the foundry Alloy design.
Supersedes `../foundry/ALLOY-DESIGN.md`. Next rev after Gate 0: §10 and §8.1 get measured
values and the palette rows get their DECIDED stamp — or the pre-committed fallback is
recorded here.*
