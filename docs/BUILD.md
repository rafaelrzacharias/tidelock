# Build — toolchain, build system, tiers, vendoring, the fingerprint (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §9. Expands `PIVOT-DESIGN.md` §11 and
> §8's fingerprint; the build system choice is new.

---

## 1. Toolchain (DECIDED — clang everywhere, ruled 2026-08-21)

| Target | Compiler | Notes |
|---|---|---|
| Windows x86-64 (dev PC) | **clang-cl** (LLVM release, pinned) | links the MSVC CRT; VS Build Tools / Windows SDK present |
| Linux x86-64 (Steam Deck) | **clang** (same major version) | built on the PC as a cross target or natively in distrobox |
| Linux aarch64 (Pi 4) | **clang `--target=aarch64-linux-gnu`** with a sysroot | cross-compiled from the PC; build once, deploy three |

One compiler family deletes MSVC-vs-clang codegen/UB behaviour as a determinism variable. MSVC
stays available as an occasional second-opinion build, never a peer. Pinned versions live in
`toolchain/VERSIONS` and are part of the fingerprint (§5). Fixed point makes codegen differences
unable to change results — except through UB, which is why sanitizers stay in the gate.

---

## 2. Build system (DECIDED — alternatives recorded)

| Option | Compile-time / stability | LOC & cognitive | Cross-compile | IDE / tooling | Verdict |
|---|---|---|---|---|---|
| **CMake + Ninja, presets, toolchain files** | unity builds via `UNITY_BUILD`; ccache-able; well-understood | ~300 lines of CMake for the whole tree | first-class (toolchain files per target) | clangd `compile_commands.json`, every IDE | **chosen** |
| hand-written `build.ninja` generator (a Python/Luau script) | fastest possible; zero CMake semantics | ~400 lines we own entirely | manual | compile_commands by hand | rejected — re-implements CMake's boring 80% to avoid its annoying 20% |
| `.bat`/`.sh` scripts invoking clang | none | small until cross-compile, then not | painful | none | rejected |

Layout: one `CMakeLists.txt` per module folder (a static lib each); `cmake/presets.json` with
`dev-win`, `netcode-win`, `ship-win`, `*-linux`, `*-pi4`; `cmake/toolchain-pi4.cmake`. Vendored
libs are separate targets compiled once (their own flags, `UNITY_BUILD` too) and cached.
`tools/` is its own CMake project (may use anything, including C++ and the STL).

Targets: `tl_foundation`, `tl_foundation_det` (the sim-safe half: fx, det math, rng, hash,
containers, arenas — audited), `tl_core`, `tl_sim` (audited), `tl_net`, `tl_render`,
`tl_platform_sdl3`, `tl_platform_headless`, `tl_editor` (dev only), `tl_script`; exes:
`tidelock` (the game host), `tl_tests`, `tl_driver`, `tl_gate0`, `tl_hovel`; tools: `fingerprint`,
`luauc`, `audit`, `fxcheck`.

---

## 3. Tiers and flags (DECIDED)

| Tier | Flags (on top of `CPP-SUBSET.md` §7) | Contents |
|---|---|---|
| `dev` | `-O1 -g`, all assert tiers, `TL_DEV=1` | editor, probes, float-shadow, impairment shim, Luau compiled on load |
| `debug` | `-O0 -g` (+ optional `-fsanitize=address,undefined`) | as dev; for stepping through sim code |
| `netcode` | `-O2 -g1`, slim fatal tier only, `TL_DEV=0` | what peers must match; Hovel and soaks run this |
| `ship` | as netcode + `-DNDEBUG`-class stripping; **no LTO** (§9 R-2 forbids it in every tier - this row used to say "optional") | the shipped binary |

`netcode` and `ship` have **different `build_id`s by design** - the tier name and `NDEBUG` are
both §5 inputs - so a session is homogeneous in tier and a `ship` peer cannot join a `netcode`
one. What must hold is that they differ by *nothing else*: `cmake/tier.cmake` builds both from one
set of variables and **`tools/audit/tier_parity.py` checks it** by diffing the resolved compile
command of every TU between the two presets, allowing only the tier markers and `NDEBUG` (a PR
job). Before the W0 review this paragraph claimed a parity the fingerprint tool "asserts", and
nothing anywhere asserted it.

**Compile time is a feature.** Budgets: full rebuild < 10 s, incremental < 2 s on the reference
PC — CI-measured, a regression fails the PR (`TESTING.md` §5). Levers in order: unity builds per
lib, prebuilt vendor libs, the include firewall (no STL anywhere in `src/` is the biggest single
win), `-gline-tables-only` in dev, ccache.

---

## 4. Vendoring (DECIDED)

`vendor/<name>/` at a pinned upstream commit recorded in `vendor/VERSIONS`; a `CMakeLists.txt`
per lib; its allocator hooked to a `mem_pool` (`MEMORY.md` §1.5); its headers never included above
its wrap module (CI grep). Adding a dep is a design decision with a line in `ARCHITECTURE.md` §1.
Current set: SDL3, SDL_ttf, Dear ImGui (+docking, ImGuiColorTextEdit), Luau (`LUA_USE_LONGJMP=1`),
ENet, stb_image, stb_sprintf, Monocypher, rapidhash. The vendored tree hashes into the fingerprint.

---

## 5. The two fingerprints (DECIDED — replaces build-hash + `@layout_hash` + `@fast_math` fingerprinting)

Two 256-bit BLAKE2b values with fixed names used identically in every doc:

| Name | Computed | Over |
|---|---|---|
| **`build_id`** | at build, by `tools/fingerprint.py`, embedded as `const u8 TL_BUILD_ID[32]` | **target-independent by construction (§9 R-8).** git tree hash of `src/` + `cmake/` + `CMakeLists.txt` + `CMakePresets.json` + `vendor/` + `script/sim/` + `script/lib/` + `toolchain/VERSIONS`, plus the **content** of every modified, untracked **or ignored** source under them (listed with `git status --porcelain`, which no git config alters - `git diff` output does) · the canonical compile tokens of the TUs under `src/`: **every token except a drop-list** of target-inherent and cosmetic ones (driver, triple, `-march`, include and output paths, warning/optimisation/debug flags, both driver spellings of one switch); an unrecognised token is hashed, so it surfaces as a loud mismatch rather than a silent hole · the tier name · `FX_PALETTE_REV` · the precompiled sim-script bytecode bytes (`script/sim/**` + `script/lib/**`) in load order |
| **`build_env`** | at build, alongside it, embedded as `const u8 TL_BUILD_ENV[32]` | the compiler id/version/target triple and the full resolved compile commands · everything `build_id` deliberately drops. **Reported, never compared** |
| **`session_fingerprint`** | at init, once the world is built (`app/` after all registrations) | `build_id` ‖ reflection field tables in registration order (name-hash, kind, offset, size per field; component name-hash per table) ‖ arena registry order (ids) ‖ action map (name-hash, kind, class per action in order) ‖ `hash(DataTables)` ‖ every `SIM`-flagged cvar value |

The define/`-std` input is what closes the bypasses the W0 review found: compile *definitions*, the
language standard and a `CXXFLAGS` environment variable are invisible to a flag string; edits to the
flag set itself are covered because `cmake/` is in the tree hash; and a `.gitignore`d `.cpp` is
compiled by the `CONFIGURE_DEPENDS` glob while being invisible to `git ls-files --exclude-standard`,
so the untracked scan no longer excludes ignored files. Each is a case in
`tools/audit/selftest.py`, which also asserts that a Windows and a Linux compile line over the same
tree produce the **same** `build_id`.

Residual, stated rather than hidden: an optimisation, warning or debug-info flag injected through
the environment changes `build_env` and not `build_id`. Under fixed point that cannot change a
result except through a compiler bug or UB in our code (§1), and the per-tick hash exchange
(`NETCODE.md` App. B `CHECKSUM_INTERVAL_TICKS`) bounds the damage to 30 ticks with `build_env` in
every CSV to name the odd peer out.

**Rules.** The handshake carries both (`NETCODE.md` §15.1); a mismatch on either ends the session
with a named diagnostic. Snapshots and the rollback ring are stamped with `session_fingerprint`;
saves and durable checkpoints are stamped with both (`ASSETS-AND-DATA.md` §5). In `dev`, script
reload and data reload recompute `session_fingerprint` and log old→new; in a lockstep session
both are refused. `tools/fingerprint` also emits `build_id` as text for CSV headers and soak
metadata.

---

## 6. Luau compilation

`tools/luauc` (links the vendored Luau compiler) compiles `script/sim/**/*.luau` and
`script/lib/**/*.luau` (library files reachable from sim scripts are fingerprinted) into `.luac`
with pinned options for `netcode`/`ship`; `dev` compiles on load with the same vendored compiler
(identical output). UI/editor scripts are always compiled on load (not in the fingerprint).

---

## 7. Cross-compile and deploy

`cmake --preset netcode-pi4 && cmake --build --preset netcode-pi4` → `out/pi4/bin/`;
`tools/deploy.sh pi4 <host>` scp's the binaries + `script/` + `assets/`; `tl_driver` and
`tl_hovel` run over SSH with their CSVs pulled back. The Deck is the same flow with the x86-64
Linux preset. The artifact is pinned per run (fingerprint in every CSV header); never repin while
a soak is in flight.

---

## 8. Repo rules (carried)

One feature per commit; test infra first; no commit without tests; commit **and push** every
time (two-PC sync); `.gitattributes` is the line-ending authority (`*.bat`/`*.cmd` CRLF, else
LF; `git config --local core.autocrlf false`); Rafael is sole author — no co-author trailers.
Durable context lives in committed files only (`docs/`, `TODO.md`, `LESSONS.md`).

---

## 9. Rulings (closed 2026-08-22 — nothing open)

- **R-1 The rebuild budget is met without a compiler cache.** ccache/sccache may be enabled
  locally as a convenience, but the CI measurement runs cold; a budget that only holds with a
  cache is not a budget.
- **R-2 No LTO in any tier.** It costs link time against the budget and has no measured win; it
  would also be a fingerprint input for nothing. Revisit only with a profile showing a cross-TU
  inlining loss in a hot sim loop — and then the fix is unity-build placement, not LTO.
- **R-3 Pi sysroot = a tarball of the Pi's `/usr/include`, `/usr/lib`, `/lib` captured by
  `tools/sysroot.sh`**, stored in a release bucket (not git), its hash pinned in
  `toolchain/VERSIONS`. The Deck uses the same mechanism with an x86-64 Linux sysroot.

- **R-4 The offline tools are Python, not C++, until one of them needs to link a vendored
  library.** `fingerprint`, `audit/*` and `rebuild_budget` are `tools/*.py`; `luauc` (links the
  Luau compiler) and `fxcheck` (links `fx.h` + MPFR) will be C++ in `tools/CMakeLists.txt`. This
  answers what `build_id` gets BLAKE2b-256 from: `hashlib`, not a second copy of Monocypher
  vendored a wave early — the fingerprint is computed offline and only its 32 bytes reach the
  binary, so nothing links a hash implementation for it. Alternatives rejected: a C++ tool with a
  reference BLAKE2b dropped into `tools/` (two implementations of one primitive in the repo, and
  the tools project would need building before the engine can configure); vendoring Monocypher in
  W0 (a lane editing another lane's module, `ROADMAP.md` §0 rule 2). Python is already a hard
  build dependency — `docaudit` is a PR gate.
- **R-5 Tiers replace CMake build types.** `CMAKE_BUILD_TYPE` is forced empty: `TL_TIER` owns
  optimisation, debug info and defines, and a `CMAKE_CXX_FLAGS_<CONFIG>` set that CMake appends
  behind our backs would silently change what peers compile. The tier flag string is a
  `build_id` input, so it must have exactly one source.
- **R-6 clang-cl takes the cl spellings of the subset's flags.** `/W4 /WX` for
  `-Wall -Wextra -Werror` (in cl mode `-Wall` means MSVC's `/Wall`, which clang maps to
  `-Weverything`), `/EHs-c-` for `-fno-exceptions`, `/GR-` for `-fno-rtti`,
  `/Zc:threadSafeInit-` for `-fno-threadsafe-statics`, `/clang:-nostdinc++` for `-nostdinc++`.
  The remaining `-W` and `-f` flags are identical in both driver modes. One warning set, two
  spellings — `cmake/tier.cmake` is the only place that knows the difference.

- **R-7 The toolchain pin is fatal in `netcode` and `ship`, a warning elsewhere.** Configuring
  with a clang whose major differs from `toolchain/VERSIONS` fails the tiers peers actually run
  and warns in the dev tiers. *Revised after R-8*: the original ruling made it a warning
  everywhere on the grounds that `build_id` carried the compiler string, so an off-pin peer could
  not silently join a session. R-8 removed the compiler from `build_id`, which voided that
  argument in the same document - this check is now the only thing keeping peers on one compiler.
  CI opts out explicitly with `-DTL_STRICT_TOOLCHAIN=OFF` because the runners carry stock clang;
  that opt-out is visible in `pr.yml` and queued in `TODO.md`, not a silent default.

- **R-8 `build_id` is target-independent; `build_env` carries the rest.** The W0 review found that
  putting the compiler string and the resolved compile commands into `build_id` made a mixed-target
  session impossible to hand-shake: `netcode-win`, `netcode-linux` and `netcode-pi4` differ by
  target triple and driver spelling alone, so `NETCODE.md` §19.5's Milestone A ("build once;
  one `build_id` for all three") and §15's "two peers on one release agree by construction" were
  both unreachable. Ruled: `build_id` covers only what can change a tick's bytes - source tree
  (including `cmake/`, so the flag set itself is covered portably), semantic defines, `-std`, tier,
  palette rev, bytecode - and peers refuse on it. Compiler, triple, optimisation level, warning and
  debug flags move to `build_env`, which is reported in CSVs, crash reports and soak metadata and
  never compared. This is the same premise Gate 0 exists to prove: under fixed point, codegen
  cannot change results except through UB **and except through target-variable language types** -
  `char` is signed on x86-64 and unsigned on aarch64, `long` is 32-bit on Windows and 64-bit on
  Linux, and either one diverges a PC peer from a Pi peer with no UB in sight. The second W0
  review found the premise stated without that clause and unenforced; `tools/audit/includes.py`
  now bans `char`, `long` and `wchar_t` in sim TUs (message literals keep `const char*`), and the
  ban is a selftest fixture. R-8 is only sound with that gate in place. Alternatives rejected: keeping `build_id`
  target-specific (coherent, but it deletes PC + Deck + Pi peers from the product, which is a scope
  decision, not a build one); a thin `build_id` of source tree + palette + bytecode only (a `dev`
  peer could then join a `netcode` session and be caught only by the tick-30 checksum, which
  abandons refuse-early for detect-late).

## 10. Implementation specification

### 10.1 Repository tree (build-relevant)

```
CMakeLists.txt                 project, options (TL_TIER, TL_SANITIZE), includes cmake/*.cmake, add_subdirectory per module
CMakePresets.json              the file CMake reads; two lines, `include`s cmake/presets.json (CMake only looks for presets at the repo root)
cmake/presets.json             dev-win · debug-win · netcode-win · ship-win · dev-linux · netcode-linux · sanitize-linux · netcode-deck · netcode-pi4 · ship-pi4; binaryDir = out/<preset>, binaries in out/<preset>/bin
.vscode/                       committed workspace config: CMake Tools on presets, clangd over .cache/compile_commands.json, LLDB launch configs, tasks (configure/build/test/audits); no solution files — VS Code + CMake presets is the IDE (ruled 2026-08-22)
cmake/tier.cmake               flag sets per tier (CPP-SUBSET.md §7, BUILD.md §3) as interface targets tl_flags_common / tl_flags_sim
cmake/toolchain-pi4.cmake      CMAKE_SYSTEM_NAME Linux, processor aarch64, clang --target=aarch64-linux-gnu, --sysroot=${TL_SYSROOT}
cmake/toolchain-deck.cmake     x86-64 Linux sysroot variant
cmake/audit.cmake              custom targets: tl_audit_symbols (llvm-nm), tl_audit_includes (grep), tl_audit_docs (tools/docaudit), tl_rebuild_budget
cmake/fingerprint.cmake        generates out/<preset>/generated/build_id.cpp from tools/fingerprint.py at configure+build
cmake/testlist.cmake           scans tests/**/*.test.cpp for TL_TEST( and generates test_list.inc (docs/TESTING.md §9.1)
src/<module>/CMakeLists.txt    one static lib each; PRIVATE include dirs; PUBLIC only the module's public header dir
vendor/CMakeLists.txt          SDL3 (subdir), SDL_ttf, imgui (sources listed), luau (VM + Compiler libs; LUA_USE_LONGJMP=1), enet, stb (one TU), monocypher, rapidhash (header)
tools/CMakeLists.txt           separate project; fingerprint, luauc, audit helpers, fxcheck (may use the STL)
tests/CMakeLists.txt           tl_tests (glob *.test.cpp → generated test_list.inc), tl_driver, tl_gate0, tl_hovel
toolchain/VERSIONS             clang version, SDL3 commit, Luau commit, … , sysroot hashes
```

### 10.2 Targets and link graph

`tl_foundation_det` (fx, det_math, rng, hash, containers, vmem_arena, registry, scratch, handle)
← nothing at compile time; at link time it references the panic ABI of `CPP-SUBSET.md` §9 R-3
(`tl_fatal`, `tl_check_failed`, `tl_assert_failed`), which `tl_foundation` defines and whose stems
are on the non-det list for that reason. The exe's link line resolves it; lld does so regardless
of archive-member order, and a GNU-ld target would need the cycle declared to CMake. `tl_foundation` (jobs, mem_pool, fmt, interner, atomic, alloc_shim) ← `det`.
`tl_sim` ← `tl_foundation_det` only. `tl_core` ← foundation, platform (headers). `tl_platform_sdl3`
/ `tl_platform_headless` ← foundation. `tl_render` ← core, `sim/views.h`. `tl_net` ← core,
enet, monocypher. `tl_script` ← core, luau. `tl_editor` ← core, render, imgui (dev only).
`tidelock` ← all. `tl_tests`/`tl_driver` ← foundation, core, sim, script, platform_headless (+
render/net when their tests are compiled in). `tl_sim` and `tl_foundation_det` compile with
`tl_flags_sim` (adds `-nostdinc++ -fno-builtin`, `-DTL_SIM_TU=1`), and are the symbol-audit inputs.

### 10.3 Scripts and tools

- `tools/fingerprint.py`: reads `toolchain/VERSIONS`, the tier's flag string (passed by CMake), `git
  rev-parse HEAD:src HEAD:vendor HEAD:script/sim HEAD:toolchain/VERSIONS` (tree hashes — a dirty
  tree appends the hash of `git diff` **and of every untracked file under those paths**, so a local
  build is still unique), `FX_PALETTE_REV` (parsed from `fx_palette.h`), and the `.luac` manifest;
  emits `build_id.cpp` and `build_id.txt`. A path that does not exist yet hashes in as an explicit
  `absent:<path>` token, never as nothing. Both outputs are rewritten only when the value changes,
  so a stable tree never relinks.
- `tools/luauc <out_dir> <in files…>`: `luau_compile` with `-O2 -g1`, writes `.luac` + `manifest.tsv`
  (`path, bytes, blake2b`); `--docs <dir>` emits the Luau binding reference pages (`CPP-SUBSET.md` §6).
- `tools/audit/symbols.py`: runs `llvm-nm --undefined-only -C` on each audited lib; allowlist in
  `tools/audit/allow.txt`; nonzero exit on any other symbol. `tools/audit/includes.py`: the grep
  rules of `CPP-SUBSET.md` §1/§4 + backend-header placement + the `float`/`double` token ban in
  sim TUs + `static` mutable + `thread_local` + `std::`.
- `tools/rebuild_budget.py`: clean configure+build of `netcode` tier with timing; then touch a sim
  TU and time the incremental build; compares to the budgets; CI artifact (TSV on stdout).
- `tools/audit/commit_docs.py`: the doc-touch gate — a commit changing `src/<module>/` must change
  that module's doc or say `[docs:none]` (CLAUDE.md doc-integrity protocol).
- `tools/audit/sysroot_hash.py`: verifies a downloaded sysroot tarball against the pin in
  `toolchain/VERSIONS` before any cross build uses it (R-3).
- `tools/audit/tier_parity.py`: the §3 netcode/ship flag-parity check, over `compile_commands.json`.
- `tools/audit/selftest.py`: the audits' own negative tests - every gate is run against a planted
  violation in a throwaway tree, plus the no-false-positive fixtures (prose about floats, a
  commented-out include, an all-inline template header) and the win/linux `build_id` equality
  check for R-8. A PR job and the `tl_audit_selftest` target.
- `tools/audit/symbols.py` takes `--layer NAME=PATH` **in DAG order** (a lib may reference only its
  own symbols and those of layers named before it) and `--data-only NAME=PATH` for the rest of
  `src/`; besides undefined symbols it fails on any object file with a non-empty `.data`/`.bss`,
  which is the only reliable catch for anonymous-namespace and `inline static` mutable globals.
- `tools/sysroot.sh <host>`: rsyncs `/usr/include /usr/lib /lib /usr/lib/gcc` from the Pi (or
  Deck) into a tarball; prints its BLAKE2b for `toolchain/VERSIONS`.
- `tools/deploy.sh <preset> <host>`: scp `out/<preset>/bin/*`, `script/`, `assets/`; prints the
  remote run line.

### 10.4 CI (GitHub Actions — `TESTING.md` §8 R-1)

`pr.yml`: matrix {windows-clang-cl, ubuntu-clang} × {dev, netcode} + ubuntu cross pi4 (build only)
→ audits → `tl_tests --isolate --junit` → driver harness jobs → sanitizer job (ubuntu, `-DTL_SANITIZE=ON`,
sim tests) → rebuild budget. `nightly.yml`: slow tests, fuzz, self-hosted `pi4` and `deck` runners
pulling the PR-built artifacts for cross-ISA replay-diff, save cross-build, pixel goldens.
`weekly.yml`: Hovel scenarios (when present), Gate 0 re-run on palette/solver changes.

Built so far: `pr.yml` with audits (doc, include firewall, header contracts, doc-touch, symbols),
the {windows, ubuntu} × {dev, netcode} build+test matrix, fingerprint stability, the sanitizer job
and the rebuild budget. Its `cross-pi4` job is gated on the repository variable `TL_SYSROOT_URL`
(R-3) and the harness jobs wait on `tl_driver`; `nightly.yml`/`weekly.yml` land with the
self-hosted Pi and Deck runners. `TODO.md` carries both.

### 10.5 Done criteria

An empty-tree configure+build passes every audit; `build_id.txt` is stable across two clean builds;
the pi4 preset produces an aarch64 ELF that runs `tl_tests --tag smoke` on the Pi via `deploy.sh`.

Met on Windows (2026-08-22, W0 skeleton, re-verified after the W0 adversarial review):
`debug/dev/netcode/ship-win` configure and build clean under `-Werror`; `tl_tests --tag smoke`
passes in-process and under `--isolate`; `build_id.txt` identical across two clean `netcode-win`
builds; rebuild budget 1.0 s full / 0.6 s incremental against the 10 s / 2 s ceilings; netcode/ship
compile-command parity clean.

Each gate is negative-tested against a planted violation, and the plants are the *adversarial* ones
the review used, not the obvious ones: `f32` (not `float`) in a sim TU; `float` in `src/sim/fmt.cpp`
(a basename that collides with a non-det foundation stem); an anonymous-namespace mutable global;
a sim TU including `net/wire.h` and `foundation/jobs.h`; `tl_foundation_det` referencing a `tl_sim`
symbol; a `/O1`-vs-`/O2` delta between netcode and ship; a `.gitignore`d `.cpp` under `src/`; a
`CXXFLAGS` injection. All eight are caught.

The pi4 leg is **unmet and blocked on hardware**: clang emits aarch64 ELF and the preset fails
loudly without `TL_SYSROOT`, but the R-3 sysroot tarball needs a live Pi. `TODO.md` carries it as a
ruling request.

*Rev 1 — 2026-08-22; §9 R-4..R-8 and §3/§5/§10.1/§10.3/§10.4/§10.5 reconciled with the W0 skeleton, its adversarial review and the R-8 ruling, 2026-08-22.*
