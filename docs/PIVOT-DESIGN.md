# The C++ Pivot — architecture ruling (rev 1, 2026-08-21)

> **Status: best so far, not final.** This doc records the decision to retire the Ore-language
> dependency and rebuild the runtime on a lean-C-style C++ engine + Luau game layer, with
> determinism secured by **fixed-point arithmetic (determinism-by-construction)** instead of
> compiler-enforced strict float. Settled sections are marked **DECIDED**; everything genuinely
> open lives in §12 and is a question, not a default.
>
> **What survives unchanged** (this doc does not restate them): `ALLOY-DESIGN.md` §1–§8
> (all mechanisms), `NETCODE-DESIGN.md` rev 3 (all consensus/succession/checkpoint design;
> only the transport + stdlib-mapping rows change), `DETERMINISM-DESIGN.md`'s ordering rules
> and §4 test harness, the module *promotion ladder discipline* of `FOUNDRY-MODULES.md`
> (as folder/organizational policy, not DLLs), and the doc/anti-rot/two-PC workflow rules.

---

## 0. The ruling and the trade (DECIDED)

**Why.** Building Ore costs more time than the program can afford. The language was never the
product; it existed to make determinism *compiler-enforced* (strict FP default, `@deterministic`,
the cross-ISA float proof).

**The substitution that makes the pivot coherent:** fixed-point sim arithmetic replaces
enforcement-by-compiler with **determinism-by-construction** — integer ops are bit-exact on every
ISA, compiler, and optimization level. Without this substitution, C++ would be a regression
(rebuilding Ore's float guarantees by hand, unenforced); with it, most of what Ore enforced stops
being a hazard at all.

**Forfeited, stated honestly** *(and largely recovered — see the 2026-08-21 recovery ledger:
`@deterministic` → the §2 symbol-audit CI gate (~90%); `@preserve_layout`/`@layout_hash` →
§8's `WIRE_STRUCT` static-assert discipline (~100%, stronger); worker-invariance guarantee →
§12a's chunk-keyed rule (~95%, stealing-safe); the 43 M-tick soak → Hovel Milestone E as its
named successor (§10); the solver's Gate 0 risk → instrumented by the float-shadow diagnostic
build (§10), the one item not recoverable by construction. The doc stack, keyed RNG, ordering
rules and netcode design were never Ore's and carried whole.)*:
- Ore's 43 M-tick cross-ISA strict-float silicon proof (the one *proven* asset). Cross-ISA is
  retained as a requirement, now delivered by fixed point instead.
- Compiler-enforced `@deterministic` / `@noalloc` / `@iterable`. Enforcement moves to
  discipline + the record→replay hash harness + CI sanitizers. The harness was always the real
  safety net; it is now the *only* one, so it lands first (test-infra-first is unchanged).
- The physical DLL seam (`FOUNDRY-MODULES.md` §1's "violations don't link"). Seams revert to
  convention on the C++ side; the *physical* seam moves to the C++/Luau boundary (scripts cannot
  reach engine internals).

**Gained:** immediate start with mature tooling (debuggers, profilers, sanitizers), the library
ecosystem, and Luau as the data/meaning/iteration layer — which deletes the entire game-DLL
hot-reload + `migrate(T)` + `@layout_hash` problem class (gate Task A's hardest half is void).

**Quiet wins worth recording:** NETCODE R12 (CSPRNG) un-blocks — OS entropy is an afternoon in
C++ (`BCryptGenRandom` / `getrandom(2)`) behind the platform seam. NETCODE R14 (`@fast_math`),
INV-8 float canonicalization, the denormal policy, and T-O-03/04 (jobs FP, mixed arch flags) are
all void: no floats in authoritative state.

**Cross-ISA requirement: KEPT** (PC x86-64 + Steam Deck x86-64 + Pi 4 aarch64 remain the
3-peer reference set), delivered by fixed point. **The XPBD/PBF fixed-point bench (Gate 0, §10)
is what this now hangs on** — its fallback is pinned-toolchain float, x86-64 only, decided then,
not drifted into.

**Prior-rule amendments this ruling makes:**
- `CLAUDE.md` principle 1 ("never C++") is rewritten as the §2 subset — the *reasons* (hidden
  control flow, hidden alloc, compile time) survive as rules; the absolute does not.
- `ALLOY-DESIGN.md` §10's "why float in the solver" is formally **re-litigated** (lock = best so
  far). The dynamic-range argument is real; the counter is mass-ratio clamping + per-domain
  Q-formats + widened intermediates (commercial precedent: Photon Quantum ships deterministic
  fixed-point physics). Gate 0 decides it by measurement, not argument.

---

## 1. The stack (DECIDED)

| Layer | Choice | Notes |
|---|---|---|
| Engine | **C++ (lean subset, §2), one static exe** | No DLLs, no engine hot reload. Modules = folders/static libs + registration order (§9) |
| Game layer | **Luau** | Data + meaning + iteration (script reload replaces hot reload). Sim-facing rules in §7 |
| Platform | **SDL3** (window/input/audio) + **SDL_ttf** | |
| Render v0 | **SDL_Render** (streaming texture + batched quads) | Fork 1's rationale transfers verbatim: the sim view is a CPU pixel buffer. SDL_GPU reserved as the second render path behind the same seam (buys shaders/compute later; costs the SPIR-V/shadercross pipeline — not paid at v0) |
| Editor/dev UI | **Dear ImGui** (SDLRenderer3 backend at v0) + ImGuiColorTextEdit | Generic inspector driven by §6 reflection. Luau step-debugger is post-v0 scope (record→replay time-travel covers most of it earlier) |
| Netcode transport | **ENet** | Channel mapping in §8. Everything above transport in NETCODE rev 3 stands |
| Image decode | **stb_image** | unchanged |
| Text formatting | **stb_sprintf** | locale-free (CRT printf is locale-dependent — a replay/log-diff footgun) |
| Crypto | **Monocypher** (vendored, single-file pure C) | BLAKE2b for the hash chain, Ed25519 for custody signing. **Never roll own crypto.** OS entropy behind the platform seam feeds keygen |
| ECS | **Own minimal** (§6) — flecs and EnTT rejected | |
| Containers/arenas/math/RNG/hash | **Own** (§3–§5) | |

Vendoring rule unchanged: pure-C (or single-file) deps, proportionate to value, compiled once in
their own TUs; they are exempt from the §2 subset internally.

---

## 2. C++ discipline (DECIDED)

The subset — C-with-namespaces ("orthodox C++"):

- **Banned in engine/sim code:** STL containers/algorithms/streams, RTTI, exceptions
  (`-fno-exceptions -fno-rtti`), inheritance/virtual/polymorphism, destructors/RAII, operator
  overloading beyond the fx palette's arithmetic, `new`/`delete`/`malloc` outside arena backing.
- **Sanctioned templates (closed list):** flat value templates with enumerated instantiation
  sets only — `fx<Rep,FRAC>`, `Array<T>`, `Span<T>`, `Map<K,V>`, `SlotMap<T>`,
  `Handle<Tag,IDX,GEN>`. No recursive/meta templates, no SFINAE/concepts, no expression
  templates. `static_assert` + `constexpr` functions are encouraged (they are the enforcement
  tools).
- **Allowed system includes in engine/sim TUs:** `<stdint.h>`, `<stddef.h>`, `<string.h>`
  (memcpy/memset). `<math.h>` is **banned in sim code** (libm is the cross-platform determinism
  hole; §3's det math is the replacement). Third-party TUs compile with whatever they need and
  must not leak includes into ours.
- **The `@deterministic` replacement — a symbol-audit CI gate** *(ratified 2026-08-21)*: all
  sim code compiles into its own static lib(s); CI runs `llvm-nm` over each and **fails on any
  undefined symbol outside an allowlist** — no allocator symbols (`malloc`/`free`/`operator
  new`), no libm (`sin`/`cos`/`pow`/`sqrt`…), no clock/entropy (`time`/`clock*`/`rand`/OS
  entropy), no io/socket symbols. That is a callgraph effect ban enforced at link granularity
  — ~90% of what the attribute gave, at an afternoon's script cost. The residue (things that
  never become symbols: `rdtsc`-class intrinsics, inline asm) is covered by a grep line in the
  same CI job. The sim-lib boundary this requires is the §9 module layout doing double duty.
- **No static mutable state in engine/sim TUs** (grep-enforceable). Rationale is correctness,
  not style: the dual-sim determinism test runs two worlds in one process, and rollback restores
  only registered arenas — state outside them is a desync generator the hash cannot see (§6).
- **Scope:** the runtime binary. Offline tools/generators may use anything (mirrors the old
  "C++ only in offline CLI tools" shape).
- **Error model** *(ruled 2026-08-21, replaces D2's Ore shapes)*: programmer bug →
  assert macro tiers (fatal in debug; release keeps a slim fatal tier for invariant checks,
  compiles out the rest); recoverable failure → **`Result<T> { T value; ErrCode err; }`
  returned by value** — the one sanctioned shape, error-code-out-param banned; absence →
  null-sentinel handles (`bits==0`) and documented-nullable pointers. No exceptions anywhere
  (`-fno-exceptions` enforces). Fail-loud stays policy: validators reject at init, loads fail
  with a named error, no silent partial states (ALLOY §11 discipline unchanged).
- **The new UB discipline** (replaces float canonicalization as the thing that can silently
  break bit-exactness): sanctioned wrapping/saturating helpers are the only arithmetic in quanta
  paths (already an Alloy rule); UBSan+ASan runs in the determinism CI; hashed state uses
  explicitly-padded structs and zero-filled arenas; hash used extents `[base, used)`, never
  capacity (per-pool ruling written in each pool's header — ALLOY's hash-region-integrity
  invariant meets C++ padding).
- **Compile time is a feature, not hygiene** — it is Alloy's iteration story now (no hot reload
  for the C++ core). Unity builds, deps prebuilt, and **full-rebuild wall time is a CI number
  with a budget** (target: full < 10 s, incremental < 2 s; a regression is treated like a perf
  regression).

---

## 3. Determinism model: fixed point + det math + keyed RNG + pinned hash (DECIDED, palette rows OPEN)

**No floats in authoritative state or on any sim path.** Floats remain legal render-side
(interpolation, camera — D10's boundary is unchanged).

### 3.1 The fx library — per-domain formats, closed palette

- Mechanism: `template <typename Rep, int FRAC> struct fx { Rep v; };` — one ~300-line header,
  zero includes. The shift is a compile-time constant folded into every op.
- **Policy: a closed palette of named domain types** (~6–8 rows), one header
  (`fx_palette.h`); ad-hoc per-field formats are forbidden (N² mixed-op decisions). Adding a
  palette row is a design decision.
- `+`/`-` compile only between identical formats; **no implicit conversions ever**
  (`explicit` everything; conversions are `to<pos_t>(x)` — greppable).
- `*`/`/` never via operator overloads — named helpers taking the result format as a template
  parameter, widened (128-bit / `mulhi`) intermediate inside, the narrowing point visible at
  every call site.
- **Width is a perf decision**: no usable 64-bit SIMD multiply exists on SSE2/AVX2/NEON, so
  every column the solver should vectorize is a 32-bit format; positions likely need 64.
  (The sizing discipline of `FOUNDRY-DECISIONS.md`, transplanted to Q-formats.)
- Runtime-scale (shift stored in data) is **rejected**: it deletes the compile-time
  mixed-scale check, which is the main reason to do this in a typed language.
- Each palette row carries a documented range/resolution line, and the `init()` table validator
  checks game data against format ranges (same slot as `v_max`, T-A-02).
- The §10-of-ALLOY stress cases map to specific rows: the feather→boulder denominator is
  `invmass_t`'s range clamp (mass-ratio clamp is a validator rule); the sub-texel-correction vs
  world-extent spread is the `pos_t` vs correction-format split. **Gate 0 runs against the
  palette**, so a failure names the row that is wrong.
### 3.1a World constants (CONFIRMED — veto pass 2026-08-21; changing one re-derives rows mechanically)

| Constant | Ruled | Reasoning |
|---|---|---|
| World unit | **1 unit = 1 m** | Human-scale side-view; agent capsule ~1.8 units |
| `TEXEL` | **1/16 m (6.25 cm)** | Noita-class carve legibility at gameplay zoom; chunk (128² texels, ALLOY §2.2) = 8 m — a sane streaming unit. Assay sanity-checks it visually |
| World extent | **±4,096 m (8 km span)** *(tightened from the ±16 km draft at the veto pass)* | Covers a Terraria-large-class world at ~1.6×; the trade is deliberate — the freed bits go to position precision, quadrupling Gate 0's stacking headroom. Raising it later is a palette-rev doc edit before saves exist, a migration after; beyond it is ALLOY §13 streaming, not coordinates |
| `V_MAX_WORLD` | **512 m/s** | Validator-enforced cap (T-A-02); faster effects are raycasts, not integrated bodies |
| Substeps/tick | **8** (60 Hz → h = 1/480 s) | XPBD default; h, h² are precomputed rounded fx constants (1/480 isn't dyadic; one shared constant is deterministic by construction) |
| `MASS_RATIO_CLAMP` | **4096 : 1 (2¹²), applied as an effective clamp** | The ALLOY §10 feather→boulder counter-measure: per-solve, each pair's inv-mass spread saturates at the clamp rather than the validator rejecting table data — content is never refused, extreme pairs just behave as if 4096:1 (a documented approximation, invisible in practice: past ~1000:1 the light body reads as massless anyway). Statics are inv_mass = 0 exactly and cost no range |

### 3.1b The palette rows (PROPOSED, pending Gate 0)

Derivation rule: integer bits ≥ ⌈log₂(range × margin)⌉, rest is FRAC; 32-bit wherever the
solver should vectorize.

| Type | Format | Range | Resolution | Notes |
|---|---|---|---|---|
| `pos_t` | **fx<i32,18>** | ±8,192 m | 3.8 µm (1/16384 texel) | 2× margin over the ±4,096 m extent. **The risk row** — the XPBD correction quantum is 3.8 µm; resting-contact jitter at this floor is what G-01 measures. The veto-pass extent tightening bought 4× the quantization headroom the ±16 km draft had. Fallback if G-01 still fails: `fx<i64,32>` world pos + 32-bit chunk-local solve deltas |
| `vel_t` | **fx<i32,20>** | ±2,048 m/s | ~1 µm/s | 4× margin over `V_MAX_WORLD` (solver transients overshoot) |
| `invmass_t` | **fx<i32,18>** | ±8,192 | 3.8e-6 | Unit mass = reference particle; inv_mass ∈ [0,4096] under the clamp, 2× headroom — the `w₁+w₂+α̃` denominator cannot overflow by construction |
| `stiff_t` (α̃=α/h²) | **fx<i32,30>** | ±2 | 9.3e-10 | Near-zero for stiff constraints; precision matters, range doesn't. Tables store α; α̃ precomputed at init |
| `q_t` (normalized dist) | **fx<i32,30>** | ±2 | 9.3e-10 | **Kernel strategy**: PBF/SDF kernels evaluate on q = r/h_kernel ∈ [0,1] — normalize once per pair, polynomial in `q_t`, scale back once. Kernel precision becomes world-scale-independent |
| `angle_t` / `omega_t` | **fx<i32,30>** / **fx<i32,20>** | ±2 turns / ±2,048 turn/s | ~1e-9 turn | **Turns, not radians** — wraps free at ±1; sin/cos tables index naturally |
| Conserved quanta | **plain i32/i64** | — | — | Unchanged from ALLOY; saturating ops only |

- **Solver internals — the precision ladder** *(ratified 2026-08-21; ordered by
  precision-per-cost, and the mandated response order to a Gate 0 convergence failure —
  climb the ladder before touching the fallback)*:
  1. **Widened accumulate + round-once-per-substep — the LEAD, day one** (upgraded from
     "a measurement"): solver-local positions/velocities stay i64 across the substep's
     constraint sweep, rounded to storage format once per substep. Most of 64-bit's
     precision benefit at zero storage/bandwidth cost. λ accumulators and multi-term sums
     likewise widen and round once at writeback, never per term.
  2. **Round-to-nearest-even in the `mul<R>` helpers, day one** — truncation biases every
     op downward (systematic energy drain + convergence stall); RNE is unbiased for ~1
     extra add per op.
  3. **Residual carry** (per-quantity rounding residual fed back next substep — error
     diffusion, fully deterministic): a standing Gate 0 bench variant, adopted only if
     rungs 1–2 leave a measurable stall.
  4. **Wide state, narrow math — the named fallback** (formalizes the `pos_t` row's
     fallback): fx<i64,32> stored positions, constraint math on 32-bit deltas against a
     local island/chunk origin — width where error accumulates (state), narrowness where
     throughput lives (math); SIMD and cache traffic survive.
  5. **Pipelined sim thread — throughput escape hatch only, never a precision fix**:
     decouple sim tick from render frame (simulate N+1 while rendering N) to buy up to the
     full 16.7 ms. Costs +1 frame of input latency and complicates the netcode FIRST/LAST
     barriers and rollback timing; consider only on a G-05 miss, and only after T-A-01
     reports. (A dedicated sim thread does not create budget — the job system already fans
     the sim across all cores during its phase; only pipelining buys wall time.)
  **Uniform 64-bit everywhere is REJECTED**: it doubles cache-line traffic on the hot
  columns (the real cost, beyond the SIMD loss) to buy precision uniformly, while rounding
  error in a Gauss-Seidel solve is localized to accumulation points — rung 4 dominates it
  on every axis.
- **Mixed-op table**: the sanctioned products and result formats (`vel×h→pos-delta`,
  `invmass×impulse→vel-delta`, q-polys, …) are enumerated in `fx_palette.h` next to the
  types, all through the widened `mul<R>(a,b)` helpers — no implicit combination exists.
- After Gate 0 this section gets rev 2 with measured values and the DECIDED stamp;
  `fx_palette.h` is written from rev 2, not rev 1.

### 3.2 Det math (sourcing RULED 2026-08-21: own core, ported kernels)

Survey verdict (libfixmath abandoned-2012/Q16.16-only — rejected; fpm = drop-in-float
philosophy, radians, no palette policy — oracle only; CNL/SG14 — template-heavy, §2-banned):

- **Core arithmetic + palette + policy is OURS, necessarily** (~300 lines, §3.1) — the
  closed palette, explicit-result `mul<R>()`, saturating tier, and turns angles are *policy*,
  and no library ships policy; adopting one means wrapping its entire surface anyway.
- **Transcendental kernels (sqrt, sin/cos, atan2, exp/log if needed) are PORTED from
  FixPointCS** (github.com/XMunkki/FixPointCS — MIT, attributed in the header): built
  explicitly for deterministic lockstep sims, bit-identical-across-compilers by design,
  precision-documented polynomial approximations. Porting transfers the
  silent-bad-polynomial risk to code that has shipped in deterministic games for years.
- **One deliberate deviation from the references: turns make range reduction exact.**
  Radian APIs must reduce mod 2π (irrational — every fixed-point trig library's precision
  wart); `angle_t` in turns reduces by bit-masking the fractional part, exactly. Only the
  per-quadrant polynomial is ported; our sin/cos is simpler and tighter than the source.
- **Three-layer oracle** (correctness is the risk — determinism is free in integer code):
  (1) **exhaustive** over all 2³² inputs for 32-bit unary functions — feasible offline in
  minutes per function, so no sampling; (2) **differential** vs vendored FixPointCS on its
  native Q32.32/Q16.16; (3) error-vs-double/MPFR bounds using fpm's published accuracy-test
  methodology as the template. Plus the PC-vs-Pi cross-compiled bit-compare test.
- No libm anywhere in sim code (§2 include firewall).

### 3.3 RNG

- Sim RNG: **stateless keyed** `rng_for(seed, tick, system_id, carrier_id)` — splitmix64-class
  mixing of the key tuple. No sequential generator whose draw order depends on scheduling
  (D11 / DETERMINISM-DESIGN keying, unchanged).
- Bounded ints via Lemire multiply-shift; unit-interval draws derive directly into fx formats,
  never through doubles.
- **Never `std::` distributions** (implementation-defined — the classic C++ lockstep desync;
  the mt19937 engine is portable, the distributions are not).
- CSPRNG (session auth, custody signing): OS entropy behind the platform seam, in a header
  physically unreachable from sim code. Resolves NETCODE R12.

### 3.4 Hash

- **Non-crypto** (state hashing, desync CRC, Map buckets, ids): one vendored/ported modern
  integer hash (rapidhash/wyhash/xxh3 class), **pinned seed, pinned implementation** — part of
  the lockstep contract; changing it is a build-hash bump. Per-arena incremental per tick
  (ALLOY §9.1 usage unchanged).
- `constexpr` FNV-1a gives compile-time `NameHash` (`"player_spawn"_id`) + a debug side-table
  (hash → literal) for inspector display. This is D12's hashed-string-id mechanism.
- **Crypto**: Monocypher (§1). BLAKE2b substitutes for the netcode doc's BLAKE3; Ed25519 as
  specified in NETCODE §11.6.

---

## 4. Memory: arenas + handles (DECIDED)

Ports D3 / ALLOY §9.1 / §13 / D16-M1 nearly verbatim.

**Four arena types, no general free():**
1. **`VMemArena`** — reserve large address space, commit pages on demand
   (`VirtualAlloc`/`mmap`). Stable bases forever → columns grow without relocation → transient
   raw pointers are safe within a pass. Zeroed pages from the OS solve padding determinism for
   fresh memory. ~150 lines/platform. Commit granularity kept explicit so ALLOY §13's
   per-chunk commit/decommit terrain arena is an extension, not a rewrite.
2. **Permanent arenas + the registered arena set** — each arena registers `(id, base, used)`;
   the registry is simultaneously the M1 snapshot unit (memcpy per arena, build-hash-stamped,
   fail-loud on mismatch), the per-tick hash unit (pass×pool desync bisection), and the rollback
   ring (T-F-04, N copies allocated once). Built day one (~50 lines).
3. **Frame/scratch arenas — one per worker thread**, bump + `reset()`, push/pop markers as the
   everyday API. Debug: poison `0xDD` on reset.
4. Slot reuse lives **inside `SlotMap`**, nowhere else. Wanting a general freeing allocator in
   the runtime is a design smell, not a missing feature.

**Handles:** one template `Handle<Tag, IDX_BITS, GEN_BITS>`; per-domain widths per D12
(`Entity = <22,10>` u32, resources u16 `<12,4>`). **Null = all-bits-zero and generation 0 is
never issued** (zero-init memory is never a valid handle). The generation-wrap arithmetic
(gate §7 finding B) is done per domain before widths are frozen. High-churn pool citizens
(Alloy particles) use plain indices with tick-scoped validity, not generational handles —
cross-tick identity is what handles are for.

**The rules that keep M1 honest:**
1. **No pointers in authoritative state — handles/indices only** (ALLOY §9.1 hard rule).
   Partially machine-enforced: debug reflection walk (§6) asserts no pointer-typed members in
   registered components; hand-rolled pools by review.
2. **Ticks allocate nothing**: arena-offset guard (record every registered arena's offset at
   tick start, assert only scratch moved by tick end — the Layr zero-alloc guard, §0.1 #6) plus
   a debug counting shim on the global allocator.
3. Whole-arena memcpy restore remains too coarse for island-scoped rollback — **T-A-01 survives
   the pivot exactly as filed** and is still the netcode's gate.

---

## 5. Containers + strings (DECIDED)

STL containers are triply disqualified (RAII contract vs no-destructors; unspecified/
implementation-defined iteration order and `std::hash`; compile-time weight). Build only what
has a consumer today:

| Container | Consumer | Key rule |
|---|---|---|
| `Array<T>` + `Span<T>` | everywhere | `static_assert(trivially_copyable)` — makes memcpy-grow/snapshot/hash *legal* (D4) |
| `SlotMap<T>` + `Handle` | D12/D14 | deterministic slot order; iterate `0..slot_cap()` never `0..count`; gen-wrap sums first |
| `Map<K,V>` open-addressing | registries, editor | fixed published hash, **pinned seed**, power-of-two. Walk order deterministic per insertion sequence but **order-fragile across refactors** — anything whose order outlives the binary uses sorted iteration; sim code keys on integers (LESSONS finding G transfers verbatim) |
| Sorted map/set (sorted array + binary search) | Luau-facing ordered containers | order = pure function of the key set; ~100 lines; no B-tree until proven |
| `RingBuffer<T>` | events, redundancy window | trivial |

Nothing else until pulled (the SparseSet lesson). Container tests are part of the determinism
harness: property tests vs a naive reference model + two-instance identical-order/identical-hash
checks.

**Strings — no general `String` class.** The runtime design has almost no strings by design.
Three narrow tools: `StrView {ptr,len}` (non-owning, ~30 lines); the **interner**
(`intern(StrView) → u16` + string arena + reverse lookup — the existing `id/` build item,
unchanged); a temp formatter over stb_sprintf. Owning copies are `arena_copy(StrView)`, not a
class. Firewall: no strings (or their hashes' order) in authoritative sim state — interned ids
and integers only inside the tick.

---

## 6. ECS + reflection (DECIDED)

**Own minimal ECS — the D4/D5/D6/D7 + FOUNDRY-API §3 spec, implemented as written.**
flecs rejected: its own allocators put gameplay state outside the registered arena set (breaks
M1/hashing), its determinism (ID recycling, table order, deferred merge) is an audit debt
re-paid per upgrade, and ~90% of its surface is unused — the audit costs more than building D6.
EnTT rejected on the template/compile-time rule alone.

- Entities + **type-erased paged sparse-set columns** on `VMemArena`
  (`register_component(id, size, align)`), queries = iterate-smallest + O(1) probe, **and stop**:
  no query DSL, no archetype graph, no change detection, no relationships (a `Parent` component
  + one pass when a consumer appears), no prefabs (Luau spawn functions + data tables), no
  observers (D15 `EventQueue<T>` as designed).
- **POD enforced at the door**: `static_assert(std::is_trivially_copyable_v<T>)` in
  `register_component<T>()` (+ the no-pointer reflection walk in debug). Same assert gates
  event types.
- **Systems are stateless free functions** + `SystemDesc {fn, label, phase, reads[], writes[],
  before[], after[]}` (D7 ordering: registration order, `before`/`after`, topo-sort,
  cycle-fatal). **Systems-as-singletons is banned** — singleton state escapes the snapshot ring
  (rollback desync the hash is blind to) and makes the two-worlds-one-process run-twice test
  unwritable. Persistent system state = a **singleton component** (world resource) in a
  registered arena.
- Gameplay entities are the *small* population; Alloy's particles/bodies stay in their own SoA
  pools with plain indices (ALLOY §1.1 unchanged).

**Reflection = one X-macro per component** (~200 lines of infra, replaces everything flecs was
for): field list → POD struct + `constexpr FieldInfo[] {name, name_hash, kind, offset}` where
`kind` is a closed enum (fx palette rows, sized ints, bool, Handle domains, StrId, fixed
arrays). One table feeds four consumers:
1. **Generic ImGui inspector** — walk fields, switch on kind; every component inspectable the
   moment it is declared. Optional per-component custom-draw hook (curve editors etc.) and
   optional per-system `debug_draw(world)` for overlays — hooks are the override, the generic
   walker is the mechanism. Dev builds only; editor mutations go through the deferred command
   buffer, never mid-frame column pokes.
2. **M2 durable saves** — generic name-keyed encoder/decoder over the same tables (renames via
   alias entry, added fields via declared defaults). This *is* the D16/ALLOY §9.2 mechanism;
   gate Task A's disk half is answered by construction.
3. **Debug/desync dumps** — field-by-field diff of a diverging component.
4. **Luau editor/tool access** by field name (hot gameplay bindings stay hand-written).

---

## 7. The Luau layer (DECIDED)

Role: games bring **data + meaning** — tables, tuning, gameplay systems, spawn logic, UI.
Script reload is the iteration mechanism (replaces game-DLL hot reload).

**The state boundary — the load-bearing rule:**

> **Authoritative state never lives in the Luau heap.** Scripts read the world and write only
> through components (reflection glue) and the ordered edit/command channel. Script-side tables
> are transient working data, reconstructible from world state, never carried across ticks.

Rationale: the Luau heap is not in the registered arena set; script-held state would survive
rollback while the world rewinds — a desync generator per-arena hashing is structurally blind
to (the singleton problem, bigger). This rule makes rollback/hashing/saves correct for scripts
*by construction*, with zero Luau-specific snapshot machinery.

Sim VM restrictions (scripts are inside the lockstep contract — every peer runs them, so they
ship identically and their **compiled bytecode joins the build fingerprint**):
- Stock `math` library **removed** (not supplemented) from the sim VM — det math bindings
  replace it. `os.*`, `io.*`, clock: absent.
- No iteration over hash-part tables in sim code — the C++ sorted map/set bindings are the
  ordered containers. Array-part iteration is fine.
- Luau numbers are f64: basic `+−×÷` is bit-deterministic cross-platform in the interpreter,
  so plain script arithmetic is safe **while values stay in the exactly-representable integer
  range**; fx values cross as integers/light userdata, never round-tripped through doubles.
- **Interpreter only in the sim VM** (native codegen is another codegen surface; revisit only
  with a measured need and a differential test).
- **Debugger scope — RULED 2026-08-21, ceiling = Tier 1.** Tier 0 lands with v0 tooling:
  tick-stamped script logging to the ImGui console, error traces with file:line, and
  record→replay scrubbing — determinism already is time-travel debugging. Tier 1
  (break-and-inspect) is built when real gameplay scripting starts, not before: gutter
  breakpoints in ImGuiColorTextEdit via Luau's native debugger interface
  (`lua_breakpoint` / debug interrupt / single-step — Roblox Studio's own path, so
  integration not research), pause-the-whole-sim on hit, stack + locals/upvalues panel,
  step-line/step-in/resume. Dev builds only, compiled out of netcode/ship (§2's build-flag
  rule); interpreter-only composes with this (Luau NCG does not support debugging).
  **Tier 2 (IDE-class: watches, conditional breakpoints, live variable editing) is
  rejected** unless daily scripting demonstrably outgrows Tier 1 — a breakpoint shows one
  moment, a replay shows every moment repeatably, so Tier 2's marginal value does not
  survive a cost look for a one-person project.
- Two VMs: restricted sim VM; unrestricted editor/UI VM. Luau's allocator hooked to an arena;
  GC given an explicit per-tick step budget (GC timing cannot change values, but its CPU spike
  can blow the frame).
- Sequencing stays **InputFrame (Option A)** — NETCODE §4.2 unchanged.

---

## 8. Netcode deltas (DECIDED)

NETCODE-DESIGN rev 3 stands in full above the transport. Changes:

- **Transport = ENet.** Channel map: `INPUT` → unreliable(-sequenced) channel carrying our own
  `REDUNDANCY_TICKS` window (**never a reliable channel** — retransmit + per-channel
  head-of-line is the failure §5.2 rejected TCP for); `CONTROL` → own channel; `BULK` → ENet
  reliable + fragmentation (replaces the TCP side-channel; one socket). §5.5's NAT ruling is
  untouched and still open.
- §1.1's "already shipped in Ore" table is void; each row maps to §3–§5 of this doc or to ENet.
- Build fingerprint (handshake + hash chain) = hash of (compiler version, flags, source tree,
  sim-script bytecode, data tables) — §11. Replaces build-hash + `@layout_hash` +
  `@fast_math`-flag fingerprinting. **The `@preserve_layout`+`@layout_hash` replacement —
  `WIRE_STRUCT` discipline** *(ratified 2026-08-21)*: every wire/checkpoint/chain struct is
  explicitly padded, with `static_assert` on `sizeof` **and every field's `offsetof`** — the
  byte layout is pinned at compile time, which is *stronger* than a fingerprint (a drift is a
  compile error, not a handshake mismatch). The §6 reflection field tables (name-hash + kind +
  offset) hash into the handshake as the cross-peer layout check. Format version byte,
  append-only growth, as before.
- R12 resolved by §3.3 CSPRNG + Monocypher. R14, T-O-03/04, INV-8, denormal policy: void
  (no floats). R3/T-A-01 (closure restore), R6, R10, R11: unchanged and still open.
- **R1 (hidden information): RULED 2026-08-21 — full-world visibility, closed.** The games are
  8-player co-op with everyone seeing the whole world (genre-normal: Valheim/Terraria/Factorio).
  INV-1 and the entire netcode design stand as written; no fog-of-war ambition is carried.
  Reopening this later means reworking the netcode design — it is a redesign trigger, not a
  feature request.

---

## 9. Modules in one exe (DECIDED)

- One static exe; modules are folders/static-lib targets with disciplined include paths
  (private headers → a seam violation is at least a visible cross-boundary include, at best a
  link error). Registration order is explicit in one wiring file — the MODULES §3 load-order
  hazard un-exists.
- The **promotion ladder + gate discipline survive** as organizational policy (pulled-never-
  pushed, two structurally-different consumers, no game nouns in a promoted API, headless
  tests). The DLL machinery (manifest sha256, loader, ABI version skew, per-module reload,
  `foundry_module_create`) is retired.
- Cross-module rules unchanged in spirit: shared state + events + ordered command channels;
  never system→system calls; the downward DAG holds.

---

## 10. Gates and build order (DECIDED, subject to §12 answers)

```
ASSAY    REPURPOSED (ruled 2026-08-21): the jam-scale probe survives as the shakedown of the
         NEW stack (C++/Luau/SDL direct, no engine) — its Ore-friction-journal deliverable is
         void, its §2.4 gate (one 15-second clip a stranger can read) is kept: that test was
         never about Ore and still gates the commercial thesis. Timing vs GATE 0 is flexible;
         they share no code.
GATE 0   Fixed-point XPBD + PBF convergence & cost bench (headless).      ← THE pivot gate
         Runs against the §3.1a/b palette so a failure names the row that is wrong.
         No engine: fx/det-math headers + minimal solver loop + CSV out.
         Thresholds CONFIRMED at the 2026-08-21 veto pass (NETCODE §19.11's rule:
         the answer is not negotiated after results exist — these numbers are frozen).
         Fallback, pre-committed: if G-01..G-04 cannot pass after palette adjustment
         → pinned-toolchain float, x86-64 only, cross-ISA (the Pi) consciously
         written off at that moment, rev-bumped here.

         | ID | Scenario | Pass | Investigate | Fail |
         |---|---|---|---|---|
         | G-01 | 10-box stack at rest, 10k ticks | p95 jitter < 0.1 texel, zero sink/pop | < 0.5 texel | creep/pop/oscillation |
         | G-02 | Feather-on-boulder at full 4096:1 clamp, impact + rest | sustained penetration < 1 texel, no tunneling at V_MAX | < 2 texels | tunneling or saturation hit |
         | G-03 | 5k-particle PBF column settling | p95 rest-density error < 2%, stays settled | < 5% | undamped "boiling" |
         | G-04 | Sealed mixed scene, 1e6 ticks | monotone non-increasing energy envelope | slow bounded drift | energy growth |
         | G-05 | Cost sweep 10k/20k/50k particles + 2k bodies | 20k ≤ 4 ms PC AND ≤ 12 ms Pi | ≤ 8 ms PC / > 12 ms Pi | > 8 ms PC at 20k |
         | G-06 | All of the above run twice + PC-vs-Pi cross-compiled | 100% identical hash traces | — | any divergence (= UB, hunt with UBSan) |

         G-05 severity split (ruled 2026-08-21): the PC half and the Pi half are
         different failures. **PC > 8 ms at 20k is pivot-level** — fixed point can't
         hold the budget on the primary platform, the fallback ladder fires. **A
         Pi-only miss is NOT** — it redraws minimum spec (the Pi drops from reference
         peer to stretch peer, the 3-machine set becomes PC + Deck + best-effort Pi)
         and never triggers the float fallback, since abandoning fixed point to
         rescue the Pi would surrender the cross-ISA property that exists FOR the Pi.
         G-05 otherwise re-derives the ALLOY §11.2 budget for fixed point — if 20k
         doesn't fit, the budget moves (counts, substeps), not the verdict.
         G-06 should pass trivially; "expected" is not "tested", and a failure there
         is the highest-information result the bench can produce.
         Diagnostic instrument (ratified 2026-08-21): the FLOAT-SHADOW build — the
         solver compiles once more over `double` in a dev-only config (cheap: the
         solver is already written over palette typedefs). Running fx and shadow
         side by side localizes precision loss to the constraint and pass where
         they diverge, turning a Gate 0 failure from "it jitters" into "row X,
         step Y". Never in netcode/ship builds; never authoritative.
         Substep sweep (ratified 2026-08-21): G-01..G-05 run at substeps 4/8/16, not
         only the default 8 — substep count interacts with quantization non-obviously
         (smaller h = smaller corrections, closer to the pos_t quantum), so the
         interaction is measured, not assumed; the sweep may move §3.1a's substep
         constant, which is a constant change, not a palette change.
         On a convergence failure, the response order is §3.1b's precision ladder,
         climbed rung by rung — the float fallback fires only when the ladder is
         exhausted.
FOUND    Foundation week(s): VMemArena + scratch + registry + offset-guard · containers ·
         fx palette + det math · keyed RNG · pinned hash · StrView/interner ·
         test runner + determinism harness (run-twice, record→replay, per-arena hash).
ECS      Minimal ECS + X-macro reflection + generic inspector + M2 encoder (~3 weeks total
         with FOUND).
V0       Window + moving sprite + fixed 60 Hz + clean exit (unchanged milestone), ImGui shell.
HOVEL    3 machines (PC/Deck/Pi), ENet, integer lockstep — NETCODE §19 transfers almost
         verbatim and gets cheaper (fixed point = Milestone A is the whole story).
         Proves the cross-ISA claim on the new stack early. Hovel Milestone E (the
         10 h three-machine soak) is NAMED (ratified 2026-08-21) as the successor to
         Ore's 43 M-tick soak: that run validated Ore's strict float, which no longer
         runs — its evidence does not transfer, its method does, and the fixed-point
         claim it must now prove is strictly easier than the one it proved.
T-A-01   Closure-scoped restore prototype — unchanged, still gates speculation vs delay-only.
M2       Alloy build queue as in TODO.md (headless-first), on the new foundation.
```

v0 stays **single-threaded**; the job system (§12 Q) slots into the phase structure later,
exactly as the original plan had it.

---

## 11. Toolchain & build (DECIDED)

- **Clang everywhere** *(ruled 2026-08-21)*: clang-cl/clang on Windows, clang on Deck, clang
  cross-target for the Pi. One compiler family deletes MSVC-vs-clang codegen/UB behavior as a
  determinism variable; MSVC stays available as an occasional second-opinion build, never a peer.
- Pinned compiler versions recorded in-repo; pinned flag sets; `-fno-exceptions -fno-rtti`;
  warnings-as-errors; UBSan/ASan jobs in the determinism CI.
- Pi 4 builds cross-compiled from the PC (aarch64 clang/gcc target) — build once, deploy three.
- Unity builds; vendored deps compiled once; **rebuild time is a CI-tracked budget** (§2).
- Build fingerprint as §8. Fixed point makes codegen differences unable to change results —
  except through UB, which is why sanitizers stay in the gate.

---

## 12a. Job system design (DECIDED shape, 2026-08-21; built post-v0)

Replaces Ore `jobs`. v0 ships the API single-threaded; the parallel impl lands before any
parallel Alloy code. INV-7 (worker-count invariance) is the requirement it exists to satisfy.

- **The one rule everything follows from: outputs are keyed by CHUNK id, never worker id.**
  `parallel_for(range, grain)` splits the range into chunks by a pure function of `(N, grain)`
  — never of worker count, timing, or arrival. Per-chunk outputs land in chunk-indexed slots;
  merges/reductions fold in chunk-index order. Worker identity is then invisible to results,
  which means **work-stealing is permitted for free** — scheduling affects wall time only.
  (This is stronger than Ore `jobs`' worker-index merge, which forbade stealing.)
- Per-worker scratch arenas (§4) and per-worker command buffers exist for allocation locality,
  but command buffers are *tagged by chunk id* and applied at the barrier in chunk order (D5's
  deterministic-order rule, restated for the new key).
- Reductions: integer/fx sums are order-free anyway (the fixed-point dividend); the
  chunk-order fold is kept as the rule regardless, so nothing breaks if a widened or
  non-commutative combine ever appears.
- **Colored Gauss-Seidel host**: colors become sequential levels; within a level,
  `parallel_for` over the color's constraint list in stable-id order chunks (ALLOY §8.1's
  coloring rules unchanged: persistent constraints color once, contacts recolor per tick,
  deterministic greedy in stable-id order).
- **Gates** (T-F-02 transfers verbatim): identical hash trace at 1/2/8/16 workers is a
  blocking release gate; plus one mixed-pair run (peer A at 4 workers, peer B at 16, same
  inputs) once transport exists.

## 12. OPEN — decisions still needed (ask, don't drift)

*(Resolved 2026-08-21 and folded into the sections above: Windows compiler → clang everywhere
(§11) · Assay → repurposed (§10) · hidden information R1 → full visibility, closed (§8) ·
error model → Result-struct returns (§2) · job system → designed, §12a · fx palette →
proposed with derivations in §3.1a/b, pending Gate 0 · `MAX_ACTIONS` → **32**, a
compile-time constant in the input header; changing it is a wire-format version bump, and
every NETCODE payload figure already assumed 32 · repo → **this repo continues** as the
engine repo — the surviving design docs are the asset; evidenced by this doc living here.)*

Remaining, in intended order of resolution:

1. ~~Confirm the §3.1a world constants and §10 thresholds~~ **DONE (veto pass 2026-08-21)** —
   texel 1/16 m + technical constants confirmed; extent tightened ±16,384 → **±4,096 m**
   (4× finer `pos_t` quantum, §3.1b re-derived); Gate 0 thresholds frozen with the G-05
   severity split (PC-half failure = pivot-level; Pi-only = min-spec redraw, never the float
   fallback). **Gate 0 is now fully specified and unblocked** — next stop is the dev machine:
   fx/det-math headers + the bench. On results, §3.1b rev 2 gets the DECIDED stamp (or the
   pre-committed fallback fires).
2. ~~Luau step-debugger scope~~ **RULED 2026-08-21 — ceiling = Tier 1** (§7): Tier 0
   (console + replay scrub) at v0; Tier 1 (break-and-inspect) when gameplay scripting
   starts; Tier 2 rejected. Details in §7's debugger bullet.
3. **Doc estate sweep — deliberately LAST** *(ruled 2026-08-21)*: retire `FOUNDRY-ORE-GATE.md`
   + Ore-asks blocks; amend ALLOY (§1.3/§10 mappings), NETCODE (§1.1/§5 rows), CLAUDE.md
   (principle 1, axes, build skill), TODO (queue rewrite); this doc is the ruling record. A
   dedicated session, one commit per doc. **Until it runs, this doc + `FX-PALETTE.md`
   override the older docs wherever they conflict** — a session starting from CLAUDE.md alone
   will read a pre-pivot world; read this doc first.

---

*Rev 1 — written from the 2026-08-21 pivot discussion. Supersedes: the Ore dependency
(`FOUNDRY-ORE-GATE.md` in full), CORE §7's DLL mechanics + `FOUNDRY-MODULES.md`'s binary
machinery (discipline survives, §9), ALLOY §10's float-solver rationale (re-litigated, Gate 0
decides), D2's Ore error model (§12.4 pending). Everything else in the doc map stands.*
