# Gate 0 results — 2026-08-25, dev PC (x86-64 Windows 11, clang-cl 22, `netcode-win` = -O2)

Bench: `tl_gate0` on branch `w2-gate0` (`git log` of this directory), `docs/GATE0-BENCH.md` §8.
Every verdict below is the bench's own stdout (`verdicts_*.txt`); every CSV is the per-tick trace
of the run that produced it (`docs/GATE0-BENCH.md` §4 columns + R-2's `broadphase_us`;
`--csv-every 10` for G-03, `100` for G-04 — one row per 10/100 ticks; the run-twice hash compare
ran on every tick in memory). `run_twice=identical` = the two in-process runs' hash traces
matched on every tick. `shadow_*.csv` are the FLOAT-SHADOW error traces (dev tier, never
authoritative). A scenario whose carrier leaves the sealed box is stopped and is a tunneling
FAIL whatever it measures; its CSV is the truncated trace. The Pi leg (G-06 cross-ISA, G-05 Pi
half) is BLOCKED on RR-1 (`TODO.md`).

Bench constants the spec leaves open (`tests/gate0/scenes.cpp` / `solver.cpp`; none is a
threshold or a row): density compliance α = 1.3e-6 (α̃ = α/h² = 0.3 at 480 Hz, `--alpha 1302`),
μ = 0.5, restitution 0, contact margin 2 texels + per-tick travel, particle lattice at CANON's
2 texels, the feather = a 2.5 m × 0.25 m plank at 1/4096 kg (the smallest 4096:1 body whose
inverse inertia fits `invmass_t`), G-03 settle tick 2000, G-04's free boxes a quarter metre over
the liquid / on the floor, G-05's bodies one floor row in a 600 m box.

## The §8.5 row — substeps = 8, PC

| Scenario | Verdict | Metric (the verdict line) | CSV | Reading |
|---|---|---|---|---|
| G-01 | **PASS** | jitter p95 0.0000 texel, top drift 0.0000, pop 0, 10,000 ticks, run-twice identical | `G01_s8_l1.csv` | also PASS at 4 and 16 (`G01_s4_l1.csv`, `G01_s16_l1.csv`); `--ladder 3` identical (`G01_s8_l3.csv`); `--ladder 0` jitter 0.0009 texel (`G01_s8_l0.csv`) — the per-constraint `lambda_t` creep, RR-8 |
| G-02a | **PASS** | sustained penetration 0.0003 texel, tunneling 0, λ saturations 0 | `G02_s8_l1.csv` (rows `G02a`) | plank dropped 2 m onto the resting boulder |
| G-02b | **FAIL** | tunneling 1 (the plank leaves the box at tick 2), ω clamps 5, V_MAX clamps 7 | `G02_s8_l1.csv` (rows `G02b`) | the boulder never tunnels; the 4096:1 plank is ejected at V_MAX and spun past the ¼-turn/substep cap — RR-9, RR-15 |
| G-03 (5k = the 39 m column) | **FAIL** | tunneling 1 at tick 34, both runs identical | `G03_s8_l1.csv` | the double shadow ejects the same particle at tick 32: solver design (RR-10) + the spec's geometry (RR-14) |
| G-03 (1k = 7.8 m column) | **FAIL** (by the KE rule) | density error p95 **2.12 %** after settle (INVESTIGATE band), 4 KE-window increases in 5,000 ticks; total energy flat to 0.014 % | `G03_1000/G03_s8_l1.csv` | the column that holds; the "boiling" is a 0.014 % breathing of the compliant column, reported as the rule reads it |
| G-04 (20,000 ticks) | **INVESTIGATE** | 4 envelope increases, all inside 1 % of initial (energy 3.24e10 → 6.80e9 settling, then flat to 0.04 %); run-twice identical | `G04_s8_l1.csv` | 1e6 ticks = 16 h per run here; `--ladder 3` PASS at 3,000 ticks (`G04_s8_l3_3k`) |
| G-05 | **FAIL** | tunneling at tick 139 (10k), 83 (20k), 83 (50k): the liquid base is crushed under its own column | `G05_s8_l1.csv` | cost over the ticks that ran (solve + broadphase, mean / max): **10k 341 / 409 ms**, **20k 657 / 757 ms**, **50k 1,486 / 1,712 ms**; 141–194 ns per pair evaluation, ~155 pair evaluations per particle per tick — RR-13. Pi: BLOCKED (RR-1) |
| G-06 | **PASS** (PC) | every scenario's two in-process runs bit-identical (G-01, G-02a/b, G-03, G-04, G-05) | `G06_s8_l1.csv` | Pi: BLOCKED (RR-1); the UBSan/ASan evidence run needs the Linux lane (`TODO.md` §8.5 remainder) |

## The substep sweep (G-03 at 1k, G-04 at 3,000 ticks)

| Substeps | G-03 (1k) | G-04 |
|---|---|---|
| 4 | FAIL, tunneling at tick 7 (α̃ = 0.075: the compliance scales with h²) — `G03_1000/G03_s4_l1.csv` | FAIL, tunneling at tick 7 — `G04_s4_l1.csv` |
| 8 | 2.12 % density error, KE rule — `G03_1000/G03_s8_l1.csv` | INVESTIGATE (above) |
| 16 | **1.56 %** density error, 1 KE-window increase — `G03_1000/G03_s16_l1.csv` | FAIL, tunneling at tick 876 (ω clamps 9) — `G04_s16_l1.csv` |

The sweep says: the liquid's stability is a compliance question, not a substep question (RR-10);
the rigid scenarios do not care (G-01/G-02 identical verdicts at 4/8/16).

## What moved and what did not

- No threshold, world constant or palette row was changed. The one doc correction is
  `GATE0-BENCH.md` §8.3's "16 extra bits" → 12 (`ALLOY.md` §14.4.3 is the home).
- The ladder: rung 1 (i64 λ across the sweep) is the bench's default and is REQUIRED (RR-8);
  rung 2 is the helpers' RNE; rung 3 (residual carry) changes nothing measurable on G-01, G-03
  (2.17 % vs 2.12 %) or G-04; rung 4 was not needed for any rigid-body outcome.
- Every remaining FAIL is the liquid's (RR-10/RR-11/RR-14), the scenario's (RR-15) or the cost
  budget's (RR-13), each reproduced or explained by the double shadow — the palette rows are not
  what those verdicts are about.
