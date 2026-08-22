# Testing — runner, harness, driver, CI gates (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §8. Test infra lands **first** (before
> any subsystem) — the harness is now the only determinism safety net (`DETERMINISM.md` §1).
> **Owns:** `tests/runner/`, `tests/driver/`, `tests/gate0/` (`GATE0-BENCH.md`), `tests/hovel/`
> (`NETCODE.md` §19), `tools/audit/` (symbol + include gates), CI configuration.

---

## 0. What we build vs what we don't

Ore's `ore test` + `assert_no_alloc` are gone with Ore. We build a small runner ourselves
(~500 lines) because the alternatives (Catch2, doctest, gtest) are C++ libraries that violate the
subset, pull `<iostream>`/exceptions, and would be the only place in the binary with them. The
runner is test code, still in the subset, with one exemption: a build-generated test list
(no static registration).

---

## 1. Runner (DECIDED)

```cpp
TL_TEST(slotmap_reuse_is_lifo, "containers", "fast") { ... TL_EXPECT_EQ(a, b); ... }
```

- **Discovery:** CMake globs `tests/**/*.test.cpp` and generates `test_list.inc` (name, tags,
  fn) — no static constructors. Tags filter (`--tag containers`, `--tag !slow`), `--filter glob`.
- **Assertions:** `EQ/NE/LT/LE/GT/GE/TRUE/FALSE/NULL/NOT_NULL`, `NEAR_FX<R>(a, b, tol)` (fx rows;
  never raw float equality anywhere), `IN_RANGE`, `SPAN_EQ`, `MEM_EQ`, soft `EXPECT_*` vs hard
  `ASSERT_*`. Fatal-expected tests (`TL_TEST_EXPECT_FATAL`) run in a child process and check the
  exit code + stderr marker.
- **Isolation:** `--isolate` re-executes each test in its own process (one worker per core,
  private cwd, sorted deterministic replay of failures). Default in CI, optional locally.
- **Fixtures:** plain functions; a `World` fixture boots headless + a Luau scene.
- **Zero-alloc guard:** `TL_ASSERT_NO_ALLOC(arena)` = mark pair (`MEMORY.md` §2) + the CRT
  counting shim; `TL_ASSERT_NO_TICK_ALLOC(world)` runs a tick under the arena-offset guard.
- **Run-twice:** `TL_ASSERT_DETERMINISTIC(setup_fn, ticks)` builds two worlds, runs both, compares
  per-arena hashes every tick (the dual-sim harness as a one-liner).
- **Golden/oracle:** `TL_GOLDEN_TSV(name, span)` compares against `tests/golden/<name>.tsv`;
  `--pin` rewrites. Goldens are text; binary goldens (pixel) go to a CAS-addressed dir later.
- **Report:** TSV `suite, test, status, dur_ms, detail` + a summary; non-zero exit on any fail.
- **Property tests:** a seeded `rng_key` generator + shrinking by halving the op sequence (good
  enough; a full shrinker only if a failure demands it).

---

## 2. The headless driver (DECIDED)

`tests/driver` — one exe, five jobs: boots the headless platform, loads a scene (a Luau script),
feeds the **Script** producer (or a `RecordedInput` via **Replay**), steps N ticks, records
per-arena hashes, probes and timings to CSV. Flags: `--scene --seed --ticks --workers --record
--replay --dual --workers-sweep 1,2,8,16 --dump-probes --csv`. Byte-identical output for the same
seed is the contract. It is the AI-iteration driver, the CI scenario runner, the soak driver, and
the chaos subject. Scenario tests are driver scripts asserting over the hash stream and world
queries ("spawn X, command Y, assert state at tick N").

---

## 3. The determinism harness (DECIDED — `DETERMINISM.md` §6)

Dual-sim · record→replay · worker sweep · long-run fuzz · cross-ISA · sanitizer runs. All are
driver invocations; the first three are in the PR lane, fuzz nightly, cross-ISA nightly when the
Pi is reachable, sanitizers in the PR lane on the sim test set.

---

## 4. Cross-ISA (DECIDED — required, infra-gated)

The PC cross-compiles the driver + tests for aarch64 (`BUILD.md` §2), deploys to the Pi 4 over
SSH, runs the same scenes with the same seeds, pulls the hash CSVs, diffs. Any difference is a bug
(UB by default hypothesis). The Deck (x86-64 Linux) runs the same job to separate OS from ISA.
Gate 0's G-06 is the first use; Hovel Milestone A the second; the 10 h soak (Milestone E) the
successor of Ore's 43 M-tick run.

---

## 5. Static gates (DECIDED — `tools/audit/`)

| Gate | What it checks | Lane |
|---|---|---|
| symbol audit | `llvm-nm --undefined-only` over every sim static lib vs the allowlist (`CPP-SUBSET.md` §4) | PR, blocking |
| include firewall | no banned system include in `src/`; no backend header outside its wrap module; no `float`/`double` tokens in sim TUs; no `static` mutable; no `thread_local`; no `std::` | PR, blocking |
| WIRE_STRUCT | every struct in `net/wire.h`, `save.h`, `InputFrame` has sizeof + offsetof static_asserts (a script checks the macro was used) | PR, blocking |
| rebuild-time budget | full rebuild < 10 s, incremental (touch one sim TU) < 2 s on the reference PC; a regression is a failure, like perf | PR, blocking (measured on the CI box with its own budget) |
| fingerprint stability | two clean builds of the same tree produce the same fingerprint | PR |
| warnings | `-Werror` across all tiers and all platforms incl. the cross target | PR |

---

## 6. CI lanes (DECIDED)

| Lane | Contents | Budget |
|---|---|---|
| **PR** (blocking) | build all tiers (dev/netcode/ship) for Windows + Linux + aarch64 cross; unit/property (`--isolate`); dual-sim + replay + worker sweep on the scene set; sim tests under UBSan+ASan; static gates; descriptor-level render tests | < 10 min |
| **nightly** | long-run fuzz; cross-ISA on the Pi + Deck; save cross-build load (yesterday's fixture); pixel goldens (software renderer, FLIP-compared, never blocking); fx exhaustive oracle runs | hours |
| **weekly** | Hovel soak scenarios once Hovel exists; Gate 0 re-run if the palette or the solver changed | 10 h |

Flakiness rules: **no PR-lane retries**; a flaky test is quarantined to nightly with an owner; a
**determinism-gate flake is P0** — it is a nondeterminism bug by definition.

---

## 7. The per-module rubric (every module's tests line)

1. fresh instance per test (two instances for container determinism);
2. happy path per public fn; 3. error path per failure mode, asserting state stays valid;
4. edge matrix (0/1/many, empty/full, min/max, null handle, wrap, malformed input);
5. cleanup: counts return to baseline; 6. determinism: same seed → identical trajectory;
7. zero-alloc on every hot path; 8. a generous perf ceiling on batch ops (order-of-magnitude
regressions only); 9. render/GPU: test the descriptor (sort order, batches, layout), not pixels;
10. round-trip/golden for every encoder; byte-stability test.

Fuzzing: structure-aware fuzzers for every untrusted-byte surface — image loader, save parser,
net packet decode, Luau bytecode loader — under ASan in nightly.

---

## 8. Rulings (closed 2026-08-22 — nothing open)

- **R-1 The runner emits TSV and `--junit` from day one** (the JUnit XML writer is ~40 lines over
  stb_sprintf; every CI system reads it). CI is **GitHub Actions** for the Windows/Linux/cross
  lanes, with the Pi and Deck as self-hosted runners for the nightly cross-ISA job.
- **R-2 `tests/` obeys the subset**, with two exemptions: the generated test list and
  `printf`-class io + clock + filesystem access (tests are not sim code). `tools/` is fully exempt.

*Rev 1 — 2026-08-22.*
