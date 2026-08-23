# tidelock — TODO (the to-do list only)

> **Parallelism:** this list is the serial queue inside each lane; which lanes run concurrently,
> and the critical path, is `docs/ROADMAP.md`. Start a wave by opening one worktree per lane.

Worked top to bottom; the first open `[ ]` is what to do next. History → `git log`; gotchas →
`LESSONS.md`; rationale → the doc named on each line. Governing rules: `CLAUDE.md` principles,
`docs/ARCHITECTURE.md` §0/§4, test-infra-first.

## Gate 0 — the pivot gate (`docs/GATE0-BENCH.md`, `docs/FX-PALETTE.md`)
- [ ] `src/foundation/fx.h` — `fx<Rep,FRAC>`, `mul<R>`/`div<R>` with RNE + widened intermediates,
      sat/wrap helpers, comparisons; exhaustive tests on small formats, property tests on 32-bit.
- [ ] `fx_palette.h` — the rev-1 rows, derivation `static_assert`s, mixed-op instantiations, world
      constants, `H`/`G_SUBSTEP`; `FX_PALETTE_REV`.
- [ ] `det_math.h` — `sqrt`/`rsqrt`/`sincos`/`atan2`/`isqrt`/`lerp`, `vec2<T>`, normalize/rotate;
      FixPointCS ports attributed; `tools/fxcheck/` three-layer oracle (exhaustive + differential +
      MPFR bounds) green for `sqrt`/`sin`/`cos`.
- [ ] `tests/gate0/` — disposable solver (gravity, rigid boxes, distance + contact + friction, PBF
      density), scenarios G-01..G-06, substep sweep 4/8/16, CSV + verdict lines, FLOAT-SHADOW config.
- [ ] Run on PC; cross-compile + run on Pi 4 (`docs/BUILD.md` §7); commit CSVs under
      `tests/gate0/results/`. Climb the ladder on any convergence failure (`FX-PALETTE.md` §3.2).
- [ ] **Decision commit:** `FX-PALETTE.md` rev 2 (rows DECIDED, or the fallback recorded) +
      `PIVOT-DESIGN.md` §3.1b/§12 updated + `LESSONS.md` entries per rung climbed.

## Ruling requests (filed, not improvised — CLAUDE.md rule 7)
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
