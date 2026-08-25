# Gate 0 results — 2026-08-25, dev PC (x86-64 Windows 11, clang-cl 22, `netcode-win` = -O2)

Bench: `tl_gate0` on branch `w2-gate0` at the `W2 gate0 review 2` commit (`git log` of this
directory), `docs/GATE0-BENCH.md` §8. **Every file here was produced by that binary** (the
adversarial review re-ran every scenario after its last bench edit; the earlier run's hash
traces reproduced bit-for-bit on every scenario, `TODO.md` "W2 gate0 — adversarial review").
Every verdict below is the bench's own stdout (`verdicts_*.txt`); every CSV is the per-tick trace
of the run that produced it (`docs/GATE0-BENCH.md` §4 columns + R-2's `broadphase_us`;
`--csv-every 10` for G-03, `100` for G-04 — one row per 10/100 ticks; the run-twice hash compare
ran on every tick in memory). `run_twice=identical` = the two in-process runs' hash traces
matched on every tick. `shadow_*.csv` are the FLOAT-SHADOW error traces (dev tier, never
authoritative; the shadow has no escape stop, so its `max_abs_err` after an ejection is the
distance a runaway particle travelled, not a precision number — read the ticks before the
ejection). A scenario whose carrier leaves the sealed box is stopped and is a tunneling FAIL
whatever it measures; its CSV is the truncated trace. The Pi leg (G-06 cross-ISA, G-05 Pi half)
is BLOCKED on RR-1 (`TODO.md`).

Bench constants the spec leaves open (`tests/gate0/scenes.cpp` / `solver.cpp`; none is a
threshold or a row): density compliance α = 1.3e-6 (α̃ = α/h² = 0.3 at 480 Hz, `--alpha 1302`),
μ = 0.5, restitution 0, contact margin 2 texels + per-tick travel, particle lattice at CANON's
2 texels, the feather = a 2.5 m × 0.25 m plank at 1/4096 kg (the smallest 4096:1 body whose
inverse inertia fits `invmass_t`), G-03 settle tick 2000, G-04's free boxes a quarter metre over
the liquid / on the floor, G-05's bodies one floor row in a 600 m box. The density solve is the
two-pass Jacobi λᵢ+λⱼ form run before the colour sweep, NOT `ALLOY.md` §14.4.3's owner-only
single pass (RR-10): G-03/G-04/G-05 grade that solver.

Metric floors: `jitter_p95_texel` and `intra_tick_max_texel` are sampled at 1e-4 texel (= 1.6
`pos_t` quanta), so `0.0000` reads "under 1.6 quanta per tick", not zero; `--perturb`
(`verdicts_G01_perturb.txt`) shows the metric moving: a 1-texel stagger of the stack gives
0.0004, a 1 m stagger 0.0005, `--ladder 0` 0.0009.

## The §8.5 row — substeps = 8, PC

| Scenario | Verdict | Metric (the verdict line) | CSV | Reading |
|---|---|---|---|---|
| G-01 | **PASS** | jitter p95 0.0000 texel, top drift 0.0000, pop 0, intra-tick max 0.0001, 10,000 ticks, run-twice identical | `G01_s8_l1.csv` | also PASS at 4 and 16 (`G01_s4_l1.csv`, `G01_s16_l1.csv`); `--ladder 3` identical (`G01_s8_l3.csv`); `--ladder 0` jitter 0.0009 texel, intra-tick 0.0010 (`G01_s8_l0.csv`, `verdicts_G01_ladders.txt`) — the per-constraint `lambda_t` creep, RR-8 |
| G-02a | **PASS** | sustained penetration 0.0003 texel, tunneling 0, λ saturations 0 | `G02_s8_l1.csv` (rows `G02a`) | plank dropped 2 m onto the resting boulder |
| G-02b | **FAIL** | tunneling 1 (the plank is pushed 0.8 m into the floor by tick 2), ω clamps 5, V_MAX clamps 7 | `G02_s8_l1.csv` (rows `G02b`) | the boulder never tunnels (and leaves tick 0 moving up at 216 m/s with e = 0 — review D7); the 4096:1 plank is spun 27° and driven into the floor, fx and double within 55 raw units — RR-9, RR-15 |
| G-03 (5k = the 39 m column) | **FAIL** | tunneling 1 at tick 34, both runs identical | `G03_s8_l1.csv` | the double shadow ejects the same particle at tick 32 (`--shadow --watch 740`; `shadow_G03_s8_l1.csv`, read the ticks before 32): solver design (RR-10) + the spec's geometry (RR-14) |
| G-03 (1k = 7.8 m column) | **FAIL** (by the KE rule) | density error p95 **2.12 %** after settle (INVESTIGATE band), 4 KE-window increases in 5,000 ticks; total energy flat to 0.014 % | `G03_1000/G03_s8_l1.csv` | the column that holds; the 2.1 % is the compliant constraint's equilibrium at α̃ = 0.3 (the double tracks it), not a kernel-precision number; the "boiling" is a 0.014 % breathing, reported as the rule reads it |
| G-04 (20,000 ticks) | **INVESTIGATE** | 4 envelope increases, all inside 1 % of initial (energy 3.24e10 → 6.80e9 settling, then flat to 0.04 %); run-twice identical | `G04_s8_l1.csv` | 1e6 ticks = 16 h per run here; `--ladder 3` PASS at 3,000 ticks (`G04_s8_l3_3k`) |
| G-05 | **FAIL** | tunneling at tick 139 (10k), 83 (20k), 83 (50k): the liquid base is crushed under its own column | `G05_s8_l1.csv` | cost of the ticks that ran (solve + broadphase, p50 / p95): **10k 347 / 360 ms**, **20k 605 / 746 ms**, **50k 1,425 / 1,674 ms**; per tick at 20k: density pass 388 ms + XSPH velocity pass 168 ms + broadphase 46 ms + colour sweep 4 ms; 3.29 M pair evaluations (164 per particle) at **184 ns each** — the pair kernel is 2 × `isqrt64` + 3 × `rne_div` (176 ns in isolation; 5.5 ns in double) — RR-13 as reframed by review D2. Pi: BLOCKED (RR-1) |
| G-06 | **PASS** (PC) | every scenario's two in-process runs bit-identical (G-01, G-02a/b, G-03, G-04, G-05) | `G06_s8_l1.csv` | the G-02b and G-03 legs compare only the ticks before their escape stops (2 and 34) — review D6; Pi: BLOCKED (RR-1); the UBSan/ASan evidence run needs the Linux lane (`TODO.md` §8.5 remainder) |

## The substep sweep (G-03 at 1k, G-04 at 3,000 ticks)

| Substeps | G-03 (1k) | G-04 |
|---|---|---|
| 4 | FAIL, tunneling at tick 7 (α̃ = 0.075: the compliance scales with h²) — `G03_1000/G03_s4_l1.csv` | FAIL, tunneling at tick 7 — `G04_s4_l1.csv` |
| 8 | 2.12 % density error, KE rule — `G03_1000/G03_s8_l1.csv` | INVESTIGATE (above) |
| 16 | **1.56 %** density error, 1 KE-window increase — `G03_1000/G03_s16_l1.csv` | FAIL, tunneling at tick 876 (ω clamps 9) — `G04_s16_l1.csv` |

G-01/G-02 are identical verdicts at 4/8/16 (`verdicts_G01_G02.txt`; G-02b's plank escapes at
tick 1/2/320). The sweep says: the liquid's stability is a compliance question, not a substep
question (RR-10); the rigid scenarios do not care.

## What moved and what did not

- No threshold, world constant or palette row was changed. The one doc correction is
  `GATE0-BENCH.md` §8.3's "16 extra bits" → 12 (`ALLOY.md` §14.4.3 is the home).
- The ladder: rung 1 (i64 λ across the sweep) is the bench's default and is REQUIRED (RR-8);
  rung 2 is the helpers' RNE; rung 3 (residual carry) changes nothing measurable on G-01, G-03
  (2.17 % vs 2.12 %) or G-04; rung 4 was not needed for any rigid-body outcome.
- Every remaining FAIL is the liquid's (RR-10/RR-11/RR-14), the scenario's (RR-15) or the cost
  budget's (RR-13), each reproduced by the double shadow — the palette rows are not what those
  verdicts are about. The cost is two functions, not a row: `isqrt64`'s bit-serial loop and a
  kernel spelled with two roots and three divisions per pair (review D2).
