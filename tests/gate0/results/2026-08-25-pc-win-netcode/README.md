# Gate 0 results — 2026-08-25, dev PC (x86-64 Windows 11, clang-cl 22, `netcode-win` = -O2)

Bench: `tl_gate0` at commit `w2-gate0` (see `git log`), `docs/GATE0-BENCH.md` §8. Every verdict
line below is the bench's own output; every CSV is the per-tick trace of the run that produced it
(`--csv-every 10` for G-03, `100` for G-04: one row per 10/100 ticks; the hash compare ran on every
tick in memory). `run_twice=identical` = the two in-process runs' hash traces matched on every
tick. The FLOAT-SHADOW CSVs (`shadow_*.csv`, dev tier, never authoritative) give the max |fx −
double| per pass per constraint kind. The Pi leg (G-06 cross-ISA, G-05 Pi half) is BLOCKED on
RR-1 (`TODO.md`); `pi=BLOCKED(RR-1)` on the verdict lines is that.

Bench constants the spec leaves open (all in `tests/gate0/scenes.cpp` / `solver.cpp`, none a
threshold or a row): density compliance α = 1.3e-6 (α̃ = 0.3 at 480 Hz; `--alpha 1302`), μ = 0.5,
restitution 0, contact margin 2 texels + per-tick travel, particle lattice at CANON's 2 texels,
the feather = a 2.5 m × 0.25 m plank at 1/4096 kg (the smallest 4096:1 body whose inverse
inertia fits `invmass_t`), G-03 settle tick 2000, free boxes a quarter metre over the liquid.

## Verdicts at substeps = 8 (the §8.5 row)

| Scenario | Verdict | Metric | Notes |
|---|---|---|---|
| G-01 | **PASS** | jitter p95 0.0000 texel, top drift 0.0000 texel, no pop, 10,000 ticks | also PASS at 4 and 16 substeps; ladder 0 (per-constraint `lambda_t`): jitter 0.0009 texel, the single-box creep of RR-8 |
| G-02a | **PASS** | sustained penetration 0.0003 texel, no tunneling, 0 λ saturations | feather (plank) dropped 2 m onto the resting boulder |
| G-02b | **FAIL** | tunneling = 1 (the plank leaves the box within 2 ticks), ω clamps 5, V_MAX clamps 7 | the boulder never tunnels; the 4096:1 plank is ejected at V_MAX and spun past the ¼-turn/substep cap — RR-9, RR-15 |
| G-03 (5k, 39 m column) | **FAIL** | tunneling at tick 34 (a mid-column particle launched at 100 m/s), both runs identical | the double shadow ejects the same particle 2 ticks EARLIER: solver design (RR-10), and the spec's geometry (RR-14) |
| G-03 (1k, 7.8 m column) | see `G03_1000/` | density error p95 after settle | the column that holds at α̃ = 0.3 |
| G-04 (20,000 ticks) | see `verdicts_G04_s8_20k.txt` | energy envelope | 1e6 ticks = 16 h per run on this PC; 20k twice is what this date carries |
| G-05 | see `verdicts_G05_s8.txt` | p95 solve+broadphase µs at 10k/20k/50k | 141 ns per pair evaluation measured at 10k — RR-13 |
| G-06 | see `verdicts_G06.txt` | two-run hash compare per scenario | PC only; Pi BLOCKED (RR-1) |

## What moved and what did not

- No threshold, world constant or palette row was changed. The one doc correction is
  `GATE0-BENCH.md` §8.3's "16 extra bits" → 12 (`ALLOY.md` §14.4.3 is the home).
- The ladder: rung 1 (i64 λ across the sweep) is the bench's default and is REQUIRED (RR-8);
  rung 2 is the helpers' RNE; rung 3 (`--ladder 3`, residual carry) changes nothing measurable on
  G-01 (`G01_s8_l3.csv`); rung 4 was not needed for any G-01/G-02 outcome.
- The failures that remain are the liquid's (RR-10/RR-11/RR-14) and the cost budget's (RR-13),
  each reproduced by the double shadow — the palette rows are not what those verdicts are about.

## Files

`G0x_s<substeps>_l<ladder>.csv` per-tick traces (`docs/GATE0-BENCH.md` §4 columns + R-2's
`broadphase_us`); `shadow_*.csv` the FLOAT-SHADOW error traces; `verdicts_*.txt` the bench's
stdout; `G03_1000/` the 1,000-particle column.
