# Gate 0 results — 2026-08-25, dev PC, **rev-2 verification re-run on the merged tree**

**This is the cross-machine leg of the post-rulings closeout** (`TODO.md` "Closeout remainder"):
the full rev-2 scenario matrix of `../2026-08-25-pc2-win-netcode-rev2/` re-run on the dev PC,
against the **merged** `w2-gate0-closeout` tree — the W2 closeout commits + main at `63083c4`
(target matrix = `CANON.md` {Windows, Linux} × {x86-64, arm64}; the Pi 4 left the program) + the
W1 wave-boundary sweep and the RR-16 wrap-stands ruling — so the evidence is of the code that
will ship, not of the branch point. Same flags and seeds as the PC2 run (`--alpha 1302`, μ = 0.5,
seed 1, ladder 1 default; that README is the home for the bench constants and per-scenario
readings — nothing is restated here).

## Host, toolchain, tree

- **Host:** the dev PC (Intel Core i5-8400, 32 GB, Windows 11 10.0.22631) — the physical perf
  reference (`CANON.md` "Build tiers and targets"). LLVM/clang-cl 22.1.7, CMake 4.3, Ninja 1.13.
- **Tree:** `w2-gate0-closeout` merged with main `63083c4` and the sweep/RR-16 commits
  (`1daccbc`); `build_id` (netcode-win) `3f16e1f0b7f9b612b3d041cb1000554ac5296c087767674855c195cc7929f63e`.
  The matrix ran on the tl_gate0 built before the sweep merge (that merge is comment-only +
  runner/test files on the gate0 path); the relinked final binary re-ran G-03b and reproduced
  the trace bit-for-bit, so the two binaries are behaviourally identical on this bench.
- **tl_tests, merged tree, this PC:** all four tiers green — 262 selected, 0 failed
  (debug/dev 258 passed + 4 skips, netcode/ship 240 + 22 tier skips), including both re-pinned
  fx trace hashes (A `f29c2358a2932bbf`, B `22598f0e81cb2e7f`) — the second machine of the
  two-PC pin protocol.

## The verification: every leg bit-identical to PC2

Every CSV of the PC2 rev-2 matrix was regenerated here and compared row-for-row with the two
machine-local timing columns (`solve_us`, `broadphase_us`) excluded; the shadow CSVs (dev tier)
were compared **byte-for-byte**. The bit-identical CSVs are **not duplicated into this
directory** — `../2026-08-25-pc2-win-netcode-rev2/` is their byte home; this directory carries
this machine's verdict lines, the one machine-specific CSV (G-05's timing trace), and this table:

| Leg | Rows compared | Result |
|---|---|---|
| G-01 s4 / s8 / s16 (10k ticks) | 10,001 each | IDENTICAL |
| G-01 s8 `--ladder 0` / `--ladder 3` | 10,001 each | IDENTICAL |
| G-01 `--perturb` 1 / 16384 / 262144 (3k) | verdict lines | byte-identical (incl. the 0.0042) |
| G-02 s4 / s8 / s16 | 2,003 / 2,004 / 2,152 | IDENTICAL |
| G-03 (1k) s4 / s8 / s16 / s8-l3 | 2 / 701 / 701 / 701 | IDENTICAL |
| G-03b (5k) s8 | 5 | IDENTICAL (escape tick 34) |
| G-04 s4-3k / s16-3k / s8-l3-3k / **s8-20k** | 2 / 24 / 3 / 106 | IDENTICAL (the s8-20k **regression escape reproduces at exactly tick 10417**) |
| G-05 10k / 20k / 50k | 312 (hash columns) | IDENTICAL (escapes 141 / 83 / 84, same ticks) |
| G-06 (7 legs incl. G03b) | 7,281 | IDENTICAL; PASS |
| shadow G01 / G02a / G02b / G03 (dev tier) | whole files | **byte-identical**, incl. the double columns |

Every verdict matches PC2's: the amended-criteria verdicts (G-02b PASS under R-5, G-03 FAIL by
the letter at 2.1184 % + 4 KE windows, G-03b RECORDED, G-04 FAIL at tick 10417, G-05 FAIL vs
32 ms, G-06 PASS) are properties of the code, not of a machine. Two Windows x86-64 machines and
two toolchain installs now produce this matrix bit-for-bit; the {Linux, arm64} legs of the
target matrix are CI's (the four-leg run on this branch).

## G-05 cost on THIS machine — the binding perf-reference numbers (netcode −O2, single thread)

Per `GATE0-BENCH.md` §4 the PC number is the binding one, and this PC is the perf reference —
PC2's faster laptop numbers are corroboration, these grade the threshold:

| Count | p50 / p95 (ms) | ns per pair eval | density / XSPH-velocity / broadphase per tick (ms) | rev 1 (this PC) |
|---|---|---|---|---|
| 10k | 273 / 307 | 158 | 136 / 88 / 28 | p50 347, 184 ns/pair |
| 20k | **501 / 615** | **152** | 268 / 167 / 50 | p50 605, 184 ns/pair |
| 50k | 1,115 / 1,312 | 145 | 584 / 380 / 111 | p50 1,425 |

**Verdict unchanged: FAIL vs the R-3 32 ms threshold** (615 ms p95 ≈ 19× over). Normalize-once
bought ~17 % on this machine (184 → 152 ns/pair; PC2 saw ~25–30 % — the split between `isqrt64`
latency and divide latency differs by microarchitecture). The named next steps stand:
Newton-from-clz `isqrt64` through the fxcheck oracle + cached W (W3), then SIMD
(`FX-PALETTE.md` §9 R-4) as the escalation.

## What this run closes and what it does not

- Closes: the `TODO.md` closeout remainder (dev-PC spot-verify — upgraded to the full matrix);
  the evidence-matches-shipping-tree requirement; the two-PC halves of the rev-2 trace pins.
- Does not close: the {Linux, arm64} conformance legs (the four-leg CI run on this branch) and
  everything filed to W3 (RR-10 liquid design pass, which owns G-03's letter, G-04's regression
  class and the G-05 kernel cost items).
