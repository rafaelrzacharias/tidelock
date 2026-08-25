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
driver invocations; the first three are in the PR lane, fuzz nightly, cross-ISA in the PR lane on
the hosted arm64 legs (re-ruled 2026-08-25), sanitizers in the PR lane on the sim test set.

---

## 4. Cross-ISA (DECIDED — required; re-ruled 2026-08-25, no longer infra-gated)

Every leg of the `CANON.md` target matrix builds natively on a hosted CI runner and runs the same
scenes with the same seeds; hash traces and `build_id` are diffed across legs. Any difference is a
bug (UB by default hypothesis); the OS and ISA axes of the matrix separate OS effects from ISA
effects. Physical perf/soak jobs run the same diff on reference hardware over SSH via
`tools/deploy.sh` (`BUILD.md` §7) — the PC now, the Steam Deck when it enters the bench.
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
| header contracts | every `module.h` has a contract block naming its spec section; every public function in a module header has a contract comment (`CPP-SUBSET.md` §6) — `tools/audit/includes.py` | PR, blocking |
| `NOMINMAX` | every file including `<windows.h>` defines `NOMINMAX` on an earlier line (`PLATFORM.md` §9.1) — `tools/audit/includes.py` gate 7, the one gate that walks `tests/` as well as `src/` | PR, blocking |
| cross-target divergence | `tools/audit/targets.py`: every sim TU preprocessed and its record layouts dumped for the four `CANON.md` target triples, then diffed. A per-target `#if`, a `#pragma pack`, an `alignas` or a bit-field is a finding, in any spelling - the class four W0 reviews kept finding holes in when it was a regex. Needs no sysroot | PR, blocking |
| **doc audit** | `tools/docaudit/docaudit.py`: dangling `NAME.md §x.y` refs, numbers contradicting `CANON.md`, docs missing from `docs/README.md`, stale markers; regenerates `docs/XREF.md`; a commit touching `src/<module>/` without its doc needs `[docs:none]` | PR, blocking |
| warnings | `-Werror` across all tiers and all platforms incl. the cross target | PR |

---

## 6. CI lanes (DECIDED)

| Lane | Contents | Budget |
|---|---|---|
| **PR** (blocking) | build all tiers on the `CANON.md` target matrix ({Windows, Linux} × {x86-64, arm64}, hosted native runners, per-leg binary-ISA assertion); unit/property (`--isolate`) on every leg — the fx trace pins inside are the cross-ISA gate; four-way `build_id` diff; dual-sim + replay + worker sweep on the scene set; sim tests under UBSan+ASan on both Linux ISAs; static gates; descriptor-level render tests | < 10 min |
| **nightly** | long-run fuzz; perf regression on the elected CI leg (`WORKFLOW.md` §4); physical network soaks once Hovel exists (replay-diff against PR artifacts); save cross-build load (yesterday's fixture); pixel goldens (software renderer, FLIP-compared, never blocking); fx exhaustive oracle runs; G-06 cross-leg hash diff on the hosted runners; the four-leg 8-peer battletest once net-p3..p8 exist (`NETCODE.md` §19.10) | hours |
| **weekly** | Hovel soak scenarios once Hovel exists; Gate 0 re-run if the palette or the solver changed | 10 h |

Flakiness rules: **no PR-lane retries**; a flaky test is quarantined to nightly with an owner; a
**determinism-gate flake is P0** — it is a nondeterminism bug by definition.

**Per-child timeout** (ruled 2026-08-24; the flag is §9.1's `--timeout-ms`): the **PR** lane passes
**`--timeout-ms=120000`** and **nightly runs without one** (`0`, off). One number serves the PR lane
because it already runs `--tag !slow`, so the `slow` exhaustive rows — minutes each on their own —
are not in it; nightly is where those live and where a wall-clock limit would be the wrong tool. A
timed-out child is killed and reported `TIMEOUT`, its own status: it fails the run, names the test,
and prints the P0-flake line above, because a test that hangs *is* a determinism-gate flake. Local
runs default to `0`: a debugger session must not be shot.

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
  stb_sprintf; every CI system reads it). CI is **GitHub Actions**: hosted native runners cover
  the whole `CANON.md` target matrix in the PR lane (re-ruled 2026-08-25 — conformance no longer
  waits on owned hardware); the Steam Deck joins as a self-hosted runner for the nightly
  perf/soak job when it enters the bench (the Pi 4 left the program, same ruling).
- **R-2 `tests/` obeys the subset**, with two exemptions: the generated test list and
  `printf`-class io + clock + filesystem access (tests are not sim code). `tools/` is fully exempt.

## 9. Implementation specification

### 9.1 Runner internals (`tests/runner/`)

```cpp
struct TestInfo { const char* name; const char* tags; void (*fn)(TestCtx*); const char* file; u32 line; };
// tests/runner/test_list.inc is generated by CMake (cmake/testlist.cmake) by scanning for `TL_TEST(` in tests/**/*.test.cpp:
//   extern void test_slotmap_reuse_is_lifo(TestCtx*); … static const TestInfo TL_TESTS[] = { {...}, … };
#define TL_TEST(name, tags...) void test_##name(TestCtx* t); /* registered by the generated list */ void test_##name(TestCtx* t)
struct TestCtx { u32 failures; u32 soft_failures; Scratch scratch; VMemApi* os; const char* name; u8 expect_fatal; };
```

CLI: `tl_tests [--filter glob] [--tag t|!t]* [--isolate] [--workers n] [--junit path]
[--report path] [--list] [--seed n] [--timeout-ms n]`. `--isolate`: the parent enumerates, then
re-executes itself with `--run-one <index>` per test on a worker pool of `core_count` processes;
exit code + captured stderr → status; a `TL_TEST_EXPECT_FATAL` test passes iff the child exits with
the fatal code (2) and prints the `TL_FATAL` marker.

`--timeout-ms n` (**ruled 2026-08-24**): a per-**child** wall-clock limit, `0` = off and the
default. Both wait paths are bounded by it — the `--isolate` pool's (each slot carries its own
deadline; the pool's multi-wait is bounded by the soonest of them) and the single-child path a
fatal-expected row takes without `--isolate`. At the deadline the child is **killed** and its row
is `TIMEOUT`: its own status in the TSV, the JUnit XML and the summary, counted separately from
`FAIL`, and it fails the run. `TIMEOUT` beats `expect_fatal` in `tl_child_verdict` — the exit code
of a process the runner killed is the runner's own, so a hung fatal-expected row must never read as
a satisfied expectation. A timed-out row is **not** re-run by the failure replay (it would only
hang again for another full budget). The limit applies to children only: an ordinary test without
`--isolate` runs in-process, where there is no separate process to kill. §6 carries the lane values. Assertions record file:line:expr and continue (`EXPECT`) or `longjmp`-free
early return via a `return` in the macro (`ASSERT` macros are `if (!(cond)) { record; return; }` —
so they are only usable at test-function top level; helpers return `bool`). `TL_ASSERT_NO_ALLOC`
wraps a block: arena mark pair + CRT counter read. `TL_ASSERT_DETERMINISTIC(setup_fn, ticks)`
calls `harness_dual_sim`. Report: TSV + JUnit.

### 9.2 The driver (`tests/driver/`)

`tl_driver --scene <luau> --seed <n> --ticks <n> [--workers n | --workers-sweep 1,2,8,16]
[--record out.tlri | --replay in.tlri --verify] [--dual] [--dump-probes dir] [--csv out]
[--snapshot-every n] [--ballast bytes]`. It boots the headless platform, runs `app/wiring.cpp`'s
init with the given scene as the sim script root, sets the Script (from the scene's `inputs`
table) or Replay producer, runs `engine_tick_once` `ticks` times, and writes the CSV
(`tick, world_hash, arena_hashes…, sim_us, per-pass us, probe summary`) and/or the recording.
Exit code 0 = completed + (if `--verify`) all hashes matched; 3 = divergence at the printed tick.

Scene script contract (`script/scenes/*.luau`): returns `{ world = WorldDesc table, spawn = fn(w),
inputs = { ScriptedEvent… } }`; the same scene files serve Gate 0-style stress scenes, harness
scenes and Hovel.

### 9.3 CI lane mapping

| Job | Command |
|---|---|
| unit | `tl_tests --isolate --tag !slow --timeout-ms 120000 --junit` (§6: nightly runs without the timeout) |
| dual-sim / replay / sweep | `tl_driver --scene scenes/harness_*.luau --dual`, `--record` then `--replay --verify`, `--workers-sweep` |
| sanitizers | same unit + harness jobs on the `-DTL_SANITIZE=ON` build (UBSan+ASan), timing ignored |
| audits | `tl_audit_symbols`, `tl_audit_includes`, `tl_rebuild_budget`, fingerprint-stability (two clean builds, diff `build_id.txt`) |
| cross-ISA (PR lane, hosted legs) | each leg: `--record`, then cross-leg `--replay --verify` on the same tree's artifacts; the Deck repeats it nightly as physical hardware when it joins |

*Rev 1 — 2026-08-22; §5/§6/§8 R-1 re-ruled to the `CANON.md` target matrix (hosted native CI
runners), 2026-08-25; §3/§4/§6/§8 R-1/§9.3 swept for the Pi 4's removal (perf reference = the PC
now, the Steam Deck later), same date.*
