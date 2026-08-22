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
Per-tick bookkeeping (per-arena hash, event drain) runs after pass 5.

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
  tagged reference that constraints, reactions, events and queries use for "either kind".
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
  one carrier enters the other, to the quantum) — that makes Σ-energy *exact*.
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

**Substep loop** (8 per tick, `H = 1/480 s` and `H2` as rounded fx constants): save
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
Result<World*> alloy_init(ArenaSet* arenas, const CompiledTables* tables, const WorldDesc* wd);
void           alloy_step(World*, const EditBuffer* cmds, u32 tick);   // the only mutator
```

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
