# Gate 0 results — 2026-08-25, second PC (reproduction run; x86-64 Windows 11, clang-cl 22, `netcode-win` = -O2)

**This is the cross-machine reproduction of `../2026-08-25-pc-win-netcode/`** (same branch
`w2-gate0`, same commit `1bd0384`, same scenario matrix, flags and seeds — that run's README is
the home for the bench constants, metric floors and per-scenario readings; nothing is restated
here). **Verdict: determinism holds. Every physics column, per-tick hash, tick count, escape
tick and verdict outcome is bit-identical to the dev PC's run across all 19 solver traces and
all 4 float-shadow traces. Zero divergent ticks.** Only the timing columns (`solve_us`,
`broadphase_us`, the G-05 cost figures) differ, as they must — they are reported below as this
machine's numbers and were excluded from the diff.

## Host and toolchain

- **Host:** Intel Core Ultra 9 185H, 32 GB, Windows 11 Enterprise 10.0.26200.
- **Toolchain (matches `toolchain/VERSIONS` exactly):** clang-cl 22.1.7 and CMake 4.3.2 as
  **portable user-scope installs** under `%LOCALAPPDATA%\Programs` (resolved from PATH — a
  different layout from the dev PC's `C:\Program Files\LLVM`; the presets resolve both from
  PATH and nothing cared); Ninja 1.13.2, Python 3.13.15; VS 2022 Professional MSVC 14.44 +
  Windows 11 SDK. `TL_STRICT_TOOLCHAIN` passed on configure (compiler major 22 = the pin).
- **`build_id` (netcode-win, clean tree at `1bd0384`):**
  `5080ca8085f1216d56388aa61821baae5f0f661adc4c8f99c65a51d3fac4327f` — target- and
  machine-independent by construction (`docs/BUILD.md` §9 R-8), so it is the same value the dev
  PC builds from this commit.
- **First run of `tl_tests` on this machine** (`--isolate --tag !slow`, the PR-lane invocation):
  254 selected, 229 passed, **0 failed**, 25 skipped — every skip is an env-gated `[fatal]`
  trigger test or a dev-tier-only log/probe/fmt test compiled out in `netcode`, plus the
  runner's two self-skips. No machine-setup traps beyond the known ones; nothing new for
  `LESSONS.md`.

## The comparison — per scenario, this run vs `../2026-08-25-pc-win-netcode/`

Compared with timing columns (`solve_us`, `broadphase_us`) excluded from the CSVs and the
`p50_us/p95_us/p99_us/ns_per_pair_eval/phase_us_per_tick/pc_20k_p95_us` tokens excluded from the
verdict lines; everything else byte-compared after line-ending normalization.

| Scenario / variant | Verdict (both machines) | Trace vs dev PC | Escape tick (both) |
|---|---|---|---|
| G-01 s8 / s4 / s16 (10k ticks) | PASS / PASS / PASS | bit-identical (all rows, all hashes) | — |
| G-01 s8 `--ladder 0` / `--ladder 3` | PASS / PASS | bit-identical | — |
| G-01 s8 `--perturb 1 / 16384 / 262144` (3k ticks) | PASS ×3 | verdict lines identical (CSV not committed, same as dev PC) | — |
| G-02a s8 / s4 / s16 | PASS ×3 | bit-identical | — |
| G-02b s8 / s4 / s16 | FAIL ×3 | bit-identical | 2 / 1 / 320 |
| G-03 5k s8 | FAIL (tunneling) | bit-identical | 34 (particle 740, same coordinates) |
| G-03 1k s8 / s16 / s8 l3 | FAIL 2.12 % / FAIL 1.56 % / FAIL 2.17 % | bit-identical | — |
| G-03 1k s4 | FAIL (tunneling) | bit-identical | 7 (particle 37, same coordinates) |
| G-04 s8 20k ticks | INVESTIGATE (4 envelope increases) | bit-identical | — |
| G-04 s4 / s16 (3k ticks) | FAIL / FAIL | bit-identical | 7 / 876 |
| G-04 s8 `--ladder 3` (3k ticks) | PASS | bit-identical | — |
| G-05 10k / 20k / 50k | FAIL ×3 | bit-identical | 139 / 83 / 83 (same particles, same coordinates) |
| G-06 (all six legs, run-twice) | **PASS** — `pc_two_runs=identical` | bit-identical | 2 / 34 / 139 (legs) |
| shadows G01, G02a, G02b, G03 (dev tier) | — | **bit-identical including the double world** | — |

Two verdict *files* (`verdicts_G04_s4_3k.txt`, `verdicts_G05_s8.txt`) differ from the dev PC's
only in the interleaving order of stdout verdict lines vs stderr escape messages — as sorted
line sets they are identical, timing tokens aside. A buffering artifact of how the two shells
redirected the streams, not a bench output difference.

The shadow traces put a second, stronger fact on the table: the dev-tier **double-precision
mirror also reproduced bit-for-bit** across the two machines (same compiler, same flags, same
ISA) — so the fx-vs-double error columns agree everywhere too. `shadow_G02b` needed
`--ticks 300` to match the committed 300-tick trace (the shadow default is 120; the dev PC ran
the G02 shadow with `--ticks 300`).

## This machine's timing (excluded from the diff; the dev PC's numbers are in its README)

G-05 cost of the ticks that ran (solve + broadphase, from `verdicts_G05_s8.txt`):

| Count | p50 / p95 / p99 (ms) | ns per pair eval | per-tick phase split (broadphase / density / velocity, ms) |
|---|---|---|---|
| 10k | 283 / 324 / 347 | 163 | 21.4 / 167.8 / 74.2 |
| 20k | 505 / 678 / 702 | 153 | 39.0 / 330.4 / 144.9 |
| 50k | 1,191 / 1,385 / 1,674 | 155 | 89.8 / 739.5 / 340.4 |

~15–20 % faster than the dev PC across the board (20k p50 505 ms vs 605 ms; 153 vs 184 ns per
pair eval) — same two functions dominate (`isqrt64` ×2 + `rne_div` ×3 per pair, RR-13 / review
D2), same FAIL verdict at every count, and the G-05 verdict rule reads the 20k p95 (677.8 ms
here vs 745.8 ms there) against the same threshold either way.

## What this run adds

- **G-06's cross-machine leg is now real evidence, not a same-process tautology:** two
  machines, two independent first-time builds of `1bd0384` (different install layouts, same
  pins), identical hash traces on every tick of every scenario. The Pi leg (cross-ISA) stays
  BLOCKED on RR-1.
- No threshold, palette row, solver line or doc was touched. Out-of-scope rulings
  (RR-8..RR-15) untouched.
