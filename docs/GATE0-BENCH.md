# Gate 0 — the fixed-point XPBD + PBF convergence and cost bench (tidelock, rev 1)

> **Status:** spec rev 1, 2026-08-22. **Thresholds are FROZEN** (confirmed at the 2026-08-21 veto
> pass; `PIVOT-DESIGN.md` §10). The answer is not negotiated after results exist. This doc is the
> executable form of that spec: what to build, what to run, what to record, what each result means.
> **This is THE pivot gate.** It is the next milestone and it hangs the cross-ISA claim.

---

## 0. What it decides

Whether fixed-point arithmetic (`FX-PALETTE.md` §3 rows) can hold an XPBD rigid + PBF liquid solve
stable and within budget. Three outcomes, pre-committed:

| Outcome | Consequence |
|---|---|
| G-01..G-06 pass | `FX-PALETTE.md` rev 2 stamped DECIDED with measured values; `fx_palette.h` written; foundation week starts |
| convergence failure (G-01..G-04) | climb the precision ladder (`FX-PALETTE.md` §3.2) rung by rung, re-run; palette rows may move *within the derivation rule*; thresholds do not move |
| ladder exhausted | **fallback fires:** pinned-toolchain float, x86-64 only, cross-ISA (the Pi) consciously written off, PIVOT rev-bumped. Decided then, not drifted into |
| G-05 PC half fails (> 8 ms at 20k) | pivot-level — same fallback ladder |
| G-05 Pi-only miss | **not pivot-level** — redraws minimum spec (Pi becomes stretch peer); never triggers the float fallback, since abandoning fixed point to rescue the Pi would surrender the property that exists *for* the Pi |
| G-06 fails (any divergence) | the highest-information result the bench can produce: it is UB. Hunt with UBSan/ASan before anything else |

---

## 1. What gets built (DECIDED)

**No engine.** `tests/gate0/` is a headless exe linking only:

- `src/foundation/fx.h`, `fx_palette.h`, `det_math.h` — **production headers**, written for real
  (they are the foundation's first deliverable; the bench is their first consumer).
- a minimal `Array<T>` + arena (the foundation's real ones if ready; a 100-line local stand-in is
  acceptable since the bench never hashes them — the solver arrays are what's hashed).
- a keyed RNG (`DETERMINISM.md` §3) for scene setup.
- a **disposable** solver: the Alloy §8.1 substep loop reduced to what the scenarios need —
  gravity, rigid bodies (box SDF, θ/ω, inertia), distance + contact constraints with position-level
  friction, PBF density constraints with the q-normalized kernel, colored Gauss-Seidel order
  (single-threaded: the bench measures arithmetic, not threading), sleeping OFF.
- CSV writer (stb_sprintf; the bench is allowed io — it is not sim code).
- the **FLOAT-SHADOW** config: the solver compiled once more over `double` typedefs in a dev-only
  build, run side by side on identical scenes; per constraint, per pass, log the max |fx − double|
  so a failure reads "row X, step Y" instead of "it jitters". Never authoritative.

Written in the C++ subset so that if the solver kernel comes out clean it can be *moved* into
`src/sim/` rather than rewritten — but promotion is not the goal; correctness of the rows is.

---

## 2. Scenarios and frozen thresholds (DECIDED)

Texel = 1/16 m. All positions are `pos_t`. Energy is computed in integer/fx: Σ(½ m v²) + Σ m g h
using widened i64 sums of fx products, reported as raw i64 so the envelope comparison is exact.

| ID | Scenario | Pass | Investigate | Fail |
|---|---|---|---|---|
| **G-01** | 10-box stack at rest (1 m boxes, unit mass), 10k ticks | p95 per-tick position jitter < 0.1 texel, zero sink, zero pop | < 0.5 texel | creep / pop / oscillation |
| **G-02** | feather on boulder at full 4096:1 (after the clamp), dropped from 2 m then resting; boulder dropped on feather at `V_MAX` | sustained penetration < 1 texel, no tunneling at `V_MAX` | < 2 texels | tunneling, or any saturation hit in `invmass_t`/`lambda_t` |
| **G-03** | 5k-particle PBF column (2 m wide, ~1.2 m tall) settling in a sealed box | p95 rest-density error < 2% after settle, stays settled over 5k further ticks | < 5% | undamped "boiling" (kinetic energy not monotone-decreasing after settle) |
| **G-04** | sealed mixed scene (the G-01 stack + 2k particles + 20 free boxes + 10 ropes of 8 distance constraints), 1e6 ticks | monotone non-increasing energy envelope (windowed max over 1k ticks) | slow bounded drift (envelope bounded within 1% of initial over the run) | energy growth |
| **G-05** | cost sweep: 10k / 20k / 50k particles + 2k bodies, 8 substeps, single thread | 20k ≤ 4 ms PC **and** ≤ 12 ms Pi 4 | ≤ 8 ms PC / > 12 ms Pi | > 8 ms PC at 20k |
| **G-06** | all of the above run twice in one process + PC-vs-Pi cross-compiled | 100% identical per-tick hash traces | — | any divergence (= UB) |

**Substep sweep (ratified):** G-01..G-05 run at substeps **4 / 8 / 16**, not only 8. Substep count
interacts with quantization non-obviously (smaller h → smaller corrections, closer to the `pos_t`
quantum), so it is measured. The sweep may move the substep constant (a constant change, not a
palette change).

**Ladder variants:** rung 3 (residual carry) is a standing build variant of the same bench; rung 4
(wide state, narrow math) is implemented only if rungs 1–3 leave a G-01/G-04 failure.

---

## 3. Which scenario gates which row

| Row | Gated by | What a failure looks like |
|---|---|---|
| `pos_t` (18 frac) | G-01 jitter, G-04 drift | corrections below 3.8 µm round to zero → creep; or accumulate → pop |
| `vel_t` (20 frac) | G-02 at `V_MAX`, G-04 | saturation on impact transients; drift from velocity quantization |
| `invmass_t` (18 frac), the clamp | G-02 | denominator saturation; feather sinks |
| `stiff_t` (30 frac) | G-04 ropes, G-01 contacts (α̃ ≈ 0) | stiff constraints go soft (α̃ rounds up) or explode |
| `q_t` (30 frac), kernel strategy | G-03 | density error; kernel precision dependent on scale |
| `lambda_t` (16 frac / i64 accumulate) | G-01, G-02 | accumulated λ loses the small corrections |
| `angle_t`/`omega_t` | G-01 (boxes must not rotate at rest), G-04 | rotational creep |
| `dt_t` (h rounding) | G-04 | systematic energy bias from a rounded h — the shadow build isolates it |

---

## 4. Measurement protocol (DECIDED)

- **Machines:** the dev PC (x86-64 Windows, clang-cl) and the Pi 4 (aarch64, cross-compiled from
  the PC with the same clang — `BUILD.md` §2). Steam Deck optional for G-06 (third data point).
- **Build tiers:** `netcode`-equivalent flags (`-O2`, no sanitizers) for timing; a second run of
  G-01..G-04 under UBSan+ASan for G-06 evidence (timing ignored).
- **Per-tick CSV** (buffered, written after the run): `tick, substeps, scenario, jitter_p95_texel,
  max_penetration_texel, density_err_p95, energy_i64, solve_us, hash_lo64`. Hash = rapidhash over the
  solver's used columns `[base, used)` each tick (`DETERMINISM.md` §4).
- **Shadow CSV** (dev config only): `tick, substep, pass, constraint_kind, max_abs_err_fx_vs_double`.
- **Timing:** p50/p95/p99 of `solve_us` over the last 80% of ticks (warm-up excluded), reported per
  machine per particle count. The Pi number is the **binding** one for min-spec; the PC number is
  the pivot-level one.
- **Pass/fail is computed by the bench itself** and printed as a verdict line per scenario; the
  CSVs are committed under `tests/gate0/results/<date>-<machine>/` so the rev-2 palette cites
  real files.

---

## 5. What "adjust the palette" may and may not touch

May: a row's FRAC within the derivation rule (range × margin must still fit); the substep count;
rung selection. May not: the thresholds; the world constants (§2 of `FX-PALETTE.md` — a world
constant change is a separate ruling); the 32-bit-vectorizable rule (rung 4 is the only sanctioned
64-bit storage path, and it keeps 32-bit math).

---

## 6. Deliverables

1. `src/foundation/fx.h`, `fx_palette.h`, `det_math.h` at rev 1 of the palette, with the §4.4
   oracle tests of `FX-PALETTE.md` passing for `sqrt`/`sin`/`cos`.
2. `tests/gate0/` exe + CSVs + the verdict table filled in, committed.
3. `FX-PALETTE.md` rev 2 (rows DECIDED or the fallback recorded), and `PIVOT-DESIGN.md` §3.1b /
   §12 updated in the same commit.
4. A `LESSONS.md` entry for every rung climbed.

---

## 7. Rulings (closed 2026-08-22 — nothing open)

- **R-1 Box–box contacts are corner-vs-SDF deepest-point** (Alloy's real method, `ALLOY.md` §2.1),
  not SAT. The bench must exercise the precision of the contact path the sim will ship, or G-01's
  verdict is about a solver nobody builds.
- **R-2 G-05 includes the broadphase rebuild** (tiered hash + radix sort) in the budget, reported as
  separate `solve_us` and `broadphase_us` columns so a miss is attributable. The threshold applies
  to the sum.

*Rev 1 — 2026-08-22.*
