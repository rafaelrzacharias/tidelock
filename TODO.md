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
- [ ] `tests/foundation/fx_test_util.h` carries a local splitmix64 - replace with `rng_for`
      from the rng/hash lane when it lands (same mix; the seeds are arbitrary).
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

## Foundation week(s) (`docs/MEMORY.md`, `CONTAINERS.md`, `DETERMINISM.md`, `TESTING.md`)
- [ ] Finish the test runner (`tests/runner`; W0 shipped the stub — generated list, tags/filter,
      `--isolate` one child per test, TSV + JUnit): `NEAR_FX`, `SPAN_EQ`/`MEM_EQ`,
      `TL_TEST_EXPECT_FATAL` via child process, `TL_ASSERT_NO_ALLOC`, `TL_ASSERT_DETERMINISTIC`,
      the parallel isolate pool, property generators. `docs/TESTING.md` §9.1. **First.**
- [ ] Delete the W0 placeholder TUs as each module gets real sources (`src/*/…_unit`), and give
      `tl_driver` / `tl_gate0` / `tl_hovel` real mains (they exit 70 today).
- [ ] `platform/` contract + **headless impl** (file/clock/vmem/entropy/threads real; window/draw/
      events null). `docs/PLATFORM.md`.
- [ ] `VMemArena` + scratch + `ArenaRegistry` (hash-all, snapshot/restore, ring) + arena-offset guard
      + CRT counting shim. Two-worlds test from line one.
- [ ] `mem_pool` (vendor heaps only) + grep rule.
- [ ] Containers: `Array/Span`, `SlotMap+Handle` (gen-wrap quarantine), `Map`, `SortedMap/Set`,
      `RingBuffer`, `Bitset`, radix `sort_u32_kv/u64_kv`; `StrView`, interner, `fmt`. Rubric tests
      + two-instance determinism tests.
- [ ] Keyed RNG (`rng_for/below/q/range`) + pinned rapidhash + `constexpr` FNV-1a `NameHash` with the
      debug side-table. Vendor rapidhash.
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
