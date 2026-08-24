# tidelock — TODO (the to-do list only)

> **Parallelism:** this list is the serial queue inside each lane; which lanes run concurrently,
> and the critical path, is `docs/ROADMAP.md`. Start a wave by opening one worktree per lane.

Worked top to bottom; the first open `[ ]` is what to do next. History → `git log`; gotchas →
`LESSONS.md`; rationale → the doc named on each line. Governing rules: `CLAUDE.md` principles,
`docs/ARCHITECTURE.md` §0/§4, test-infra-first.

## Gate 0 — the pivot gate (`docs/GATE0-BENCH.md`, `docs/FX-PALETTE.md`)
- [x] `src/foundation/fx.h` — `fx<Rep,FRAC>`, `mul<R>`/`div<R>` with RNE + widened intermediates,
      sat/wrap helpers, comparisons; exhaustive tests on small formats, property tests on 32-bit.
      (W1 fx, 2026-08-23; the fatal-expected halves wait for the runner, below.)
- [x] `fx_palette.h` — the rev-1 rows, derivation `static_assert`s, mixed-op instantiations, world
      constants, `H`/`G_SUBSTEP`; `FX_PALETTE_REV`. (W1 fx, 2026-08-23.)
- [x] `det_math.h` — `sqrt`/`rsqrt`/`sincos`/`atan2`/`isqrt`/`lerp`, `vec2<T>`, normalize/rotate;
      FixPointCS ports attributed; `tools/fxcheck/` three-layer oracle (exhaustive + differential +
      mpmath bounds) green for `sqrt`/`sin`/`cos`. (W1 fx, 2026-08-23; bounds in `det_math.h`.)
- [ ] `tests/gate0/` — disposable solver (gravity, rigid boxes, distance + contact + friction, PBF
      density), scenarios G-01..G-06, substep sweep 4/8/16, CSV + verdict lines, FLOAT-SHADOW config.
- [ ] Run on PC; cross-compile + run on Pi 4 (`docs/BUILD.md` §7); commit CSVs under
      `tests/gate0/results/`. Climb the ladder on any convergence failure (`FX-PALETTE.md` §3.2).
- [ ] **Decision commit:** `FX-PALETTE.md` rev 2 (rows DECIDED, or the fallback recorded) +
      `PIVOT-DESIGN.md` §3.1b/§12 updated + `LESSONS.md` entries per rung climbed.

## Ruling requests (filed, not improvised — CLAUDE.md rule 7)
- [ ] **RR-6 A tighter sine?** Measured (`FX-PALETTE.md` §4.4): the ported `SinPoly4` gives
      max 9.06 ulp of `q_t` (its documented 27.13 bits), not the 2 ulp §10.5 had guessed; the
      reference ships nothing better (its 64-bit `Sin` uses the same polynomial). At 1 m lever
      arm that is 8 nm - 1/450 of a `pos_t` quantum - so it is below anything Gate 0 can see.
      Options if a consumer ever needs more: a degree-5/6 minimax fit (bespoke - would need its
      own oracle run, which `tools/fxcheck` now provides), or a 2^k-entry table + linear
      interpolation. Not before a consumer names the need.
- [ ] **RR-5 Tagged palette rows?** `fx<Rep,FRAC>` keys a row by format, so `pos_t`/`invmass_t`,
      `q_t`/`stiff_t`/`angle_t`/`dt_t`, `vel_t`/`omega_t` and `scalar_t`/`lambda_t` are one C++
      type each: `pos_t + invmass_t` compiles, and the §3.1 op table collapses to format triples
      (`FX-PALETTE.md` §1, the W1 fx lane's finding). The compiler checks scale, not units. A
      `fx<Rep,FRAC,Tag>` third parameter would make the rows distinct types at the cost of an
      explicit `to<R>` at every unit change (`invmass_t → scalar_t` is already one) and a larger
      op table. Rev-1 ships format-keyed, as the doc now states; decide before Gate 0's solver is
      promoted (W3 alloy-solver), because that is the last point where the retag is a header edit.
- [x] **`foundation/tl_assert.h` landed from the fx lane, not tooling-rt.** `fx.h` needs
      `TL_ASSERT` on its first line of arithmetic and the tooling-rt lane had not published its
      header; the header's content is pinned by `TOOLING.md` §9 + `CPP-SUBSET.md` §9 R-3 +
      `tools/audit/allow.txt`, so it was transcribed (each tier routed to its own R-3 symbol;
      `TOOLING.md` §9 snippet aligned). **DONE 2026-08-24 (W1 tooling-rt):** `tl_assert.cpp` is
      real - `tl_fatal`/`tl_check_failed`/`tl_assert_failed` log then call `foundation/crash.h`'s
      `tl_crash_raise` seam (RR-7, `CPP-SUBSET.md` §9 R-4), whose built-in fallback (today's only
      path - `platform/` has not landed) prints the `TL_FATAL origin=<TL_FATAL|TL_CHECK|
      TL_ASSERT> <file>:<line>: <msg>` stderr marker and `exit(2)` - the exact contract
      `TESTING.md` §9.1's fatal-expected tests match on. Proven by
      `tests/foundation/tl_assert.test.cpp` (relaunches `tl_tests` itself via `--filter`, since
      `TL_TEST_EXPECT_FATAL` is not built yet). tooling-rt owns both files from here on.
- [ ] **Gate finding (fixed in the same commit, for the record):** `tools/audit/includes.py`
      rejected `fx.h` → `tl_assert.h` as "a sim TU includes a non-det foundation header" because
      the det/non-det split is by *stem* and `tl_assert` is non-det for its runtime. R-3 mandates
      the include. Exempted by full path (`PANIC_ABI_HEADER`), with three selftest fixtures
      (`tl_assert.h` passes; `tl_assert.cpp` and `tl_log.h` still fail). Also: `static_assert`
      message literals are literals to the non-ASCII gate, so header messages spell "section 3.1",
      not "§3.1" - not a gate bug, noted so nobody "fixes" it.
- [x] **RR-7 W1 tooling-rt is blocked: `src/foundation`'s non-det stems have no sanctioned io or
      storable state, but `TOOLING.md` §9 requires both.** Two gates, read together, make
      `TOOLING.md` §9's foundation half unbuildable as specified, not just the `<stdio.h>` line
      the lane brief expected:
      (a) `tools/audit/includes.py`'s system-include allowlist for `src/foundation` is
      `{stdint.h, stddef.h, string.h, limits.h}` with no exception for the non-det stems
      (`tl_log tl_prof tl_probe crash` in `src/foundation/CMakeLists.txt`), so nothing in that
      half can reach `<stdio.h>`/`<stdlib.h>`/an OS header - no `fprintf`, no `exit`, not even
      `abort`.
      (b) `tools/audit/symbols.py`'s writable-static check runs on the `--data-only` libs too
      (read the tool: `--data-only` drops the undefined-symbol layering check but keeps the
      `.data`/`.bss` zero check), and `CPP-SUBSET.md` §1 states the rule as "every object file
      in every `src/` lib" - no carve-out for non-audited state. `TOOLING.md` §9.2's
      `LogState`/`ProfState`/`ProbeState` (ring buffers, a TSV sink) cannot be namespace-scope
      globals under that rule, and neither can a stored pointer for "the installed crash
      writer" that `TL_FATAL` would call into.
      Meanwhile `CPP-SUBSET.md` §9 R-3 says `tl_fatal`/`tl_check_failed`/`tl_assert_failed`
      "are defined in `tl_foundation`... and reach io" - the ruling's own text assumes the io
      path (a) forbids outright. `TOOLING.md` §9's macro catalogue (`TL_LOG_TRACE(...)`,
      `TL_PROF_SCOPE(lit)`, `TL_PROBE_LOG(lit, v, n)`) takes no state/platform parameter at the
      call site, by design (`TOOLING.md` §0's "zero cost by absence" tier-gates argument
      *evaluation*, not argument *threading*).
      **Consequence:** none of `tl_log.cpp`/`tl_prof.cpp`/`tl_probe.cpp`, the crash writer, or a
      real (non-stub) `tl_assert.cpp` can be written today without either breaking a gate
      silently or improvising an assumption CLAUDE.md reserves for a ruling.
      **Options:** (A, recommended) two narrow exemptions, both scoped and justified the way R-3
      already is: (i) a stem-aware io allowance in `includes.py` for foundation's non-det stems
      only (the det/sim stems stay banned exactly as today); (ii) a bounded exemption from the
      `.data`/`.bss` rule for named, non-hashed, non-snapshotted tooling-plane singleton state
      (`LogState`, `ProfState`, `ProbeState`, the crash-writer install slot) - the tooling plane
      is never part of a world's registered arena set, so it carries none of the rule's own
      stated reason (the two-worlds-one-process test, rollback restoring only registered
      arenas). (B) thread an explicit state/platform-api parameter through every macro call
      site in the codebase - rejected as a default: touches every future consumer with no
      consumer yet to shape it against, and contradicts the macro text `TOOLING.md` §9.1
      already pins. (C) leave `foundation`'s macros header-only until `core`/`platform` exist to
      own the state as `World` singletons (the way `CvarTable` is already specified to) -
      rejected as a default: `TOOLING.md` §9.6's own build order puts `tl_log`/`tl_prof`/
      `tl_probe` before any sim code, specifically so the Alloy harness can use them; Option C
      inverts that order.
      **Status: RULED 2026-08-24 - Option A.** `TL_FOUNDATION_TOOLING` (`src/foundation/
      CMakeLists.txt`) names the exempt stems (`log prof probe crash tl_assert` - not `tl_log`/
      `tl_prof`/`tl_probe`; `TOOLING.md` §9.1's file table names the `.cpp` implementations
      `log.cpp`/`prof.cpp`/`probe.cpp`, only the headers keep the `tl_` prefix, which this line
      originally missed). `docs/CPP-SUBSET.md` §1 amended, §9 gained R-4; `tools/audit/
      includes.py`/`symbols.py` read the one list; selftest fixtures prove the exemption is
      stem-keyed, not directory-keyed, and opt-in (`--root` required). `tl_assert.cpp` and
      `foundation/crash.h`/`.cpp` are real now (`17dd4da` the ruling, next commit the runtime).
- [ ] **fx tests that need the runner lane** (`TESTING.md` §9.1 `TL_TEST_EXPECT_FATAL`): `div`
      by zero, `sqrt` of a negative, `normalize` of a zero vector, `atan2(0,0)`, `to<R>` out of
      range, `clamp` with lo > hi - each asserts in dev and has a documented release value; the
      release values are testable today only by building the test against a non-dev tier. Land
      them in `tests/foundation/fx_fatal.test.cpp` the day the macro exists.
      **The exit-2/marker contract `TL_TEST_EXPECT_FATAL` needs is real (W1 tooling-rt, 2026-08-24):**
      a fatal (any of `TL_FATAL`/`TL_CHECK`/`TL_ASSERT`) exits the process with code 2 and writes
      exactly one line to stderr, `TL_FATAL origin=<TL_FATAL|TL_CHECK|TL_ASSERT> <file>:<line>:
      <msg>\n` (`foundation/crash.h`'s `tl_crash_raise`, `foundation/crash.cpp`'s fallback) - match
      on the literal leading token `TL_FATAL`, never on `origin`, so all three tiers of the panic
      ABI satisfy one check. `tests/foundation/tl_assert.test.cpp` proves this today by relaunching
      `tl_tests` itself (`--filter <name>`, `TL_TESTS_EXE` from `tests/CMakeLists.txt`) - a pattern
      usable as a stopgap for any fatal-expected test until `TL_TEST_EXPECT_FATAL`/`--isolate`
      grow the same child-process-inspection support natively.
- [x] **W1 tooling-rt: `tl_log`/`tl_prof`/`tl_probe` land (`TOOLING.md` §9 foundation half,
      2026-08-24).** `tl_log.h` + `log.cpp` and `tl_probe.h` + `probe.cpp` are real (RR-7's io/state
      exemption); `tl_prof.h` ships macro-only, per `TOOLING.md` §9.6 build order item 3 - its
      runtime needs `NameHash` (`foundation/hash.h`) and `Scratch` (`MEMORY.md` §1.3), neither
      built yet, and has no consumer until the ECS scheduler auto-scopes systems. `tl_probe.h`'s
      macros have the same `NameHash` dependency (`"lit"_id`), so tests call the underlying
      `tl_probe_*`/`tl_log_write` functions directly with a caller-computed key/file/line, not the
      macros - reconciled the same day as `tl_prof.h`'s runtime (`NameHash` is `u64`, so nothing
      about either runtime's logic changes, only the call spelling).
      **Left for whoever builds those:** `probe_tsv_golden` (`TOOLING.md` §9.5) needs
      `TL_GOLDEN_TSV` (`TESTING.md` §1), which the runner lane has not shipped -
      `tests/foundation/tl_probe.test.cpp` asserts the staging buffer's exact bytes directly
      instead; swap to the golden macro the day it exists, per the same pattern as the fx-fatal
      tests below. `LogState`/`ProbeState` are simplified from `TOOLING.md` §9.2 (a private fixed
      array instead of `RingBuffer<T>`/`Map<K,V>`, no `ClockApi`/`FileApi`/`StrView`, tick pinned
      at 0) - reconcile against `CONTAINERS.md`/`PLATFORM.md`/`FRAME-LOOP.md` once those land. Both
      files' sinks stay staging-buffer-only; the disk flush waits for `PlatformApi.file.append`.
## W1 tooling-rt - the adversarial review (2026-08-24)
- [x] **Adversarial review of W1 tooling-rt (`c7dfcb5`..`2482058`) - DONE 2026-08-24 (Opus 5 high,
      fresh context), fixes in reviews 1-2 on `w1-tooling-rt`. Verdict: FIX FIRST -> shipped.**
      This lane edited the gates every other lane is judged by, so RR-7's four acceptance
      conditions were each re-tested against the real tools, not read. Two of the four had holes.
      What held, measured not assumed: the exempt stem set has exactly one home
      (`TL_FOUNDATION_TOOLING`, `src/foundation/CMakeLists.txt`) and both `includes.py` and
      `symbols.py` parse it - neither retypes it, and CMake fails configure if it ever escapes
      `TL_FOUNDATION_NONDET`; a newly added non-det stem inherits nothing (a planted
      `newthing.cpp` with `<stdio.h>` + a mutable static: 2 violations, and the same file renamed
      `tl_newthing.cpp` too); a det foundation stem with a mutable global still fails; the audited
      libs still report zero `.data`/`.bss`; `tools/audit/allow.txt` still carries exactly the
      three R-3 names and nothing else from the tooling plane. Ranked defects, all fixed:
      1. **(High, gate) `symbols.py` exempted a bare STEM in every `--data-only` lib.** `log`,
         `prof`, `probe` and `crash` are ordinary words, and the exemption was keyed on the archive
         member's stem with no lib scope - i.e. all of `src/` minus the two audited libs. Measured:
         a fabricated `tl_platform` holding a `log.o` with 4 bytes of `.data` reported 0 violations.
         `includes.py`'s twin exemption was already scoped to `src/foundation/`; this one was not.
         Fix: `--tooling-lib tl_foundation` (review 1) - `--root` alone now grants nothing - with a
         fixture for the other-lib case and one for `--root` without `--tooling-lib`.
      2. **(High, gate) `includes.py` granted io and mutable state to `tl_assert.h`.** Its stem is
         on the list, and it is also `PANIC_ABI_HEADER` - the one tooling header a sim TU may
         include (`CPP-SUBSET.md` §9 R-3). Measured: `#include <stdio.h>` plus `static int g_ta = 0;`
         appended to `tl_assert.h` produced 0 violations, i.e. both in every det TU in the tree.
         The comment at `includes.py`'s `PANIC_ABI_HEADER` was written about exactly this "header
         det, runtime non-det" split; the new stem rule reopened it from the other side. Fix: the
         header is excluded by path, only `tl_assert.cpp` is the tooling plane; two fixtures.
      3. **(High, runtime) `TL_LOG_MIN` was defined nowhere.** `TOOLING.md` §9 names it
         (`debug`/`dev` 0, `netcode` 2, `ship` 3); `cmake/tier.cmake` sets `TL_DEV` and never it,
         and `#if TL_LOG_MIN <= 0` reads an unknown identifier as 0 - so every level compiled in,
         in every tier including `ship`, and `tl_log.test.cpp`'s compile-out test mirrored the same
         `#if`, making its `#else` arms dead code. The one property `TOOLING.md` §0 turns on - a
         logged expression with side effects must not run in a tier that compiled the call out -
         had no test that could fail. It cannot be a `-D` either: `BUILD.md` §3 and
         `tools/audit/tier_parity.py` allow only the stripping defines to differ between `netcode`
         and `ship`. Fix: derived from the tier markers inside `tl_log.h`, `#ifndef`-guarded,
         verified per tier with `-E`; `tests/foundation/tl_log_compileout.test.cpp` pins
         `TL_LOG_MIN=4` before the include and proves zero argument evaluations in any tier.
      4. **(High, test) the fatal trigger killed a bare `tl_tests` run.** `tests/runner/main.cpp`
         selects every test when no `--tag` is given, so the `slow` tag protected nothing:
         `tl_tests --filter "tl_assert_forced_fatal*"` exited 2 with zero PASS lines. Gated on an
         env var the checker sets; a bare run is 66/66 again, and a third test asserts the triggers
         are inert without it.
      5. **(Medium) `TL_LOG_MSG_CAP` was 240** against `TOOLING.md` §9.2's `msg[224]` and §9.5's
         "223 bytes + NUL". Docs are the contract - the code moved, and the test now asserts the
         doc's literal 223 next to the derived value.
      6. **(Medium) `tl_log_test_ring_at` documented write order and returned ring order.** After a
         wrap slot 0 is not the oldest live record; the wrap test scanned every slot, so no
         assertion could tell the two apart, and there was no exactly-full test at all. Fixed, plus
         `TL_CHECK` on the documented precondition and two order assertions the old scan could not
         make.
      7. **(Medium) UB in `probe.cpp`'s `on_change`.** `raw - k.last_raw` is signed overflow for
         any pair straddling half the i64 range and `-diff` is UB outright at `INT64_MIN`
         (`CPP-SUBSET.md` §5). Magnitude computed in u64; `eps < 0` is now fatal rather than
         silently meaning "never row".
      8. **(Medium) the threading claims contradicted each other.** `crash.h` said `tl_crash_raise`
         may be entered from any thread; `tl_log.h`, which that path calls, said "main thread
         only". Both now state the same thing: the ring is unsynchronized, and a fatal off a worker
         races it the day `JOBS.md` starts one. Aspirational, and now labelled as such.
      9. **(Medium, docs) the exempt stem list was restated in `CPP-SUBSET.md` §9 R-4 and
         `TOOLING.md` §9** on top of its real home - the drift class R-4's own text cites
         `LESSONS.md` for, twice. Both now cite `TL_FOUNDATION_TOOLING` and name no members.
      10. **(Low) the probe throttle wrapped on a backwards tick.** `g_tick - k.last_tick` on u64
          reads as an enormous gap after a replay scrub seek (`TOOLING.md` §9.3.10) or
          `tl_probe_test_set_tick`, so the throttle became a no-op. Clamped. Answering the standing
          question: with the tick pinned at 0 the throttle is **not** a no-op - the first call rows
          and every later call with `n > 0` is suppressed forever. That is now in the contract
          block, which previously said only "not meaningfully testable yet".
      11. **(Low) the marker contract was pinned by `strstr(buf, "TL_FATAL")` alone.** The runner
          lane is building `TL_TEST_EXPECT_FATAL` against the full string, so a change to `origin=`
          or the separators would have broken them at merge and not here. Now pinned end to end,
          with a `TL_CHECK` trigger proving `origin` varies while the leading token does not. Found
          while doing it: the child's stderr is a text-mode stream, so Windows writes CR+LF where
          `crash.cpp` writes LF - whoever implements the macro has to match the line ending, not
          the two literal bytes.
      Checked and dismissed, so nobody re-opens them: `ProbeKey` omits `TOOLING.md` §9.2's trailing
      `_pad0[5]`, but `CPP-SUBSET.md` §1's explicit-padding rule is scoped to *hashed* state and the
      probe table is never hashed; `probe.cpp` compiles to nothing outside `TL_DEV` while
      `tl_probe.h` declares the functions unconditionally, which is deliberate (a stray call site is
      a link error, not silent absence); `tl_log_write`'s definition carries C linkage from the
      header's prior `extern "C"` declaration, which is correct C++ and not a mismatch.
      **Merge note:** `w1-tooling-rt` merges cleanly into `main` as of `e2f4b17`
      (`git merge-tree --write-tree` reports no conflict); `TODO.md` is a both-sides edit that
      resolves.
- [ ] **`TL_LOG_MIN` for `netcode` vs `ship` is a live tension between two docs, not resolved by
      the fix above. Ruling request.** `TOOLING.md` §9 wants the two tiers to differ (2 vs 3);
      `BUILD.md` §3 wants them to differ only by stripping, and `tools/audit/tier_parity.py`
      enforces the allowed define list. Deriving `TL_LOG_MIN` inside `tl_log.h` from the tier
      markers satisfies both gates *today* because no `-D` changes - but the two tiers really do
      compile different code, which is the thing §3's rule exists to prevent. It is defensible (a
      log level is stripping-class, exactly like `NDEBUG`) and it is not ruled. Either (a) add
      `TL_LOG_MIN=\d` to `tier_parity.py`'s allowed row and pass it as a `-D` from
      `cmake/tier.cmake` - explicit, gated, one home; or (b) state in `BUILD.md` §3 that
      tier-marker-derived divergence inside headers is in scope for the rule and name `TL_LOG_MIN`
      as its first instance. Not invented here (CLAUDE.md rule 7).
      **RULED 2026-08-24 (Rafael, option (c) - neither of the above): netcode and ship share ONE
      compiled floor, INFO+ (TL_LOG_MIN = 2 in both).** The tension dissolves instead of being
      exempted: BUILD.md §3's parity stays absolute, tier_parity.py stays untouched, and ship
      quiets further at RUNTIME via the log-level cvar (TOOLING.md §3 - a non-SIM cvar).
      Implementation (W1 ruling-closeout lane): tl_log.h's derivation makes both tiers 2;
      TOOLING.md §9/§7b table and CPP-SUBSET.md §7b's TL_LOG row change "INFO+ (ship: WARN+)"
      to "INFO+ (both; ship quiets via cvar)"; the compile-out test's per-tier arms follow.

- [x] **fx tests that need the runner lane** (`TESTING.md` §9.1 `TL_TEST_EXPECT_FATAL`) - landed
      in `tests/foundation/fx_fatal.test.cpp` (W1 runner+driver lane, 2026-08-24): `div` by zero,
      `sqrt` of a negative, `normalize` of a zero vector, `atan2(0,0)`, `to<R>` out of range,
      `clamp` with lo > hi, each `TL_TEST_EXPECT_FATAL` and gated `#if TL_DEV`. Outside dev,
      where `TL_ASSERT` compiles out, each body **`TL_SKIP`s with its reason** and reports SKIP -
      it was an empty body reporting PASS until the review, six rows of green having executed
      nothing (`fx_review_release_error_values` in `fx_review.test.cpp` covers the returned
      release values on those tiers, for five of the six - see the `to<R>` item below). The
      runner judges these as fatal-expected only under `TL_DEV` (`tl_child_verdict`,
      `tests/runner/runner_core.h`), else as ordinary children, so the row is never a false fatal
      expectation outside dev. The fatal check itself is still the loose one - see "Tighten
      `TL_TEST_EXPECT_FATAL` to the real contract" below for the exact string, exit code and
      prerequisites.
- [ ] **RR-1 Pi 4 sysroot + the aarch64 leg of `BUILD.md` §10.5.** Rafael has a Pi 4 on the LAN,
      so this is now an execution task, not a decision. Lane: W0 skeleton (**Opus 5 high**). It
      touches only `toolchain/`, `cmake/toolchain-pi4.cmake`, `tools/sysroot.sh|deploy.sh` and this
      file, so it does not collide with the audit/fingerprint code under adversarial review.

      *Prerequisites, from Rafael, before anything runs:*
      1. the host string (`user@host` or `user@ip`);
      2. **key-based SSH already working** — the agent shell is non-interactive, so a password
         prompt hangs rather than prompts. `id_ed25519` exists on the dev PC; if it is not on the
         Pi yet: `ssh-copy-id -i ~/.ssh/id_ed25519.pub <user>@<host>` (needs a password once);
      3. **confirmation the Pi runs a 64-bit OS** — paste
         `ssh <user>@<host> "uname -m; head -2 /etc/os-release; df -h / | tail -1; which tar gcc"`.
         `uname -m` must read **`aarch64`**. Many Pi 4s run 32-bit Raspberry Pi OS (`armv7l`), and
         `cmake/toolchain-pi4.cmake`, `CANON.md` and `NETCODE.md` all specify `aarch64-linux-gnu`;
         a 32-bit Pi is a different ABI and a different determinism target, so that outcome is a
         **new ruling request, not a quiet retarget**.

      *Known change required first:* `tools/sysroot.sh` uses `rsync`, which is **not installed on
      the dev PC** (checked 2026-08-22: `ssh`, `scp`, `tar` present; `rsync` absent). Rewrite it as
      tar-over-ssh — one stream, needs nothing on the Pi but `tar`, and it matches R-3's wording
      exactly ("a tarball of the Pi's `/usr/include`, `/usr/lib`, `/lib`"). Expect ~200–600 MB.

      *Then:* capture the tarball, pin its BLAKE2b in `toolchain/VERSIONS` (`sysroot_pi4`),
      `cmake --preset netcode-pi4 -DTL_SYSROOT=...`, build, `tools/deploy.sh netcode-pi4 <host>`,
      and run `tl_tests --tag smoke` on the Pi. Record the result in `BUILD.md` §10.5.

      *What it closes and what it does not:* it closes the **local** half of §10.5 — cross-compile
      against a pinned sysroot, deploy, smoke tests green on aarch64 hardware. It does **not**
      un-gate the `cross-pi4` PR job, which needs the tarball at a URL CI can `GET` (R-3's "release
      bucket"); no bucket exists. So RR-1 then shrinks to "publish the sysroot tarball somewhere
      CI can fetch it, set `TL_SYSROOT_URL`", and RR-2 stays as written. The commit says exactly
      that rather than marking §10.5 fully met.

      *Still blocked on the above:* the cross-ISA nightly (`docs/TESTING.md` §4) and Gate 0's G-06
      run on the Pi (`docs/GATE0-BENCH.md`) — G-06 is a Gate 0 scenario, not a nice-to-have.
- [ ] **RR-2 `nightly.yml` / `weekly.yml` (`docs/BUILD.md` §10.4).** Both need self-hosted `pi4`
      and `deck` runners; committing them before the runners exist buys a nightly red build.
      Land them with RR-1.
- [ ] **RR-4 (b) is BUILT** (`tools/audit/targets.py`, `tl_audit_targets`, PR lane). Every sim TU
      is preprocessed and its record layouts dumped for `x86_64-pc-windows-msvc`,
      `x86_64-unknown-linux-gnu` and `aarch64-unknown-linux-gnu`, then diffed. Measured: 0
      divergences on the real tree, ~75 ms per triple per TU, no sysroot (freestanding headers come
      from clang's resource dir; `<string.h>` is stubbed with the four declarations `CPP-SUBSET.md`
      §1 allows). Selftest fixtures prove it catches `[[no_unique_address]]`, `#pragma pack` +
      `alignas`, bit-fields and a `#ifdef __GNUC__` branch - four constructs no regex caught - and
      does not fire on ordinary sim code. **Remaining from RR-4: (a)**, the libclang contract
      scanner, still open below; and the value-divergence classes stay with the token bans by
      design (`char` signedness, `long`/`size_t` in an expression, wide literals, high escape
      bytes), which is the split the review's own attack recommended.
- [ ] **Coverage boundary of the cross-target gate, tied to RR-1.** `tools/audit/targets.py`
      measures the TUs under `src/sim` and the det half of `src/foundation`. A record instantiated
      only from `net`/`script`/`save` - a `TL_WIRE_STRUCT` template with a bit-field, say - is
      measured nowhere until those modules compile for aarch64, i.e. until RR-1. Stated in
      `CPP-SUBSET.md` §5; close it by extending the gate's TU set once a Pi build exists.
- [ ] **The contract-comment rule is at the limit of a regex.** Three reviews have now found
      false positives and false negatives in `tools/audit/includes.py`'s declaration scanner
      (operators, attributes, template heads, a `(` inside a literal). The token bans are fine as
      greps - they are line-local - but the contract rule wants a parser. If a fourth round finds
      another case, replace it with `clang -Xclang -ast-dump=json -fsyntax-only` over each public
      header, asking for `FunctionDecl`/`CXXMethodDecl` without an attached comment (the include
      paths are already in `compile_commands.json`). Recorded as the escalation, not done on spec.
- [ ] Assert the audited-layer ORDER equals the module DAG. `cmake/audit.cmake` gets the layers in
      `add_subdirectory` order, which is correct only because the root `CMakeLists.txt` is hand-
      ordered; nothing checks it. A one-line comparison against a declared list would.
- [ ] `tools/audit/symbols.py` matches allowlist patterns with `fnmatch`, which is case-folding on
      Windows hosts (`tl_fatal` also matches `TL_FATAL`). Harmless today; use `fnmatchcase`.
- [ ] **Turn `TL_STRICT_TOOLCHAIN` back on in CI.** Since R-8 the compiler is not in `build_id`,
      so the pin check is the only thing keeping peers on one clang (`docs/BUILD.md` §9 R-7). It is
      fatal by default in `netcode`/`ship`, and `pr.yml` opts out with `-DTL_STRICT_TOOLCHAIN=OFF`
      because the runners carry stock clang. Install the pinned LLVM major on the runners
      (apt.llvm.org for ubuntu, choco/winget for windows) and delete the opt-out.
- [ ] The ubuntu-clang half of `pr.yml` is written against the non-MSVC flag path but has only
      been exercised through the GNU driver locally (`clang++` on the real sources, clean under
      `-Werror`); the first PR run is its real proof. The runners also carry stock clang, not the
      pinned major — turn on `TL_STRICT_TOOLCHAIN` in CI once the pinned LLVM is installed there
      (`BUILD.md` §9 R-7).
- [ ] `out/luac/manifest.tsv` is shared across presets, so a stale manifest could feed one preset's
      `build_id` from another's bytecode. Move it under the preset's binary dir when `tools/luauc`
      lands (W2 luau-vm lane).

## Assay (repurposed shakedown of the new stack — timing flexible, shares no code with Gate 0)
- [ ] A jam-scale C++/Luau/SDL probe, no engine; deliverable = one 15-second clip a stranger can
      read (the commercial-thesis gate). `PIVOT-DESIGN.md` §10.

## W1 fx - NEXT: the adversarial review, then what the other lanes inherit (2026-08-23)
- [x] **Adversarial review of W1 fx (`3a35976`..`a45ae57`) - DONE 2026-08-23 (Fable 5, fresh
      context), fixes in `cc104f2` (review 1), `a470abb`+`b83d9af` (review 2), `ad38ed6` (review 3).
      **Verdict: fix first -> shipped.** The arithmetic core (`rne_shr`/`rne_div`/`mul`/`div`/
      `mul_int`/`to`, `isqrt`, `sincos`, `atan2`, the vec2 helpers) survived every attack: the
      executed INT32_MIN / 2^62 / 2^63 edge matrix, the sanitized run (ubuntu clang 18, ASan+UBSan
      no-recover: zero findings in `src/`), both trace pins on dev/debug/netcode/ship-win + Linux,
      and every recorded deviation re-derived (RNE Horner steps are sign-symmetric by
      construction and measured; the `+-ONE` clamp is forced by `sum(SIN_POLY4) == 2^30 + 1`,
      checked by hand; the exact-division atan ratio is determinism-neutral and atan2 is off the
      hot path; the format-keyed op table is RR-5; `mul_int`'s rescale is an exact 2-bit widen /
      RNE 10-bit narrow, tested; the three R-3 symbols match `TOOLING.md` §9). The defects were
      around it. Ranked (all fixed unless marked):
      1. **High** `fx_float.h`: the `1.5*2^23` rint magic is exact only for `|s| <= 2^22`; every
         odd quantum in `[2^22, 2^23)` (pos_t 16..32 m, q_t 2^-8..2^-7) quantised to the even
         neighbour, and the round-trip test's tolerance of 1 ulp hid exactly that. Non-sim, but
         `from_f32_quantized` is the INPUT/editor write path.
      2. **High (gate)** the `sanitize-linux` lane could not fail: UBSan recovers by default, and
         run 32645441509 printed `det_atan2.test.cpp:78: signed integer overflow` and went green.
         `-fno-sanitize-recover=all` (`cmake/tier.cmake` - outside the review's file list, edited
         because the brief named the UBSan configuration; one line, CI-proven).
      3. **High (gate)** `includes.py` passed `decltype(1.0) x`, `auto y = 0x1p3`, `k * 1e3`,
         `_Float16`, `decltype(1L) x` in a sim TU - floats and longs with no banned token, and a
         local never reaches the layout gate. Floating literals, the `_Float*`/`__fp16` family and
         `L`/`UL` suffixes are banned now (7 negative + 1 clean fixture; 0 hits on the tree).
      4. **Medium** `atan2` contract `(-1/2, 1/2]` was false: `y < 0, |y|/|x| < ~2^-31` returns
         `-HALF_TURN` (the lane's own test pinned it). Header + §10.3 now say closed `[-1/2, 1/2]`.
      5. **Medium** `to<R>` / `mul_int` widening precondition `2^(62-D)` rejected the top bit that
         fits (`2^(63-D)`): the identity `to<fx<i64,F>>` on INT64_MIN asserted.
      6. **Medium** no test anywhere for the documented release-tier error values (div by 0,
         sqrt < 0, rsqrt(0), atan2(0,0), NaN/inf quantisation) - now `fx_review.test.cpp`.
      7. **Medium** trace A exercised half the helpers (no `to` widening, no `sqrt` at S=18/30, no
         `rsqrt`/`lerp`/`dot`/`cross`/`len`, no narrowing `mul_int`, no sat tier, div only below
         1/2): its pin proved less than its name. Trace B (own pin `0x14179b6d064d0ca6`) covers them.
      8. **Low** property skip counts summed across 16 rows (a vacuous row could hide). Per row now.
      9. **Low** `det_atan2.test.cpp:78` `INT32_MAX - 1 + 3` (test-code UB; the item-2 report).
      10. **Low** `FX-PALETTE.md` §4.2 still spelled `isqrt<R>(u64)`; §10.3 is the home.
      11. **Low, OPEN -> ruling (b) below** §3.1's `div<lambda_t>` over the `fx<i64,30>` XPBD
          denominator contradicts §10.1's 32-bit-only `div` - the same class as the magnetism line.
      Not attacked (waits for its lane): the Pi leg of both pins (RR-1); the fatal-expected halves
      (runner lane). Wart, not a defect: `fx_int<R>(-2^INT_BITS)` is representable but asserts
      (the contract says `|i| < 2^INT_BITS`; symmetric and documented, left as is).
- [ ] **Cross-ISA half of `FX-PALETTE.md` §10.6 is open until RR-1**: `fx_trace_hash_pinned`
      reproduces `0x1a1803512f224fad` on clang-cl (dev/debug/netcode/ship) and is in the PR lane
      for ubuntu clang; the Pi leg runs the same test the day a Pi build exists. A mismatch
      there is UB until proven otherwise (`TESTING.md` §4).
- [ ] `tools/fxcheck` is not in CI yet: it is a separate CMake project (`cmake -S tools -B
      out/tools`), ~4 min in full. Add a nightly step (`fxcheck` + `oracle.py check-coeffs` +
      `oracle.py verify worst.tsv`) with RR-2's `nightly.yml`; `--quick` (~3 s) could sit in the
      PR lane today.
- [x] `tests/foundation/fx_test_util.h` carried a local splitmix64 - it now calls `rng.h`'s
      `mix64` (same mix, so the pinned trace hashes did not move; both fx_trace tests still pass).
      (W1 rng/hash.)
- [ ] **For alloy-substrate / alloy-solver (W2/W3) - three facts the headers state and the
      ALLOY pseudocode does not yet respect:**
      (a) `len2_wide`/`dot<pos2_wide_t>` saturate and assert when `|d|^2 >= 2^63` raw, i.e.
      `|d| >= 11,585 m` - a world-diagonal pair overflows Q36. Every pair the broadphase hands
      the solver is inside a kernel radius, so this is only a precondition to STATE in
      `ALLOY.md` §14.3, but a debug `len2` over an arbitrary pair (ray queries, far-field
      magnetism) must not call it.
      (b) RULED 2026-08-23 (`FX-PALETTE.md` §9 R-6): the two i64 quotients (the XPBD
      denominator, the magnetism ratio) are one `rne_div` on the raw bits at the site, RNE, no
      new helper; `ALLOY.md` §14.4.3's `(num << 16) / den` (truncating) and the circuit solve's
      `floor_div` were rewritten to `rne_div` in the same edit, and `GATE0-BENCH.md` §8's
      `div<lambda_t>` with them. Left for the alloy-fields lane: the magnetism `|num| < 2^33`
      raw bound is a validator claim to assert, and ALLOY §14.4's F1/F2/F3/buoyancy lines still
      spell `i64(x.v) << 16` on signed values (UB by `CPP-SUBSET.md` §5: multiply by 65536).
      (c) `normalize(vec2<pos_t>)` is bounded by the INPUT's quantisation: `|u|^2 - 1` is
      within `2^30/|d| + 4` ulp, so a contact normal from a 4-quantum difference is a 25%
      vector. The SDF gradient path (`ALLOY.md` §3.2, R-2 of `FX-PALETTE.md` §9) normalises
      differences of sampled i16 distances, not positions - check that its inputs are long
      enough (>= a texel, 2^14 quanta, gives 1 part in 16k) before the first contact test.
- [ ] **For luau-bindings (W3):** the helpers live in `namespace fx` (`fx::mul`, `fx::sqrt`,
      `fx::sincos`); only the nine row aliases are global. `fx.vel_from_delta` is
      `mul_int<vel_t>`; `fx.div_q` is `div<q_t>(A, A)` and is only in the table for
      `pos_t/vel_t/q_t/scalar_t` formats; `fx.atan2` takes `pos_t`, `fx.atan2_q` takes `q_t`.

## W1 mem - notes and ruling requests (2026-08-24, w1-mem lane)
- [ ] **Ruling request: `VMemApi`'s definition needs one foundation-visible home.**
      `PLATFORM.md` §9.1/§9.2 define it in `platform/platform.h`, but foundation is a leaf
      (`ARCHITECTURE.md` §1 rule 1) and `vmem_arena.cpp` must call through the table, so it
      cannot include platform.h. Transcribed verbatim to **`foundation/vmem_api.h`** (the
      `foundation/atomic.h` precedent - owned by `JOBS.md`, lives in foundation). The platform
      lane should `#include "foundation/vmem_api.h"` from platform.h, not redefine the struct,
      and `PLATFORM.md` §9.1 needs the one-line doc fix (its owner's edit, not this lane's).
- [x] **`registry_hash_all` waited on w1-rng-hash** (`ROADMAP.md` §2 lists mem's dependency as
      "skeleton" only, but `MEMORY.md` §8.3 calls `tl_hash64` - the doc's dependency row was
      incomplete; note for the ROADMAP owner). Resolved 2026-08-24: w1-rng-hash's header commit
      merged into w1-mem, hash_all implemented, and the §8.8 done criteria are green
      (hash-region integrity, two-worlds-in-one-process equality over divergent dirt histories,
      mid-run restore reproducing the hash trace). No hash VALUES are pinned in mem tests
      (relative properties only).
- [ ] **Ruling request: §7 R-2's dev-tier `TL_LOG_WARN` cannot live in the det half** (the audit
      allowlist is closed to io - `CPP-SUBSET.md` §4/§9 R-3; `tl_log.h` also does not exist until
      tooling-rt lands). Implemented as: blob-cap overflow returns `ERR_MEM_RING_OVERFLOW` in dev
      tiers (TL_FATAL in netcode/ship per §8.3); the CALLER (the loop, non-det, W3) warns once and
      grows at the next barrier. `MEMORY.md` §7 R-2 should either bless this split or name the
      non-det home for the warn+grow.
- [ ] **Signatures added over the rev-1 spec are folded into `MEMORY.md` §8 in the same commit**
      (its lane's own doc): `ring_init`, `registry_set_fingerprint`, two-arg barrier guards,
      `alloc_shim.h`, `vmem_api.h`, mem `ErrCode`s, arena_guard's non-det placement - announce
      at the wave merge. Two rows belong to OTHER owners: `CPP-SUBSET.md` §7b's
      `TL_SCRATCH_SCOPE(s)` row should spell the shipped `_BEGIN`/`_END` pair, and `CANON.md`
      "Types" claims `NameHash` for `tl_types.h` (the alias now lives there; the `""_id`
      operator stays with w1-rng-hash's hash.h) - both are one-line owner edits.
- [ ] The `pool_alloc` CI grep (`MEMORY.md` §1.5/§8.6) is not built yet; when it lands it must
      exempt `mem_pool.h`'s own declarations alongside `mem_pool.cpp` and `vendor_glue/`.
- [ ] **Ruling request: the CRT-malloc COUNTER (`MEMORY.md` §2/§8.4) cannot exist under the
      writable-static gate.** One cumulative counter is one word of `.data`/`.bss`, and
      `CPP-SUBSET.md` §1's link gate bans writable static storage in EVERY `src/` lib
      (`tools/audit/symbols.py` checks `tl_foundation` too, `--data-only`) with no exemption
      mechanism. The same wall faces the tooling-rt lane (log sinks, profiler buffers). Shipped
      meanwhile: `operator new/delete` are stateless TL_FATAL tripwires in dev/netcode tiers;
      `tl_alloc_shim_install` returns `ERR_MEM_UNSUPPORTED` so the guard's zero delta is
      vacuous but HONEST. Options: (a) a per-object writable-static allowlist (R-3-style: one
      named u64 in `alloc_shim.cpp`, plus whatever tooling-rt needs), (b) drop the counter and
      lean on the symbol audit + pool hooks, (c) app-owned state reached through a global
      pointer (same gate problem). (a) is the recommendation; also note the dev tier links the
      RELEASE CRT (`/MD`), so `_CrtSetAllocHook` is unavailable regardless - Windows counting
      needs a different interposition even after the ruling.

## W1 mem - the adversarial review (2026-08-24)
- [x] **Adversarial review of W1 mem (d69aadb..53e73ac + both merges) - DONE 2026-08-24
      (Fable 5 high, fresh context), fixes in reviews 1-3 on `w1-mem`. Verdict: fix first ->
      shipped.** What held up under attack: the alignment-gap zero-on-push fix is correct and
      its test constructs real dirty-reuse-then-realign (the dirt survives every tier); the
      snapshot/restore blob layout is symmetric, hashes cover `[base, used)` never capacity,
      the per-arena array bisects, restore-then-grow re-zeroes via the `high_water =
      max(high_water, used)` rule; the OS-backed test fixture leaks no address into hashed
      state (two worlds at different bases hash equal); registration order is a pure function
      of the app wiring's call order (single-threaded init, sealed - not timing); the two merge
      resolutions are clean (LESSONS/TODO text only; `registry_hash_all`'s `tl_hash64` use
      matches DETERMINISM section 4 token for token); mem_pool's 64K two-granule carve, header
      recovery at `p & ~0xFFFF`, budget-return-on-large-free and realloc matrix all verified
      against section 8.6. Ranked defects, all fixed:
      1. **High** `mem_pool.cpp` carve_aligned: the reserve check ignored arena_push's COMMIT
         rounding, so on a base that is only page-aligned (every mmap reserve on Linux/Pi -
         VirtualAlloc's 64K alignment is why no existing test could see it) a carve at the
         reserve edge TL_FATALed where mem_pool.h promises null. Measured with a misaligned-base
         fixture: pre-fix the test dies on the fatal trap. (review 1)
      2. **High (test vacuity)** `arena_reset_to` poisoned EVERY arena in dev, not just
         ARENA_POISON ones: both worlds' "divergent dirt histories" in the section 8.8
         two-worlds criterion became identical 0xDD before the identical ops ran, so the
         criterion passed even with zero-on-push deleted; the restore-trace test wrote every
         pushed byte at 16-alignment and could not catch it either. Poison is now flag-gated,
         the two-worlds test writes divergent dirt directly and diverges the histories
         structurally (restore vs decommit), and sim_step pushes odd-sized partially-written
         blocks so the mid-run-restore replay crosses rollback dirt through alignment gaps.
         (review 3)
      3. **Medium** `registry_seal` did not fold the sealed ids into `session_fingerprint`
         (MEMORY.md section 8.3 says it does): until the app's `registry_set_fingerprint`, the
         fingerprint was zero and restore accepted a snapshot from ANY same-count registry -
         "the fingerprint check IS the id check" was vacuous exactly when no fingerprint
         existed. Seal now writes a little-endian tl_hash64 fold of the id array; wrong id and
         wrong ORDER are both refused with no app fingerprint set. (review 2)
      4. **Low** `arena_push`: an `end` within one COMMIT_GRANULE of 2^64 wrapped
         `align_up(end, GRANULE)` to a small value and sailed past the over-reserve fatal;
         `end` is now checked against the reserve before the rounding. (review 3)
      5. **Low** `ring_push` stamped the new tick on a slot still holding the EVICTED
         snapshot's payload; between push and a failed/skipped `registry_snapshot`, `ring_find`
         served a lie. The claimed slot is invalidated until the fill succeeds; pinned. (rev. 3)
      6. **Low** the alloc-shim vacuity (honest ERR_MEM_UNSUPPORTED, counter reads 0) was
         stated in comments but invisible to a test reader; now pinned by
         `alloc_shim_vacuity_is_visible`, which the writable-static ruling must flip. (rev. 3)
      Edge tests added: tick-0 snapshot find/restore (tick 0 is NOT a sentinel - the
      invalidated-slot path keys on `count == 0`), empty-registry hash, MAX_ARENAS full-house
      round trip, restore from the oldest live slot after wrap, commit exactly at the reserve
      edge, zero-size push (already present). Deferred-fatal tests (over-reserve, add-after-seal,
      guard trips, netcode overflow) remain gated on the runner lane's `TL_TEST_EXPECT_FATAL`
      (TESTING.md section 9.1) - the per-file headers list them; re-check at the runner merge.
- [ ] **For the platform merge (recorded by the mem review): `src/foundation/handle.h` exists on
      BOTH w1-mem and w1-platform and the copies differ - mem's is canonical (MEMORY.md §3).**
      w1-platform's copy: `handle_make` shifts `gen` in u32 (silent truncation for any geometry
      past 32 bits where mem's widens to u64), `handle_gen` truncates `bits` to u32 BEFORE
      shifting (wrong for >32-bit handles), it lacks mem's `IDX_BITS >= 1 / <= 31 / sum <= 64`
      static_asserts, and it exposes `IDX_BITS_V/GEN_BITS_V` where mem's spells `IDX_BITS_N` +
      `rep` - any platform-side consumer of those names breaks when mem's copy wins. The merge
      must take mem's file whole and re-point platform consumers.
- [ ] **For the platform merge: `platform/platform.h` REDEFINES `struct VMemApi` instead of
      including `foundation/vmem_api.h`** (the already-filed ruling request above). Verified
      token-by-token 2026-08-24: all three copies (PLATFORM.md §9.2, vmem_api.h, platform.h)
      agree field-for-field today, so the fix is mechanical - delete platform.h's definition,
      include the foundation header - but until then any TU including both headers is an ODR
      violation waiting at the merge.
- [ ] **Ruling request: is `ARENA_HASHED` without `ARENA_SNAPSHOT` legal for MUTABLE state?**
      A hashed-but-not-snapshotted arena that mutates cannot be rolled back, so a mid-run
      restore CANNOT reproduce the hash trace (section 8.8) - a desync trap wired at
      registration, caught only weeks later. Legit use is immutable data (compiled tables,
      MEMORY.md §5). Options: (a) `registry_add` TL_FATALs on the combo and immutable tables
      get SNAPSHOT anyway (they're small; restore is a no-op-equivalent memcpy), (b) bless the
      combo for immutable arenas and state the mutation ban in §1.2. The registry test fixture
      documents the hazard in place; MEMORY.md §1.2/§5 should carry the ruling.
      **RULED 2026-08-24 (Rafael): option (a).** `registry_add` TL_FATALs on HASHED without
      SNAPSHOT; immutable tables take SNAPSHOT anyway (small, and a restore of never-mutated
      bytes is a no-op-equivalent memcpy). Implementation (W1 ruling-closeout lane): the fatal
      in registry_add + a fatal-expected test; MEMORY.md §1.2/§5 carry the ruling; the fixture's
      hazard note becomes a citation of it.
- [ ] **Ruling request (spec gap, behavior matches spec pseudocode): a reserve that is not a
      COMMIT_GRANULE multiple has an unusable tail** - `arena_push` TL_FATALs "over reserve"
      when `align_up(end, 64K) > reserved` even though `end <= reserved`, so the effective
      budget is `round_down(reserved, 64K)` and a sub-64K reserve can never push. Recommend:
      `vmem_arena_init` rounds the reserve up to COMMIT_GRANULE (address space is free) so the
      fatal coincides with the real budget; MEMORY.md §8.2's "rounded up to page" would change.
      Not improvised in the review - the shipped behavior is what §8.2's pseudocode spells.
      **RULED 2026-08-24 (Rafael): the review's recommendation, which is the stronger form of
      "the stated budget is the usable budget" - `vmem_arena_init` rounds the reserve UP to
      COMMIT_GRANULE** (address space is free; no byte of the requested budget is ever
      unusable, and the over-reserve fatal coincides with the real edge). Implementation (W1
      ruling-closeout lane): the init rounding + tests at a sub-64K and a non-multiple reserve;
      MEMORY.md §8.2's "rounded up to page" becomes "rounded up to COMMIT_GRANULE".

## W1 rng/hash - the adversarial review (2026-08-24)
- [x] **Adversarial review of W1 rng/hash (`8bdc6ee`) - DONE 2026-08-24 (Opus 5 high, fresh
      context), fixes in reviews 1-3 on `w1-rng-hash`. **Verdict: fix first -> shipped.** What
      held up under attack, measured not assumed: `vendor/rapidhash/rapidhash.h` is byte-identical
      to upstream `bc4b4baa` (the pin is a real commit SHA and is the `rapidhash_v3` tag; LICENSE
      identical too) and is included only from `hash.cpp`; `rng_for`/`mix64`/`K0`/the
      `(system_id << 32 | draw)` packing match `DETERMINISM.md` §3 token for token, and all seven
      `rng_for` goldens plus all five rapidhash goldens re-derive exactly in an independent Python
      implementation; `__SIZEOF_INT128__` is defined for all three triples (measured), so
      rapidhash's MSVC `_umul128` path really is dead code everywhere; `rng_q` is the top 30 bits
      in `[0, 1)`; `rng_below` is Lemire with the documented `~n/2^64` bias and is correct at
      `n = 1` and `n = 2^32-1`; the four `R`s `rng_range` admits are exactly `FX-PALETTE.md`
      §3.1's `q_t x row` lines; the `fx_test_util.h` splitmix replacement is the same mix and both
      pinned fx trace hashes still pass (run, not trusted: 65/65 green before the review's fixes).
      Ranked defects, all fixed:
      1. **High (gate)** `tools/audit/targets.py`: `-U_MSC_VER` in `BASE_FLAGS` did not remove the
         `<intrin.h>` noise, it removed the gate's ability to see the one platform macro clang
         predefines for a triple we ship. Measured: a `src/sim` TU with `#ifdef _MSC_VER` around
         two different structs passed with "0 divergences", both legs blind. Fixed by stubbing
         `<intrin.h>` in the temp include dir (the mechanism `<string.h>` already uses) and
         leaving `_MSC_VER` defined - `hash.cpp` still reports 0 divergences and the planted TU
         now fails both the preprocess and the layout leg.
      2. **High (gate)** no negative fixture landed with the gate change, against the standing
         rule. `tools/audit/selftest.py` gains two: a `_MSC_VER` two-programs case (fails as it
         must) and a vendor-shaped `#include <intrin.h>` case (clean, so the false positive the
         flag was reaching for stays fixed). The existing `__GNUC__` case did not cover this -
         `__GNUC__` is undefined for the win triple, so it never exercised the win-vs-linux leg.
      3. **High** `rng.h`'s `rng_range` had no preconditions: `hi - lo` is the row's WRAPPING
         subtract, so `rng_range<pos_t>(r, -WORLD_HALF, WORLD_HALF)` - a uniform world position,
         the most obvious call there is - wrapped the span to `INT32_MIN` and returned positions
         thousands of metres outside the world, silently, in a dev build with asserts on
         (measured: raw 2124193170 = +8103 m for a +-4096 m range). Now two `TL_ASSERT`s.
      4. **Medium** `rng_range` is CLOSED at both ends and neither the header, `DETERMINISM.md`
         §3, nor the test said so: `rng_q` maxes at `1 - 2^-30` and `mul<R>` rounds RNE, so any
         span under 2^29 raw units rounds its top draw to exactly `hi` (measured:
         `rng_range<scalar_t>(~0, -10, +10) == +10`). The test asserted `<= hi` and therefore
         encoded the behaviour without documenting it. Contract stated in both places; the
         endpoint is now pinned by `rng_range_closed_at_both_ends`.
      5. **Medium** the goldens were circular: both sets were "computed from the pinned
         implementation", i.e. proof that the code equals itself, and `DETERMINISM.md` §9.5's
         "from upstream" names a source that does not exist (rapidhash ships no vectors at the
         pin). `tools/rapidhash_ref.py` now re-derives both families from the algorithm in Python
         (`--check`); every vector agrees, and §9.5 says where goldens come from.
      6. **Medium** `DETERMINISM.md` §9.5 asks for `rng_below` uniformity over 2^24 draws within
         0.5%; the lane shipped 2^16 within 10% - 256x fewer draws at 20x the tolerance. Now
         2^24 at 0.5% for `n = 8` and `n = 5` (the non-power-of-two, where Lemire's bias lives),
         counted per `n`, 305 ms.
      7. **Medium** the `u8` cast in `fnv1a64` is the whole cross-ISA claim of `NameHash` and had
         no test. `name_hash_high_bytes_are_unsigned` pins the unsigned reading of a 5-byte input
         with three bytes >= 0x80; deleting the cast gives `0xd05320c608f3293b` on this
         signed-`char` host and fails.
      8. **Low** `[docs:none]` was wrong: the gate edit silently falsified `TESTING.md` §5 ("a
         per-target `#if` ... in any spelling") and `CPP-SUBSET.md` §5's matching claim. Fixing
         the gate rather than the docs is what makes those sentences true again, so no wording
         change was needed - but the commit could not have known that without checking.
      9. **Low** thin edge coverage: `carrier_id = 2^64-1`, `n = 2^32-1`, and the
         `(system_id, draw)` packing's injectivity at the field boundary were all untested.
         `rng_edge_matrix` and `rng_for_system_id_draw_packing_is_injective` cover them.
- [x] **Ruling (2026-08-24, Rafael): `system_id == 0` is RESERVED and is a precondition.**
      `rng_for` gains `TL_ASSERT(system_id != 0)` (fail-loud, compiled out in netcode/ship like
      every `TL_ASSERT`); the six goldens that used `system_id` 0 were recomputed on a real enum
      id (`RNG_SYS_LUAU_BASE`) by `tools/rapidhash_ref.py`, independently, never by re-running the
      header; the ruling's home is `DETERMINISM.md` §3 and `rng_systems.h` cites it. The other
      option on the table - reading the reservation as registration policy only and softening the
      header's wording - was rejected: it would have left a default-initialised `system_id`
      aliasing whatever registration puts first.

## Foundation week(s) (`docs/MEMORY.md`, `CONTAINERS.md`, `DETERMINISM.md`, `TESTING.md`)
- [x] Finish the test runner (`tests/runner` — W1 runner+driver lane, 2026-08-24; **3 review
      commits**, 2026-08-24): `NEAR_FX`, `SPAN_EQ`/`MEM_EQ`, `TL_TEST_EXPECT_FATAL` (always via a
      child process, isolate or not), the parallel `--isolate` pool (one child per test,
      `core_count` workers, sorted deterministic replay of failures), property-test seeding
      (`tl_seed_for(global_seed, index)`), `TL_SKIP`, TSV + JUnit. `docs/TESTING.md` §9.1.
      **Adversarial review verdict (fresh context, 2026-08-24): FIX FIRST → fixed and shipped.**
      Five silent-pass paths and two untested-code findings; ranked list and the reasoning are in
      the `W1 runner review 1/2/3` commit messages, the residue is the three open items below.
      **Still not implemented, and now they say so:** `TL_ASSERT_NO_ALLOC` (needs `VMemArena`'s
      mark pair + `alloc_shim.cpp`, mem lane) and `TL_ASSERT_DETERMINISTIC` (needs a `World`)
      **refuse to compile** rather than pass — they shipped as no-op stubs that ran the statement
      and asserted nothing, which is the W0 "a gate that cannot fail" shape (`LESSONS.md`).
      Replace each body with the real check the day its dependency lands, and delete the
      `static_assert` (`tests/runner/tl_test.h`).
      **Ruling recorded**: the isolate pool's process-spawn + core-count primitives
      (`CreateProcess`/`fork`+`exec`, `GetSystemInfo`/`sysconf`) are read as covered by
      `docs/TESTING.md` §8 R-2's "printf-class io + clock + filesystem access" exemption, since
      the feature §9.1 specifies cannot exist without them; `tests/runner/tl_test.h`'s contract
      block states this. The POSIX (`fork`/`execv`/`waitpid`) leg is written to the same contract
      as the Windows leg but **untested on this machine** (Windows-only dev PC) — exercise it the
      first time the Linux PR lane runs `tl_tests --isolate`.
- [ ] **Tighten `TL_TEST_EXPECT_FATAL` to the real contract.** Today `tl_child_verdict`
      (`tests/runner/runner_core.h`) passes a fatal-expected row on "the child spawned and then
      terminated abnormally", which cannot tell an expected assert from a stack overflow, a
      missing DLL, or `kill -9`. Two prerequisites, then one mechanical edit — do not tighten
      before both, or every fatal-expected test fails by construction.
      **Update (W1 wave merge, 2026-08-24): prerequisite 1 is met** — the real tl_fatal is on
      main and the six fx_fatal rows failed by construction exactly as predicted (exit(2) is a
      NORMAL exit; the abnormal-exit predicate stopped matching). Interim edit landed:
      `tl_child_verdict` passes expect_fatal iff `exit_code == TL_EXIT_FATAL` (2); abnormal is
      now a FAIL. Remaining: prerequisite 2 (stderr capture) and the marker + file:line pinning
      below.
      1. **`tl_fatal` must be the real one.** This tree still links the trap stub
         (`src/foundation/tl_assert.cpp`, `__builtin_trap()`). The real writer already exists on
         `w1-tooling-rt` (`src/foundation/crash.cpp`, commit `1c894ce`) and emits, verbatim:
         `TL_FATAL origin=%s %s:%u: %s\n` to stderr, then `exit(2)`. `origin` is one of
         `TL_FATAL`/`TL_CHECK`/`TL_ASSERT`; the literal `TL_FATAL` prefix is fixed regardless of
         origin *precisely so these tests can grep one string*. `TL_FATAL_MARKER` in
         `tests/runner/tl_test.h` is that prefix, `"TL_FATAL origin="` — it read `"TL_FATAL:"`
         until review 1 compared the two lanes, and the colon would have made the "mechanical"
         tightening compile and then match nothing.
      2. **The runner must capture the child's stderr**, which it does not — it inherits the
         parent's handles, so the marker is unreachable and parallel children interleave onto the
         console. `docs/TESTING.md` §9.1 already says "exit code + **captured** stderr → status";
         this is the missing half. Windows: `CreatePipe` + `STARTF_USESTDHANDLES` per child, drain
         before `WaitForMultipleObjects` returns the slot. POSIX: `pipe()` + `dup2` in the child.
      Then the edit is: `expect_fatal && dev_tier` passes iff `cr.exit_code == 2` **and** the
      captured stderr contains `TL_FATAL_MARKER`, **and** the marker's `file:line` is the one the
      test names — a fatal-expected test asserts WHICH assert fired, not merely that something
      did. That last part needs a per-test expectation: extend `TL_TEST_EXPECT_FATAL` to take the
      expected source file (`__FILE__` of the header the assert lives in) or a substring of the
      failing expression, and pin it in `tests/foundation/fx_fatal.test.cpp`'s six rows. Delete
      the KNOWN GAP notes in `tl_test.h`, `runner_core.h` and `fx_fatal.test.cpp` in the same
      commit. Cover it with a negative test: a child that exits 0, a child that exits 2 with no
      marker, and a child that exits 2 with the marker naming the WRONG file:line, must all FAIL.
- [ ] **Ruling request — a per-child timeout.** `docs/TESTING.md` is silent, and the runner has
      none: `WaitForMultipleObjects(..., INFINITE)` / `waitpid(pid, &status, 0)`
      (`tests/runner/main.cpp`). A test that hangs — or a fatal-expected test whose assert does
      not fire and whose body loops — stalls the PR lane forever with no output, and
      `LESSONS.md` already records one incident of a straggler being misread as a deadlock. The
      value is the decision: §6 budgets the PR lane at < 10 min total, but the `slow` exhaustive
      rows (2^30 iterations) run for minutes on their own, so one number cannot serve both.
      Proposal to rule on: `--timeout-ms`, default 0 (off) in a local run and set explicitly by
      each CI lane (PR: 120000; nightly: off), a timed-out child reported as its own `TIMEOUT`
      status that fails the run and names the test. Not improvised in this review, per CLAUDE.md
      ("silence in the spec is not permission").
      **RULED 2026-08-24 (Rafael): the proposal as filed.** `--timeout-ms`, default 0 (off)
      locally; the PR lane passes 120000 (it already runs `--tag !slow`, so one number serves),
      nightly runs without one; a timed-out child is its own TIMEOUT status, fails the run,
      names the test, and prints TESTING.md §6's P0-flake line. Implementation (W1
      ruling-closeout lane): the flag + both wait paths + a test with a deliberately hanging
      child; TESTING.md §9.1 gains the flag, §6 the two lane values.
- [ ] **`to<R>` out of range has no release-value test.** `tests/foundation/fx_fatal.test.cpp`
      pairs each dev-tier assert with `fx_review_release_error_values`'s netcode/ship value —
      for five of its six rows. `to<q_t>(fx_raw<pos_t>(1 << 19))` asserts in dev and, with the
      assert compiled out, narrows `2^31` through `i32` (well-defined wrap in C++20, so
      `INT32_MIN`: a positive value comes back as the most negative one). Nothing documents or
      tests that. Either give `to<R>` a documented out-of-range return in `src/foundation/fx.h`
      and a row in `fx_review_release_error_values`, or state in `fx.h` that the release
      behaviour is undefined-by-contract and callers must range-check — but not silence. Owner:
      the fx lane.
- [x] `tests/driver` skeleton (W1 runner+driver lane, 2026-08-24; parser extracted and tested in
      review 2): the full `--scene/--seed/--ticks/--workers|--workers-sweep/--record|--replay/
      --verify/--dual/--dump-probes/--csv/--snapshot-every/--ballast` contract of
      `docs/TESTING.md` §9.2, in `tests/driver/driver_args.h` (a closed `ErrCode` set + name
      table, `docs/CPP-SUBSET.md` §3) with `tests/driver/driver_args.test.cpp` over it. The boot
      itself (headless platform init, `app/wiring.cpp`'s scene load, the Script/Replay producer,
      `engine_tick_once`, CSV + hash output) is a named stub (`driver_boot_headless_STUB`,
      `tests/driver/main.cpp`) until the platform lane's headless impl and `app/wiring.cpp` land
      — a valid invocation today exits **70** (EX_SOFTWARE, the W0 stub convention), never the
      real contract's 0/3, so nothing downstream can mistake "not implemented" for "ran clean" or
      "diverged"; a malformed one exits 1. `tl_gate0`/`tl_hovel` stay on the W0 `stub_main.cpp`
      (their own lanes).
- [ ] **Property generators + the shrinker** (`docs/TESTING.md` §1: "a seeded `rng_key` generator
      + shrinking by halving the op sequence"). What the runner lane shipped is the *seed*:
      `tl_seed_for(--seed, row index)`, reproducible and now passed down to `--isolate` children.
      Nothing reads `TestCtx::seed` yet and there is no generator and no shrinker — the existing
      property tests roll their own. Build it on `rng_for` (`src/foundation/rng.h`, merged to
      main 2026-08-24) so a generator is keyed, not seeded ad hoc.
- [ ] **The isolate pool's fault paths have no test.** `tl_child_verdict`'s spawn-failure rule is
      covered (`runner_core.test.cpp`), and a mid-run `kill` of one child was verified by hand
      (pool drains, the row is FAIL, the other 76 still run, exit 1, the P0 flake line prints).
      What is still only a `LESSONS.md` line is the `WAIT_FAILED` drain and the absolute-cwd
      requirement that produced it — both need fault injection (a forced bad handle, a forced
      relative cwd) that the runner has no seam for. Add the seam when the pool next changes.
- [ ] Delete the W0 placeholder TUs as each module gets real sources (`src/*/…_unit`), and give
      `tl_driver` (replace `driver_boot_headless_STUB`) / `tl_gate0` / `tl_hovel` real mains.
- [ ] `platform/` contract + **headless impl** (file/clock/vmem/entropy/threads real; window/draw/
      events null). `docs/PLATFORM.md`.
- [x] **W1 platform - the contract header (`src/platform/platform.h`), 2026-08-24.** Every struct
      and the layout `static_assert`s from `PLATFORM.md` §9.2, transcribed verbatim; 0 violations
      on `tl_audit_includes` and a clean `/W4 /WX` standalone compile. Landed alongside it, because
      `platform.h` needs them and no lane owning them had started (same precedent as `tl_assert.h`
      landing from the fx lane, `LESSONS.md`): `foundation/handle.h` (MEMORY.md §3/§8.5,
      **superseded by mem's canonical copy at the 2026-08-24 merge below - was defective**:
      `handle_gen` truncated through `u32` before shifting, no `IDX_BITS`/`GEN_BITS` range
      asserts), `foundation/strview.h` (CONTAINERS.md §8.6), `foundation/ring.h`
      (CONTAINERS.md §8.5), `foundation/span.h` (CONTAINERS.md §1/§8.1), `foundation/rect.h`
      (RENDER2D.md §9.2, struct line only - no min/max/overlap helpers, nothing landing today
      needs them). `rect` added to `TL_FOUNDATION_NONDET` in `src/foundation/CMakeLists.txt`
      (it carries `f32`). The mem/containers/render2d lanes own these files outright the moment
      they start; a conflicting definition there wins over this stopgap.
      *(Renumbered from RR-7 on 2026-08-24: `w1-tooling-rt` filed its own RR-7 - the tooling
      plane's io/state exemption, now `CPP-SUBSET.md` §9 R-4 - on a branch that had not
      merged, so both lanes minted the same number off the same base. RR-7 is the tooling
      one; this is RR-8. Nothing else in the tree referenced this number. Wave merge: check
      the next free RR number against every open W1 branch, not just `main`.)*
- [x] **RR-8 CLOSED 2026-08-24: mem merged to main, W1 platform resumed and reconciled.**
      `main` merged into `w1-platform` (three conflicts: `LESSONS.md`/`src/foundation/CMakeLists.txt`
      kept both sides' additions plus `rect` folded into mem's `TL_FOUNDATION_NONDET` list;
      `src/foundation/handle.h` was add/add - **mem's canonical copy wins whole**, per the mem
      review finding below, this lane's copy is deleted). `platform.h` now `#include`s
      `foundation/vmem_api.h` (mem's foundation-visible home for the struct, `MEMORY.md` §8.2)
      instead of redefining `VMemApi` - the mem review's "triple copy" ODR risk is closed. Clean
      `tl_audit` (0 violations, 101/101 selftest) and a standalone `/W4 /WX` recompile of
      `platform.h` against the merged tree. `strview.h`/`ring.h`/`span.h`/`rect.h` are unchanged -
      containers hasn't started - and defer to containers' canonical copies the same way, at
      whichever merge lands second; not extended in the meantime.
      The real blocker RR-8 named - `VMemArena` being non-trivial, load-bearing state a second
      implementation shouldn't shadow - is now resolved by mem's real `vmem_arena.h`/`.cpp`
      existing on `main`; W1 platform's remaining build (`os_*_vmem.cpp`, `os_entropy.cpp`,
      `impl_headless/{init,file,clock,thread,vmem,entropy}.cpp`, the step-1 test set) proceeds
      against it directly rather than a stopgap.
- [ ] `VMemArena` + scratch + `ArenaRegistry` (hash-all, snapshot/restore, ring) + arena-offset guard
      + CRT counting shim. Two-worlds test from line one.
- [ ] `mem_pool` (vendor heaps only) + grep rule.
- [ ] Containers: `Array/Span`, `SlotMap+Handle` (gen-wrap quarantine), `Map`, `SortedMap/Set`,
      `RingBuffer`, `Bitset`, radix `sort_u32_kv/u64_kv`; `StrView`, interner, `fmt`. Rubric tests
      + two-instance determinism tests.
- [x] Keyed RNG (`rng_for/below/q/range`) + pinned rapidhash + `constexpr` FNV-1a `NameHash`.
      Vendor rapidhash. (W1 rng/hash.) The debug side-table (hash -> literal) is NOT built here:
      `CONTAINERS.md` §8.6 already rules it as the interner's job in dev tiers - foundation has no
      runtime table to register into without static mutable state (`CPP-SUBSET.md` §1 bans it
      engine-wide), so there is nothing for this lane to add beyond `operator""_id` itself.
- [ ] Determinism harness in the runner: `TL_ASSERT_DETERMINISTIC`, per-arena hash trace compare.
- [ ] Symbol audit + include firewall wired into CI against the det libs.

## ECS + reflection (`docs/ECS.md`, `FRAME-LOOP.md`)
- [ ] X-macro `TL_COMPONENT` + `FieldInfo`/kinds + static_asserts; `World`, columns (paged sparse
      set on VMem), entities, `world_get/column/entities`.
- [ ] Systems + `SystemDesc` + schedule build (topo-sort, tie-break, cycle fatal) + phases.
- [ ] Command buffer (record/apply at barrier; `GROWS_AT_BARRIER` window) + `EventQueue<T>`
      double-buffer + the end-of-tick barrier.
- [ ] Reflection encoder/decoder (name-keyed, alias, defaults) — round-trip tests; desync field-diff.
- [ ] Frame loop + time + `InputProducer` seam + Script producer + `RecordedInput` record/replay;
      the headless driver (`tests/driver`) with `--dual --replay --workers-sweep`.

## v0 — "the engine is alive" (`docs/RENDER2D.md`, `INPUT.md`, `ASSETS-AND-DATA.md`, `LUAU-LAYER.md`, `TOOLING.md`)
- [ ] Vendor SDL3, SDL_ttf, stb_image, stb_sprintf, Luau (`LUA_USE_LONGJMP`), Dear ImGui (docking).
      Allocator hooks → pools.
- [ ] `platform/impl_sdl3`: window, events ring, draw verbs (`RenderGeometry`, streaming textures,
      clip), present.
- [ ] Action map + Live producer (fold, edges, snorm8 quantization, pointer → `pos_t`).
- [ ] Assets: texture slotmap + stb_image loader; data-table compiler (schema = reflection table,
      fx literal conversion, validators, fingerprint hash); save format v1.
- [ ] Luau: three VMs, sandbox (sim VM library set, `sortedpairs`, frozen globals), `fx` bindings,
      `ecs.*` bindings (component declare, system register/trampoline, each/get proxy, commands),
      `input`/`events`/`data`/`log`; bytecode compile tool + fingerprint; reload command.
- [ ] Render2d: camera + `resolve_layout`, extract (fx→float, interpolation, snap bit), sort key +
      radix + batching, sprite system, immediate debug draw, layers WORLD/UI/DEBUG.
- [ ] ImGui shell: inspector (reflection walker → commands), console/cvars, log, profiler scopes +
      Chrome trace export, probes, replay scrub (Tier 0), crash pipeline.
- [ ] **v0 milestone:** window + moving sprite (Luau-declared component + Luau system) + fixed 60 Hz
      + clean exit + record→replay identical. ← the gate.
- [ ] Build fingerprint tool + init-time extension; fingerprint-stability CI test; rebuild-time
      budget measured.

## Hovel — 3 machines, integer lockstep (`docs/NETCODE.md` §18–§19)
- [ ] Vendor ENet + Monocypher (+ own crc32, little-endian writers, WIRE_STRUCT macro).
- [ ] Phase 1: `InputFrame` encoder/decoder (lossless delta), archive format, log retention ring,
      checkpoint writer. Phase 2: two-peer ENet, fixed coordinator, quorum fold, hash exchange.
- [ ] `tests/hovel/` throwaway sim (tile grid, pawns, regions via union-find, fx heat field),
      impairment shim, ballast, CSV metrics. Milestone A (PC + Deck + Pi, 1 h, zero divergence).
- [ ] Milestones B–E per `NETCODE.md` §19.5; S-01..S-15 scenarios; Milestone E = the 10 h soak.
- [ ] Hand the combat-design constraints (`AOE_ISLAND_LIMIT` = 4, min telegraph 6 ticks, `commit_ticks`)
      to the game design docs when a game repo exists (NAT is ruled: LAN/direct-IP v1, `NETCODE.md` §5.5).

## Alloy (`docs/ALLOY.md` — headless-first; its own build queue in "Gates & rulings ledger")
- [ ] **T-A-01 closure-scoped arena restore prototype — THE GATE for speculation.** Before any
      netcode Phase 3 work.
- [ ] Alloy test infra: conservation oracles, per-arena hash, run-twice, worker sweep, perf harness.
- [ ] Substrate → pass-5 topology core → solids → solver (promote the Gate 0 kernel if clean) →
      liquids/gases → fields → chemistry/fire/vegetation → AgentBody (+ `commit_ticks`) → Foundry
      wiring + the sim view (**Milestone 2**: dig/flood/melt a toy slice on screen).
- [ ] T-A-02 `v_max` validator · T-A-03 arena-set size · T-A-05 per-arena hash views · T-A-06
      island-merge telegraph.

## Job system (post-v0, before parallel Alloy — `docs/JOBS.md`)
- [ ] Atomic-counter pool, `parallel_for`/`parallel_levels`, per-worker scratch, chunk-tagged
      command/event merge; shuffle mode; 1/2/8/16 gate in CI.

## Reserved (design complete, build on first consumer — `docs/RESERVED-SEAMS.md`)
Audio · game UI (Luau) · spatial index · tilemap · nav/AI · frame animation · replay UI/cinematics ·
modding (Luau profiles) · game-logic substrate · streaming/cook · SDL_GPU path · editor shell.

## Doc debt
- [ ] `PIVOT-DESIGN.md` §12.3 doc-estate sweep: this repo's docs now supersede the foundry set;
      add a one-line "migrated 2026-08-22 → tidelock/docs" banner to each foundry doc (in the
      foundry repo) and retire `FOUNDRY-ORE-GATE.md`.
- [ ] After Gate 0: `FX-PALETTE.md` rev 2; after Hovel A: `NETCODE.md` §0 "assumptions carried"
      gets its first measured numbers.
