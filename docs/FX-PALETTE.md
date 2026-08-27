# The fx palette and det math (tidelock, rev 2)

> **Status:** rev 2, 2026-08-25 — the Gate 0 decision commit. Rows **DECIDED against measurement**
> (`tests/gate0/results/2026-08-25-pc-win-netcode/README.md` + the pc2 reproduction beside it;
> bit-exact across two machines, netcode and dev-shadow tiers). **No row was moved by a failure**;
> every Gate 0 FAIL was solver design or scenario spec (`TODO.md` RR-10..RR-15). Two rev-2 edits,
> both within the derivation rule: `omega_t` retuned to its structural cap (§3, §9 R-8) and
> `lambda_t`'s i64-accumulator status hardened from "likely" to REQUIRED (§3.2 rung 1, §9 R-7).
> The float fallback did **not** fire. `fx_palette.h` is written from this doc; the rev-2 header
> edit (the `omega_t` alias + op-table split + trace-pin refresh) is the commit after this one.
> **Lineage:** expands `PIVOT-DESIGN.md` §3.1–§3.2. PIVOT is the ruling.
> **Owns:** `src/foundation/fx.h`, `fx_palette.h`, `det_math.h`, `fx_float.h`.

---

## 0. The substitution this whole engine rests on

Fixed-point sim arithmetic replaces compiler-enforced strict float with **determinism by
construction**: integer add/sub/mul/shift are bit-exact on every ISA, compiler, and optimization
level. The cost is range/resolution bookkeeping — which is why this is a *typed* palette with a
*closed* set of rows and *no implicit conversions*, not a "fixed-point number" type. The bookkeeping
is the compiler's job; the palette is how we hand it over.

**No floats in authoritative state or on any sim path.** Floats remain legal render-side, in the
editor, and in offline tools (`fx_float.h` is the only bridge, §6).

---

## 1. The mechanism — `fx<Rep, FRAC>` (DECIDED)

```cpp
template <typename Rep, int FRAC> struct fx { Rep v; };   // one header, no system includes (layout: §7)
```

- **`+`/`-` compile only between identical formats.** Saturating variants `sat_add/sat_sub` exist;
  the plain operators wrap (two's complement, explicit) — quanta paths must use `sat_*`
  (`CPP-SUBSET.md` §5).
- **`*`/`/` are never operators.** They are named helpers taking the *result* format as a template
  parameter, with a widened intermediate inside and the narrowing point visible at every call site:
  ```cpp
  template <typename R, typename A, typename B> R mul(A a, B b);   // i64 (or mulhi) intermediate
  template <typename R, typename A, typename B> R div(A a, B b);   // i64 intermediate
  ```
  The shift `(A::FRAC + B::FRAC - R::FRAC)` is a compile-time constant folded into every op.
- **Rounding is round-to-nearest-even in `mul<R>`/`div<R>`, day one** (precision ladder rung 2).
  Truncation biases every op downward — systematic energy drain and convergence stall — and RNE
  costs ~1 extra add per op. Conversions `to<R>(x)` also RNE when narrowing.
- **No implicit conversions, ever.** Every constructor is `explicit`; `to<R>(x)` is the only
  conversion and it is greppable. `fx` ↔ integer: `fx_int<R>(i)` / `fx_to_int_floor(x)`.
- **Rows are keyed by format, not by name (RULED at rev 2 — stays; `TODO.md` RR-5 closed
  2026-08-25).** `using pos_t = fx<i32,18>` and `using invmass_t = fx<i32,18>` are one C++ type,
  as are `q_t`/`stiff_t`/`angle_t`/`dt_t` and `scalar_t`/`lambda_t`. The compiler checks *scale*,
  never units: the mixed-op table (§3.1) has one line per distinct format triple and admits every
  row of those formats. A tag parameter (`fx<Rep,FRAC,Tag>`) would buy unit checking at the cost
  of an explicit `to<R>` at every unit change and a larger op table; no desync class found by
  Gate 0 or the W1 reviews would have been caught by tags, so the retag is not paid. (`vel_t`/
  `omega_t` ceased to share a format at rev 2 — the §9 R-8 retune, not a tagging decision.)
- **Comparisons** between identical formats only. `abs`, `min`, `max`, `clamp`, `sign` per format.
- **Width is a performance decision.** No usable 64-bit SIMD multiply exists on SSE2/AVX2/NEON, so
  every column the solver should vectorize is a 32-bit format. `fx<i64,·>` rows exist only where
  the derivation rule forces them.
- **Runtime scale (shift stored in data) is REJECTED**: it deletes the compile-time mixed-scale
  check, which is the main reason to do this in a typed language.
- **Ad-hoc per-field formats are FORBIDDEN.** A new format is a new palette row is a design
  decision recorded here. N² mixed-op decisions are exactly what the palette prevents.

---

## 2. World constants (CONFIRMED — veto pass 2026-08-21)

| Constant | Value | Reasoning |
|---|---|---|
| world unit | **1 unit = 1 m** | human-scale side view; an agent capsule ≈ 1.8 units |
| `TEXEL` | **1/16 m (6.25 cm)** | Noita-class carve legibility at gameplay zoom; an Alloy terrain chunk (128² texels) = 8 m, a sane streaming unit |
| world extent | **±4,096 m (8 km span)** | ~1.6× a Terraria-large world; the freed bits go to position precision (4× the stacking headroom of the ±16 km draft). Raising it later is a palette-rev edit before saves exist, a migration after; beyond it is streaming (`ALLOY.md` §13), never coordinates |
| `V_MAX_WORLD` | **512 m/s** | validator-enforced cap (T-A-02); faster effects are raycasts, not integrated bodies |
| tick / substeps | **60 Hz, 8 substeps → h = 1/480 s** | h, h² are precomputed rounded fx constants (1/480 isn't dyadic; one shared constant is deterministic by construction) |
| `MASS_RATIO_CLAMP` | **4096 : 1 (2¹²), applied as an effective per-pair clamp** | per solve, each pair's inv-mass spread saturates at the clamp; content is never refused, extreme pairs behave as 4096:1 (past ~1000:1 the light body reads massless anyway). Statics are `inv_mass = 0` exactly and cost no range |

Changing any of these re-derives §3 mechanically (derivation rule below) — it is a constant
change, not a redesign.

---

## 3. The palette rows (DECIDED — verified by Gate 0, 2026-08-25)

**Derivation rule:** integer bits ≥ ⌈log₂(range × margin)⌉, the rest is FRAC; 32-bit wherever the
solver should vectorize. Each row carries a range/resolution line; the `init()` table validator
checks game data against these ranges (same slot as `v_max`).

| Type | Format | Range | Resolution | Notes |
|---|---|---|---|---|
| `pos_t` | **fx<i32,18>** | ±8,192 m | 3.8 µm (1/16384 texel) | 2× margin over the extent. **The risk row**: the XPBD correction quantum is 3.8 µm; resting-contact jitter at this floor is what G-01 measures. Fallback if G-01 fails after the ladder: `fx<i64,32>` world pos + 32-bit chunk-local solve deltas (rung 4) |
| `vel_t` | **fx<i32,20>** | ±2,048 m/s | ~1 µm/s | 4× margin over `V_MAX_WORLD` (solver transients overshoot) |
| `invmass_t` | **fx<i32,18>** | ±8,192 | 3.8e-6 | unit mass = the reference particle; inv_mass ∈ [0,4096] under the clamp, 2× headroom — `w₁+w₂+α̃` cannot overflow by construction |
| `stiff_t` (α̃ = α/h²) | **fx<i32,30>** | ±2 | 9.3e-10 | near zero for stiff constraints; precision matters, range doesn't. Tables store α (as `q_t`-scaled data), α̃ precomputed at init with an i64 divide |
| `q_t` (normalized/unitless) | **fx<i32,30>** | ±2 | 9.3e-10 | **kernel strategy:** PBF/SDF kernels evaluate on q = r/h_kernel ∈ [0,1] — normalize once per pair, polynomial in `q_t`, scale back once; kernel precision becomes world-scale-independent. Also density ratio C = ρ/ρ₀−1, friction coefficients, restitution, blend weights |
| `angle_t` | **fx<i32,30>** | ±2 turns | ~1e-9 turn | **turns, not radians** — wraps free at ±1 by masking; sin/cos index naturally |
| `omega_t` | **fx<i32,22>** | ±512 turn/s | ~2.4e-7 turn/s | angular velocity. **Retuned at rev 2** (§9 R-8): the implicit encoding `pθ = θ − ω·h` caps \|ω\| at `inv_h/2` = 240 turn/s at 480 Hz whatever the row holds (measured, G-02b), so the old ±2,048 was ~90 % unreachable headroom; ±512 is 2× margin over the structural cap, buying 4× resolution. No longer the same format as `vel_t` — angular and linear velocity are distinct C++ types from rev 2 on |
| `dt_t` | **fx<i32,30>** | ±2 s | 9.3e-10 s | h, and only h. h² is never a runtime operand (α̃ is precomputed); `inv_h` is the plain integer 480 |
| `scalar_t` | **fx<i32,16>** | ±32,768 | 1.5e-5 | unitless scalars outside the solver: quanta-path coefficients, animation speed, modifiers, utility scores (§9 R-5) |
| `lambda_t` | **= `scalar_t`** (accumulated in i64) | ±32,768 m·mass | 1.5e-5 | XPBD Lagrange multiplier (length × mass units); an alias of `scalar_t`. **Rev 2: the row did not move — the accumulator ruling hardened instead** (§9 R-7): λ is an i64 frac-30 local across the substep's sweep, narrowed once per substep (`lam_narrow`, RNE by 14, saturation counted); per-constraint narrowing is BANNED — measured to creep a resting box 12 quanta/tick (`--ladder 0`, G-01). `lambda_t` is storage only |
| conserved quanta | **plain i32 / i64** | — | — | mass-quanta, moles, charge, load: integers, saturating ops only (`ALLOY.md` §10). Never an fx row |

Stress-case mapping: the feather→boulder denominator is `invmass_t`'s clamp; the sub-texel
correction vs world-extent spread is the `pos_t` vs `lambda_t`/delta split. **Gate 0 runs against
these rows, so a failure names the row that is wrong.**

### 3.1 The mixed-op table (the only sanctioned products)

Enumerated in `fx_palette.h` next to the types; all through `mul<R>`/`div<R>`; no other
combination compiles because no other helper is instantiated.

| Product | Result | Where |
|---|---|---|
| `vel_t × dt_t` | `pos_t` (delta) | integrate / predict |
| `(pos_t − pos_t) × inv_h (int 480)` | `vel_t` | implicit velocity `v = (x − x_prev)/h` |
| `invmass_t × lambda_t` | `pos_t` (delta) | constraint projection Δx = λ·w·∇C |
| `q_t × q_t`, `q_t × pos_t`, `q_t × vel_t` | same as the non-q operand | kernel weights, friction, damping, normals (`∇C` is a unit vector in `q_t`) |
| `pos_t × pos_t` | `fx<i64,36>` (local, never stored) | squared distance before `sqrt<pos_t>` |
| `omega_t × dt_t` | `angle_t` (delta) | rotate |
| `angle_t` → `(sin, cos)` | `q_t`, `q_t` | det math |
| `stiff_t + invmass_t (+ invmass_t)` | `invmass_t`-ranged `fx<i64,30>` local | the XPBD denominator, widened; **one `rne_div` on the raw i64 bits** (§9 R-6: `div<R>` is 32-bit; the caller spells the shift — `ALLOY.md` §14.4.3). A body's angular share `inv_I·(r×n)²` joins the den as the same i64 frac-30 local and is **never narrowed into `invmass_t`** (§9 R-8: it overflows the row for any light plank — measured, G-02) |
| `invmass_t = div(unit, quanta·unit_mass)` | at creation only | i64 divide, not a per-tick op |

Precomputed constants (rounded once, shared): `H = dt_t(1/480)`, `G_SUBSTEP = vel_t(9.81·h)`,
`H_SQ_INV` used only inside the init-time α̃ computation.

### 3.2 The precision ladder (DECIDED — mandated response order to a Gate 0 failure)

Climb rung by rung; the float fallback fires only when the ladder is exhausted.

1. **Widened accumulate + round-once-per-substep — REQUIRED, not a rung to climb (hardened at
   rev 2, §9 R-7).** Solver-local positions, velocities and λ stay i64 across a substep's
   constraint sweep; rounded to storage format once. Most of 64-bit's precision at zero
   storage/bandwidth cost. Gate 0 measured the alternative: per-constraint λ narrowing creeps a
   resting box 12 quanta/tick while the double shadow sits still (`--ladder 0`, G-01).
2. **RNE in `mul<R>`, day one** (§1).
3. **Residual carry** — per-quantity rounding residual fed back next substep (error diffusion,
   deterministic): a standing Gate 0 bench variant, adopted only if rungs 1–2 leave a stall.
4. **Wide state, narrow math — the named fallback:** `fx<i64,32>` stored positions; constraint
   math on 32-bit deltas against a local island/chunk origin. Width where error accumulates,
   narrowness where throughput lives; SIMD and cache traffic survive.
5. **Pipelined sim thread — throughput escape only, never a precision fix** (a G-05 miss, after
   T-A-01 reports; +1 frame latency; complicates the netcode barriers).

**Uniform 64-bit everywhere is REJECTED** (doubles cache-line traffic on the hot columns for
precision that is only needed at accumulation points — rung 4 dominates on every axis).

---

## 4. Det math (sourcing RULED 2026-08-21: own core, ported kernels)

### 4.1 What is ours and what is ported

| Piece | Source | Why |
|---|---|---|
| core arithmetic, palette, `mul<R>`/`div<R>`, saturating tier, turns angles, mixed-op table | **ours** (~300 lines) | these are *policy*; no library ships policy, and adopting one means wrapping its whole surface anyway |
| `sqrt`, `sin`/`cos`, `atan2`, (`exp`/`log`/`pow` when a consumer appears) | **ported from FixPointCS** (github.com/XMunkki/FixPointCS, MIT, attributed in the header) | built for deterministic lockstep sims, bit-identical across compilers by design, precision-documented polynomials; porting transfers the silent-bad-polynomial risk to code that has shipped in deterministic games for years |

Survey verdict recorded so it isn't re-run: libfixmath (abandoned 2012, Q16.16 only) — rejected;
fpm (drop-in-float philosophy, radians, no palette policy) — oracle only; CNL/SG14 (template-heavy)
— banned by `CPP-SUBSET.md` §2.

### 4.2 The API (DECIDED shape)

```cpp
// det_math.h — sim-safe, zero includes beyond fx.h
template <typename R, typename A> R   sqrt(A x);           // i64 Newton/FixPointCS integer sqrt; x<0 → 0 (+ TL_ASSERT)
template <typename R, typename A> R   rsqrt(A x);          // = div<R>(one, sqrt(x)); NEVER an estimate instruction
q_t sin(angle_t a);  q_t cos(angle_t a);  void sincos(angle_t a, q_t* s, q_t* c);
angle_t atan2(pos_t y, pos_t x);                           // also a q_t overload
template <typename R, typename A> R   lerp(A a, A b, q_t t);
u32 isqrt32(u32 x);  u64 isqrt64(u64 x);                  // plain integer floor sqrt for quanta paths (§10.3 is the signature's home)
```

**One deliberate deviation from the references: turns make range reduction exact.** Radian APIs
must reduce mod 2π (irrational — every fixed-point trig library's precision wart); `angle_t` in
turns reduces by masking the fractional bits, exactly. Only the per-quadrant polynomial is ported;
our `sin`/`cos` is simpler and tighter than the source.

`normalize(vec2<pos_t>) → vec2<q_t>`: squared length in `fx<i64,36>`, `sqrt<pos_t>`, then two
`div<q_t>`. Zero-length → `(0,0)` and a `TL_ASSERT` in debug (callers guard first; the contact and
PBF code never normalizes a zero vector by construction).

### 4.3 Vector/matrix types (DECIDED — thin, sim-side)

`vec2<T>` for palette rows (`vec2<pos_t>`, `vec2<vel_t>`, `vec2<q_t>`), component-wise `+`/`-`,
`dot<R>`, `cross<R>`, `rotate(vec2<pos_t>, q_t sin, q_t cos)`. No general matrix type on the sim
side (2D rigid transforms are `(pos, sin, cos)`); `mat3` lives render-side in float
(`RENDER2D.md`).

### 4.4 Three-layer oracle (DECIDED — correctness is the risk; determinism is free)

1. **Exhaustive** over all 2³² inputs for 32-bit unary functions (`sin`, `cos`, `sqrt` per row) —
   minutes offline per function; no sampling.
2. **Differential** vs vendored FixPointCS (tools-only, C#/C++ reference) on its native Q32.32 /
   Q16.16 — proves the port.
3. **Error bounds at arbitrary precision** — `tools/fxcheck/oracle.py`, Python + `mpmath` at 60
   digits (`BUILD.md` §9 R-4: offline tools are Python until one must link a vendored library;
   MPFR is not vendored and a hand-rolled oracle would be worth nothing as a check on the thing
   it checks). It emits the coefficient block (verbatim sin integers; the atan table pre-scaled
   to turns) and re-checks it in CI, emits the correctly-rounded reference tables the fast tests
   use, and re-evaluates the worst cases layer 1 reports. Bounds are recorded as constants in
   `det_math.h` (`FX_*_MAX_ERR_ULP`) and asserted by the tests.

Layers 1–2 are C++ (`tools/fxcheck/fxcheck.cpp`, vendored FixPointCS under
`tools/fxcheck/vendor/`), exempt from the subset. Plus the cross-ISA bit-compare across the
`CANON.md` target legs (`TESTING.md` §4).

**Measured at rev 1 (2026-08-23, exhaustive):** `sin`/`cos` max |err| **9.06 ulp** of `q_t`
(the reference `SinPoly4` is "27.13 bits" — 7.3 ulp of Q30 — before our RNE steps); `sin²+cos²`
within 18 ulp; `atan2` max 4.34 ulp (2²⁴ samples + every octant boundary) and `atan2(sin a,
cos a)` returns `a` within 4 ulp; `sqrt` is correctly rounded (0.5 ulp) by construction. The
symmetries (`sin(−a) = −sin a`, `sin(a+¼) = cos a`, `atan2(x,y) = ¼ − atan2(y,x)`) are
bit-exact, which the reference's truncating `Qmul30` does not give. A tighter sine is a ruling
(`TODO.md`), not a quiet refit: FixPointCS ships nothing better, so it would be a bespoke kernel.

---

## 5. Keyed RNG on fx (DECIDED — `DETERMINISM.md` §3 owns the generator)

Unit-interval draws derive directly into fx formats: `rng_q(key) → q_t` takes the top 30 bits of
the 64-bit mix; `rng_range<R>(key, lo, hi)` is `lo + mul<R>(rng_q, hi − lo)`. Never through doubles.
Bounded integers via Lemire multiply-shift (`rng_below(key, n)`).

---

## 6. The float bridge — `fx_float.h` (DECIDED)

```cpp
float  to_f32(pos_t x);    // x.v * 2^-18 — exact for |x| < 2^24 raw; render only
double to_f64(...);        // editor/tools
pos_t  from_f32_quantized(float x);  // RNE to the row quantum; INPUT capture and editor writes only
```

Rules: **this header is unreachable from sim TUs** — the symbol audit's grep bans the tokens
`float`/`double` in `src/sim/` and the det halves of `src/foundation/` (`CPP-SUBSET.md` §4).
Render-side, `to_f32` of a `pos_t` loses nothing visible (float has 24 bits of mantissa; world
positions up to ±4,096 m at 3.8 µm need 31 — the loss is below a texel at the far edge and the
render is interpolated anyway). Editor writes into sim state go through `from_f32_quantized` then
the command channel, never direct pokes.

Luau: fx values cross as raw integer bits (`v`), never as doubles of the scaled value. Every 32-bit
row is exactly representable in a Luau number; `fx<i64,·>` rows (rung 4 fallback only) would not
be — if rung 4 ever fires, `LUAU-LAYER.md` §3 gains a boxed-i64 rule.

---

## 7. Header layout (DECIDED)

| File | Contents | Includes |
|---|---|---|
| `fx.h` | `fx<Rep,FRAC>`, `mul/div/to`, sat/wrap helpers, compare/abs/min/max/clamp | `tl_types.h` (widths), `tl_assert.h` (the range asserts - the R-3 panic ABI, `CPP-SUBSET.md` §9) |
| `fx_palette.h` | the rows as named types, `static_assert`s on the derivation rule, the mixed-op instantiations, world constants, precomputed `H`, `G_SUBSTEP` | `fx.h` |
| `det_math.h` | `sqrt/rsqrt/sincos/atan2/isqrt/lerp`, `vec2<T>`, normalize, rotate; FixPointCS attribution | `fx_palette.h` |
| `fx_float.h` | the bridge (§6) | `fx_palette.h` — render/editor/tools only |

Everything lives in `namespace fx` (`CPP-SUBSET.md` §6: one namespace per module); the helpers
stay namespaced (`fx::mul`, `fx::sqrt`, `fx::div`) so they never collide with libc names. The
nine row aliases are *also* exported at global scope by `fx_palette.h`, because every X-macro
field table (`ECS.md` §6, `ALLOY.md` §14) spells them bare. The i64 locals of §3.1 have names
too: `pos2_wide_t = fx<i64,36>`, `den_wide_t = fx<i64,30>`.

`fx_palette.h` has a `FX_PALETTE_REV` constant; it is part of the build fingerprint
(`BUILD.md` §5) — two peers on different palette revs cannot handshake.

---

## 8. Alternatives recorded (so they aren't re-proposed)

| Alternative | Verdict |
|---|---|
| strict float + pinned toolchain (Ore's model by hand) | the **pre-committed Gate 0 fallback**, x86-64 only — never the default; it surrenders cross-ISA and makes every codegen flag a determinism variable |
| one "fixed" type with runtime scale | rejected — deletes the compile-time mixed-scale check |
| uniform `fx<i64,32>` | rejected — cache traffic + SIMD loss for precision needed only at accumulation points |
| radians | rejected — irrational range reduction; turns reduce by masking |
| a vendored fixed-point library as the core | rejected — no library ships our policy; kernels are ported, not the core |
| floats render-side converted per draw | **accepted** (it's the D10 boundary); the extract step converts once per entity per frame |

---

## 9. Rulings (R-1..R-6 closed 2026-08-22/23; R-7..R-9 the Gate 0 rev-2 rulings, 2026-08-25; R-10 2026-08-27 — nothing open)

- **R-1 The rows are DECIDED at the §3 values.** Gate 0 *verifies* them; it does not choose them.
  A convergence failure triggers the pre-committed ladder (§3.2) and, if a row must move, a
  recorded rev-2 edit within the derivation rule — the same "best so far" revision discipline as
  every other lock, not an open question. `lambda_t` is the row most likely to move; its fallback
  is already named (i64 accumulate, narrow once).
- **R-2 SDF texel distance is `i16`, 4 fractional bits** (1/16 texel resolution, ±2,048 texels
  range). `i8` is rejected: contact normals come from the SDF gradient, and 1/16-texel precision is
  what keeps resting contacts from jittering at the `pos_t` quantum. Memory is irrelevant
  (128² × 2 B = 32 KB per chunk; ~60 resident chunks ≈ 2 MB). Storage width, not a palette row.
- **R-3 No `exp`/`log`/`pow` at v0.** Rates are integer quanta/tick; decay is keyed-RNG
  probability from half-life (precomputed per-tick probability in `q_t` at `init()`). Ported from
  FixPointCS only when a consumer exists; the port goes through the §4.4 oracle like every kernel.
- **R-4 The fx helpers are scalar through v0 and Gate 0.** A `fx4<Rep,FRAC>` lane type over
  SSE2/NEON intrinsics lands only if G-05 names a column where it buys the budget; the 32-bit-row
  rule is what keeps that option open and is decided now.
- **R-5 `scalar_t = fx<i32,16>` is a palette row (added at rev 1.1, 2026-08-22)** — unitless
  scalars outside the solver: quanta-path coefficients (`ALLOY.md` §5), animation speed, attribute
  modifiers, utility scores. Range ±32,768, resolution 1.5e-5. `lambda_t` is an alias of it (same
  format, solver-facing name). Mixed ops: `scalar_t × any row → that row`, `scalar_t × scalar_t →
  scalar_t`, enumerated in `fx_palette.h` like the rest. The palette is now nine rows + quanta.

- **R-6 i64 quotients are one `rne_div` on the raw bits, RNE, at rev 1 (ruled 2026-08-23,
  the W1 fx review's defect 11).** `div<R>` narrows 32-bit rows only (§10.1); the two sim
  quotients with an i64 operand — the XPBD denominator (§3.1, `ALLOY.md` §14.4.3,
  `GATE0-BENCH.md` §8) and the magnetism ratio (`ALLOY.md` §14.4) — are spelled
  `rne_div(num * (i64(1) << S), den)` at the site, the widening visible, no new helper. The
  rounding is RNE like every other narrowing (§1, rung 2): a `/` (truncation toward zero) or a
  `floor_div` never appears in sim pseudocode or code — `ALLOY.md`'s earlier `(num << 16) / den`
  and the circuit solve's `floor_div` were the same downward bias §1 bans. The `<< S` is a
  multiply, never a shift of a signed value (`CPP-SUBSET.md` §5). A `div<R>` over an i64 operand
  becomes an op-table row only if a third site appears.
- **R-7 In-kernel λ is an i64 frac-30 local; `lambda_t` is storage (ruled 2026-08-25, Gate 0
  RR-8 — rung 1 REQUIRED).** `dλ = rne_div(num · 2³⁰, den)` (with the deterministic operand
  halving when `|num| ≥ 2³³` — a pure function of the operands), accumulated in i64 across the
  substep's sweep, narrowed once per substep (`lam_narrow`: RNE by 14, out-of-row values counted
  and clamped). The per-constraint form `lambda_t(i32(rne_div(num·2¹⁶, den)))` is banned in sim
  code: it quantises a unit-mass correction to 4 `pos_t` quanta and creeps a resting box 12
  quanta/tick (measured, both bindings; `tests/gate0/results/…/G01_s8_l0.csv`). `ALLOY.md`
  §14.4.3 carries the spelling.
- **R-8 The angular constraint terms stay wide; `omega_t` is retuned to its structural cap
  (ruled 2026-08-25, Gate 0 RR-9).** (a) A body's denominator share `inv_I·(r×n)²` is computed
  and kept as the i64 frac-30 local (`w_ang30`), never narrowed into `invmass_t` — a 4096:1 plank
  with a 1.25 m lever has `inv_I·(r×n)² ≈ 12,000`, outside ±8,192. (b) `inv_I` itself is stored
  in `invmass_t`, which bounds CONTENT: the validator rejects a body whose `inv_I` exceeds the
  row (the smallest legal 4096:1 body is 2.5 m × 0.25 m); a lighter/smaller body is a new ruling,
  not a quiet clamp. (c) `omega_t` becomes `fx<i32,22>` (±512 turn/s): the implicit encoding
  `pθ = θ − ω·h` caps |ω| at `inv_h/2` = 240 turn/s by construction, so range above the cap was
  unreachable; ±512 keeps 2× margin and the 4 reclaimed bits go to resolution. Consequence:
  `vel_t` and `omega_t` are
  distinct C++ types from rev 2 — the op table instantiates the omega triples explicitly.
- **R-9 Wide-local, narrow-storage is the palette-wide principle (ruled 2026-08-25, Gate 0
  RR-8/RR-9/RR-11).** Kernels compute in i64 frac-30 locals; palette rows are what SoA columns
  store, written once per substep at the single rounding point. Third instance: the PBF density
  ratio ρ/ρ₀ stays an i64 frac-30 local through the constraint (it exceeds `q_t`'s ±2 under
  impact — measured, G-03/G-04); `q_t` clamps only the stored metric copy. A future kernel that
  narrows mid-sweep is a bug by this ruling, not a style choice.
- **R-10 The decimal-literal quantizer is integer-only, in `fx.h`, additive (RR-38, ruled
  2026-08-27).** `editor/inspector.cpp`'s fx-field edit widget and `core/cvar.cpp`'s `CVAR_FX_RAW`
  `set <name> <f64>` path both needed to turn a user-typed base-10 literal into a row's raw
  representation — a real gap (`cvar.cpp`'s own `CVAR_FX_RAW` case already documented it: "raw:
  only... needs FX-PALETTE.md's RNE quantizer"). Two options were on the table: a `double`
  intermediate (rejected — `fx.h` carries ZERO float/double tokens today, and one would be the
  first, straight against `CPP-SUBSET.md`'s float ban on this path), or track the literal's exact
  rational (integer numerator/denominator) and round it ONCE via `rne_div`, the same primitive
  every other narrowing path in this header already uses. The second dissolves the dilemma rather
  than picking a horn: `fx.h` stays float-free, the result is bit-exact on every ISA as a free
  strengthening (an integer parse has no rounding-mode/rewrite freedom a compiler's `strtod`-class
  path would), and both callers share one implementation (`fx_parse_decimal_raw(StrView, u8 frac)`
  — a RUNTIME `frac`, since neither caller has a compile-time row type at its call site: Inspector
  walks a runtime `FieldKind`, cvar reads `CvarDesc::frac_bits`; `fx_parse_decimal<R>` is a
  three-line typed convenience wrapper over it for call sites that do have `R`, §10.1). Malformed
  or oversized user text returns a named `Result<T>` error (`ERR_FX_PARSE`/`ERR_FX_RANGE`) — never
  an assert, since untrusted text is `Result`'s job by `CLAUDE.md`'s own error-model split, not a
  caller-bug signal. Additive only: no existing `fx.h` symbol's signature, semantics, or name
  changed. `CMD_SET_FIELD`/`CMD_SET_CVAR` both carry the RAW value on the wire, never a decimal
  string, so quantization happens once, locally, before the command is recorded — the quantizer
  itself never needed to be bit-exact ACROSS PEERS (only within one process, which integer
  arithmetic already guarantees), so this ruling did not have to weigh a cross-ISA determinism
  cost against the float ban either.

## 10. Implementation specification

### 10.1 `fx.h` — the type and the arithmetic (zero includes)

```cpp
template <typename Rep, int FRAC> struct fx {
    Rep v;
    static constexpr int  FRAC_BITS = FRAC;
    static constexpr Rep  ONE       = Rep(1) << FRAC;
    using rep = Rep;
};
// rev 1 instantiates Rep = i32 only (i64 rows exist only for the rung-4 fallback); static_assert(sizeof(Rep) == 4) in every helper until then.

template <typename R> constexpr R fx_raw(typename R::rep bits);                 // the only bit-level constructor (greppable)
template <typename R> constexpr R fx_int(i32 i);                                  // i << FRAC; static_assert/TL_ASSERT |i| < 2^(31-FRAC)
template <typename R> constexpr R fx_lit(i64 num, i64 den);                       // RNE of num/den at the row quantum — for H, G_SUBSTEP, kernel coefficients
template <typename R, typename A> constexpr R to(A x);                            // the only conversion: widen = exact; narrow = rne_shr; TL_ASSERT on range. Release (assert compiled out): the intermediate WRAPS into R::rep by C++20 modular conversion - it does not saturate, so callers on a slim tier range-check (2026-08-24; pinned by fx_review_release_error_values, TESTING.md section 9.1)
```

Operators: `+`, `-`, unary `-` (two's-complement wrap, explicit by policy), `== != < <= > >=`
— all defined **only between identical formats** (a template on one `R`). Free functions per
format: `abs`, `min`, `max`, `clamp`, `sign`, `sat_add`, `sat_sub`, `sat_neg`, `is_zero`.

**The rounding primitive (one function, used everywhere):**

```cpp
constexpr i64 rne_shr(i64 x, int s) {          // round-to-nearest-even arithmetic shift right, s in [0, 62]
    if (s == 0) return x;
    const i64 q    = x >> s;                    // arithmetic shift (C++20: defined)
    const i64 r    = x & ((i64(1) << s) - 1);   // remainder in [0, 2^s); never `q << s` - a negative left shift is UB (CPP-SUBSET §5)
    const i64 half = i64(1) << (s - 1);
    return (r > half || (r == half && (q & 1))) ? q + 1 : q;
}
```

**Multiply and divide (named helpers, result format first):**

```cpp
template <typename R, typename A, typename B> R mul(A a, B b) {
    static_assert(fx_op_allowed<R, A, B>::value, "product not in the mixed-op table");
    constexpr int S = A::FRAC_BITS + B::FRAC_BITS - R::FRAC_BITS;   // static_assert(0 <= S && S <= 62)
    const i64 p = i64(a.v) * i64(b.v);                              // exact: 32×32 → 64
    const i64 q = rne_shr(p, S);
    TL_ASSERT(q >= INT32_MIN && q <= INT32_MAX);                     // dev: range; release: see sat_mul
    return fx_raw<R>(i32(q));
}
template <typename R, typename A, typename B> R sat_mul(A a, B b);  // same, clamps to i32 instead of asserting — quanta paths
template <typename R, typename A, typename B> R div(A a, B b) {
    static_assert(fx_op_allowed<R, A, B>::value);
    constexpr int S = R::FRAC_BITS + B::FRAC_BITS - A::FRAC_BITS;   // static_assert(0 <= S && S <= 31)
    TL_ASSERT(b.v != 0);                                             // callers guard; release returns saturated sign(a)*INT32_MAX
    const i64 n = i64(a.v) * (i64(1) << S);                          // a multiply, never a shift of a signed value
    i64 q = n / b.v, r = n % b.v;                                    // C++ truncation toward zero (the body is `rne_div(n, b.v)`, the second rounding primitive)
    // RNE on the exact rational: compare 2|r| with |b|; ties to even
    const i64 r2 = (r < 0 ? -r : r) * 2, bb = (b.v < 0 ? -i64(b.v) : i64(b.v));
    if (r2 > bb || (r2 == bb && (q & 1))) q += ((n < 0) != (b.v < 0)) ? -1 : 1;
    TL_ASSERT(q >= INT32_MIN && q <= INT32_MAX);
    return fx_raw<R>(i32(q));
}
template <typename R, typename A> R mul_int(A a, i32 k);            // exact a.v * k, then rescaled from A's point to R's (pos_t x 480 -> vel_t widens 2 bits exactly; angle_t x 480 -> omega_t narrows 8 with RNE); listed in the op table as fx_op_allowed<R, A, i32>
template <typename A> i64 mul_wide(A a, A b);                        // raw i64 product, no rounding — for squared lengths (fx<i64,36> local)
```

`fx_op_allowed<R,A,B>` is a closed `constexpr bool` table in `fx_palette.h` (one specialization per
distinct *format* triple of §3.1 plus the `scalar_t` rules - rows sharing a format are one type,
§1); the primary template is `false`, so an unlisted product fails to compile with the message
above. `B = i32` marks the plain-integer factor of `mul_int`. **Every call site names `R`
explicitly**; there is no deduction of the result format. `dot<R>`/`cross<R>` consult the same table.

Wrapping/saturating integer helpers for quanta paths live in the same header: `wrap_add/sub/mul`
(unsigned-cast two's-complement), `sat_add/sub/mul` for `i32`/`i64`, `mulhi64`.

**Decimal literal parsing (RR-38, R-10, added 2026-08-27) — integer-only, no float/double token:**

```cpp
Result<i32> fx_parse_decimal_raw(StrView s, u8 frac);   // the primitive: runtime frac (both real
                                                          // callers only know FRAC at runtime)
template <typename R> Result<R> fx_parse_decimal(StrView s);  // typed wrapper, R::FRAC_BITS
```
Grammar `[+-]?[0-9]*(\.[0-9]*)?`, at least one digit, nothing trailing. Tracks the literal as an
exact integer numerator/denominator (accumulated digit-by-digit, `u64`, overflow-checked at every
step — never a `double`) and rounds it once via `rne_div`, the same primitive `div<R>` above
already uses. `ERR_FX_PARSE`: malformed text. `ERR_FX_RANGE`: more than 18 fractional digits (the
denominator would not fit `u64`), the numerator overflows, or the rounded result does not fit
`i32` — every one of these is caught here so untrusted text never reaches an internal `TL_ASSERT`
(`Result` is the error-model door for text; asserts are for a caller's own logic bug,
`CLAUDE.md`). Pinned known-answer vectors, including the criterion's own `1.5` → `pos_t` raw
`0x60000` and explicit RNE tie cases, are `fx_decimal.test.cpp` (§10.5).

### 10.2 `fx_palette.h` — rows, constants, the op table

```cpp
using pos_t = fx<i32,18>;  using vel_t = fx<i32,20>;  using invmass_t = fx<i32,18>;
using stiff_t = fx<i32,30>; using q_t = fx<i32,30>;  using angle_t = fx<i32,30>;
using omega_t = fx<i32,22>; using dt_t = fx<i32,30>; using scalar_t = fx<i32,16>; using lambda_t = scalar_t;
constexpr u32 FX_PALETTE_REV = 1;

constexpr pos_t   TEXEL          = fx_raw<pos_t>(1 << 14);                 // 1/16 m
constexpr i32     INV_H          = 480;                                     // plain integer
constexpr dt_t    H              = fx_lit<dt_t>(1, 480);                    // raw 2236962 (RNE of 2^30/480 = 2236962.13)
constexpr vel_t   G_SUBSTEP      = fx_lit<vel_t>(981 /*9.81*/, 100 * 480);  // raw 21430 (RNE of 9.81/480 · 2^20 = 21430.27; the compiler corrected an earlier 21432)
constexpr pos_t   WORLD_HALF     = fx_int<pos_t>(4096);
constexpr vel_t   V_MAX_WORLD    = fx_int<vel_t>(512);
constexpr i32     MASS_RATIO_CLAMP = 4096;
constexpr angle_t TURN           = fx_raw<angle_t>(1 << 30);               // 1.0 turn; masking with (TURN.v - 1) wraps
```

Every derivation (range × margin → integer bits → FRAC) is a `static_assert` next to the row
(e.g. `static_assert((i64(1) << (31 - 18)) >= 2 * 4096)` for `pos_t`). The §3.1 mixed-op table is
the list of `fx_op_allowed` specializations, in the same order, each with a comment naming its
use site.

### 10.3 `det_math.h` — kernels (FixPointCS ports, attributed)

```cpp
u32  isqrt32(u32 x);  u64 isqrt64(u64 x);                       // exact floor sqrt, bit-by-bit restoring (31/63 iterations, branch-free form)
template <typename R, typename A> R sqrt(A x);                   // S = 2·R::FRAC − A::FRAC (static_assert 0 ≤ S ≤ 30); y = isqrt64(u64(x.v) << S);
                                                                  // nearest: if (u64(x.v)<<S) − y·y > y then y+1; x.v < 0 → TL_ASSERT, returns 0
template <typename R, typename A> R rsqrt(A x);                  // div<R>(fx_int<R>(1), sqrt<R>(x)); never an estimate
void   sincos(angle_t a, q_t* s, q_t* c);                        // see below
q_t    sin(angle_t a);  q_t cos(angle_t a);
angle_t atan2(pos_t y, pos_t x);  angle_t atan2q(q_t y, q_t x);
template <typename R> R lerp(R a, R b, q_t t);                   // a + mul<R>(to<q_t>… no: a + mul<R>(b − a, t) — (b−a) is R, t is q_t, listed in the op table
```

**`sincos` in turns.** `z = i32(u32(a.v) << 2)` is the angle in quarter turns, Q30, mod 2³² —
the integer turns fall off the top, which *is* the range reduction (exact); it is also exactly
the reference's `UnitSin` input domain, so the port is the reference's own mirroring (quadrants
1–2 → `2 − z`, wrapping) and its quarter-wave polynomial (`FixedUtil::SinPoly4`, coefficients
verbatim, committed by `tools/fxcheck/oracle.py emit-coeffs` and re-checked by `check-coeffs`),
`sin = P(z²)·z`, `cos = sin(z + quarter)`. Two deviations, both measured: every Horner step
rounds with `rne_shr` rather than the reference's truncating `Qmul30` (odd symmetry becomes
bit-exact, the downward bias goes), and the result is clamped to `±ONE` because the reference
overshoots by one ulp at `±1` (`P(1) = ONE + 1` with the verbatim coefficients). Output `q_t`;
`sin(0)` is exactly 0, `sin(quarter)` exactly `q_t::ONE`, `|sin| ≤ ONE` always.

**`atan2`.** Octant reduction on `(|y|, |x|)` (as i64, so `INT32_MIN` negates) to a ratio
`z = min/max` via one exact `rne_div` (the reference uses a reciprocal polynomial here; we do
not), the FixPointCS `AtanPoly5Lut8` polynomial on `z ∈ [0, 1]` (eight segments by `z >> 27`
plus the `atan(1)` row; coefficients pre-scaled by `1/(2π)` at 60 digits by
`tools/fxcheck/oracle.py`, so the result is in turns directly — no runtime radian conversion),
then octant unfold (`quarter − r`, `half − r`, negate for `y < 0`), result in `[−½, ½]` —
closed at both ends (`y < 0` with `|y|/|x|` below ~2⁻³¹ rounds to `−½`, the same angle as `+½`
mod one turn; compare angles masked, never `== HALF_TURN`). `atan2(0, 0)` returns 0 with a
`TL_ASSERT`. Axes and diagonals are exact.

Vectors: `template <typename T> struct vec2 { T x, y; }` with `+`/`-` per format, `dot<R>`,
`cross<R>` (both `mul_wide` sums in `i64`, one `rne_shr`), `len2_wide(vec2<pos_t>) → i64` (Q36),
`len<pos_t>` (= `sqrt<pos_t>` of the wide value: `isqrt64(len2) >> 18`... implemented as
`fx_raw<pos_t>(i32(isqrt64(u64(len2_raw))))` since `sqrt(Q36) = Q18`), `normalize(vec2<pos_t>) →
vec2<q_t>` (two `div<q_t>` by `len`; zero → `(0,0)` + assert), `rotate(vec2<pos_t>, q_t s, q_t c)`
(four `mul<pos_t>`, two adds).

### 10.4 `fx_float.h` (render/editor/tools; banned tokens in sim TUs)

`to_f32(fx) = float(x.v) * (1.0f / (1 << FRAC))` (the reciprocal is exact — a power of two);
`to_f64` likewise; `from_f32_quantized<R>(float f)` / `from_f64_quantized<R>` = RNE of
`f * 2^FRAC` to an integer, range-clamped, with a `TL_ASSERT` on NaN/inf. **No `<math.h>`**:
`CPP-SUBSET.md` §1 allows it only in `render/`/`editor/`/`platform/` and the include gate
enforces that for `foundation/` too, so the rounding is the libm-free identity
`(|s| + 2^23) − 2^23` applied per sign (f64: `2^52`), exact under the default rounding mode -
which is guaranteed because `-ffast-math` is banned everywhere (`CPP-SUBSET.md` §7). Not the
sign-free `1.5·2^23` magic: it is exact only for `|s| ≤ 2^22` (above, `s + 1.5·2^23 ≥ 2^24`
where the spacing is 2) and the W1 fx review measured every odd integer in `[2^22, 2^23)`
coming back even.

### 10.5 Tests (`tests/foundation/`)

| File | What |
|---|---|
| `fx_rne.test.cpp` | `rne_shr` tie table (±half, even/odd) for s = 1..62; exhaustive on 16-bit inputs |
| `fx_mul_div.test.cpp` | exhaustive small-operand `mul<R>`/`div<R>` (8-bit × 16-bit raw, every 16-bit-shift entry; 9-bit × 9-bit quotients) and property (1M seeded pairs, the in-range count asserted **per row**, never aggregated) for every distinct format triple of the op table, both vs an **exact integer** rational reference written sign-magnitude style (a `double` loses bits above 2^53; the reference does not) — bit-equal, not "≤ ½ ulp"; `div` ties (an `INT32_MIN` divisor is the only way to make one); `sat_*`/`wrap_*` clamps and wraps at every boundary; `mulhi64` vs the compiler's 128-bit product. `div` by zero → fatal-expected **lands when the runner lane ships `TL_TEST_EXPECT_FATAL`** (`TODO.md`) |
| `fx_palette.test.cpp` | every `static_assert` derivation re-checked at runtime; `H`, `G_SUBSTEP`, `TEXEL` raw values equal the documented constants |
| `det_sqrt.test.cpp` | `isqrt32` exhaustive (2³² in ~1 min, `slow` tag) and every perfect square ± 1 at both widths; `sqrt<R>` nearest property as an exact integer inequality (no reference needed) over 2²⁰ seeded inputs per shift (0, 18, 30); `rsqrt` = `div` of `sqrt` |
| `det_trig.test.cpp` | exact points (0, ¼, ½, ¾, ±turns, `INT32_MIN`); every symmetry bit-exact over 2²⁰ seeded angles (fast) and all 2³⁰ (`slow`); `sin²+cos²` within `FX_SIN2COS2_MAX_ERR_ULP`; monotone per quadrant; vs the correctly-rounded mpmath table at 4096 turn fractions within `FX_SIN_MAX_ERR_ULP`. The true-error sweep over all 2³⁰ inputs is `tools/fxcheck` (nightly) — a 2³⁰-entry table cannot be committed; its measured bound is the constant the tests assert |
| `det_atan2.test.cpp` | axes/diagonals exact at every magnitude incl. `INT32_MIN`; mirror/swap symmetries bit-exact over 2²⁰ seeded pairs + octant neighbourhoods; `atan2(sin a, cos a) == a` within `FX_ATAN2_ROUNDTRIP_MAX_ERR_ULP` for all 2¹⁶ grid angles; vs the mpmath table at 4096 ratios. The 2²⁴-sample true-error sweep is `tools/fxcheck` |
| `det_vec.test.cpp` | `dot`/`cross` vs the exact product (seeded), `len` 3-4-5 exact, `normalize` within the input-quantisation bound (`2³⁰/|d|` + 4 ulp — a short vector cannot normalise better than its own quantum), `rotate` by quarter turns exact and round-trip within 3 quanta, `lerp` ties. Zero-vector assert: fatal-expected, pending the runner (`TODO.md`) |
| `fx_trace.test.cpp` (+ `fx_crossisa` driver job) | two ~1M-value traces, each folded into one FNV-1a 64 that is **pinned in the test**: trace A (`mul/div/mul_int/to/sqrt/sincos/atan2/normalize/rotate`) and trace B (the rest: `to` widening, `sqrt` at every shift, `rsqrt`, `lerp`, `dot/cross/len`, `mul_int` narrowing, `sat_mul`, the sat/abs/min/max/clamp tier, `mulhi64`/`sat_mul(i64)`, `isqrt32/64`, `div` at unit quotients - the W1 fx review's coverage gap); every tier, compiler and ISA must reproduce both (PR lane: clang-cl + ubuntu clang; nightly: the Pi). The driver job hashes the same traces through `tl_driver` once the runner+driver lane lands |
| `fx_review.test.cpp` | the W1 fx adversarial review's probes, kept as regressions: the `fx_rint` band `[2^22, 2^23)`, the closed `atan2` range, `sat_mul<R>` at the clamp, the in-contract `INT32_MIN` / 2⁶² / 2⁶³ edge matrix, and (netcode/ship only) every documented release-tier error value - `div` by zero, `sqrt` of a negative, `rsqrt(0)`, `atan2(0,0)`, `normalize(0)`, NaN/±inf quantisation |
| `fx_float.test.cpp` | `to_f32` exact below 2²⁴ raw, `to_f64` exact for every row; `from_f32/f64_quantized` RNE ties at the row quantum, clamps at the row range, round trips |
| `fx_decimal.test.cpp` (RR-38, R-10) | pinned known-answer vectors, not property/fuzz (no float path to fuzz against): `1.5` → `pos_t` raw `0x60000` (the ruling's own criterion); integer/negative/leading-`+`/bare-`.`/trailing-`.` forms; malformed text (empty, lone sign, lone `.`, double dot, trailing garbage, embedded letters) → `ERR_FX_PARSE`, never a crash or a silent partial parse; out-of-range magnitude, >18 fractional digits, and numerator overflow → `ERR_FX_RANGE`, never a `TL_FATAL`; explicit RNE ties-to-even at `fx<i32,1>`'s exact midpoints (`0.25`→0, `0.75`→2, `1.25`→2) plus a 41-point sweep at `fx<i32,3>` cross-checked against `fx_test_util.h`'s independent `ref_rne_div`; `fx_parse_decimal_raw` verified to agree with the typed `fx_parse_decimal<R>` wrapper at every palette FRAC |

`tools/fxcheck/` (exempt from the subset): `fxcheck.cpp` (C++, `long double` reference) is the
exhaustive + differential layers — `cmake -S tools -B out/tools && cmake --build out/tools &&
out/tools/fxcheck [--quick]`, nightly in full (~4 min), `--quick` anywhere; `oracle.py` (mpmath)
emits/checks the coefficients, emits `tests/foundation/det_ref_tables.inc`, and `verify`s the
worst cases `fxcheck` writes to `worst.tsv`.

### 10.6 Done criteria

`fx.h`, `fx_palette.h`, `det_math.h`, `fx_float.h` compile under every tier with the sim flag set
(`-nostdinc++`, `-fno-builtin`); the `tl_foundation_det` symbol audit is empty; every test above is
green on PC, `slow` ones nightly; the cross-ISA trace matches on the hosted arm64 legs
(re-ruled 2026-08-25; was "on the Pi"). Then Gate 0.

*Rev 1.1 — 2026-08-22. Rev 2 is written from Gate 0's CSVs.*
