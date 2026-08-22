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
| `ship` | as netcode + `-DNDEBUG`-class stripping, LTO optional (LTO is *not* a determinism variable under fixed point, but it is a fingerprint input) | the shipped binary |

`netcode` and `ship` produce the same fingerprint class only if their flag sets are identical
except for symbol stripping — they are kept so, and the fingerprint tool asserts it.

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
| **`build_id`** | at build, by `tools/fingerprint`, embedded in a generated TU as `const u8 TL_BUILD_ID[32]` | compiler version string · the tier's full flag set · git tree hash of `src/` + `vendor/` + `script/sim/` + `script/lib/` + `toolchain/VERSIONS` · `FX_PALETTE_REV` · the precompiled sim-script bytecode bytes (`script/sim/**` + `script/lib/**`) in load order |
| **`session_fingerprint`** | at init, once the world is built (`app/` after all registrations) | `build_id` ‖ reflection field tables in registration order (name-hash, kind, offset, size per field; component name-hash per table) ‖ arena registry order (ids) ‖ action map (name-hash, kind, class per action in order) ‖ `hash(DataTables)` ‖ every `SIM`-flagged cvar value |

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
← nothing. `tl_foundation` (jobs, mem_pool, fmt, interner, atomic, alloc_shim) ← `det`.
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

Met on Windows (2026-08-22, W0 skeleton): `debug/dev/netcode/ship-win` configure and build clean
under `-Werror`; symbol, include-firewall, header-contract and doc audits green, each
negative-tested against a planted violation; `tl_tests --tag smoke` passes in-process and under
`--isolate`; `build_id.txt` identical across two clean `netcode-win` builds; rebuild budget 1.0 s
full / 0.6 s incremental against the 10 s / 2 s ceilings. The pi4 leg is **unmet and blocked on
hardware**: clang emits aarch64 ELF and the preset fails loudly without `TL_SYSROOT`, but the R-3
sysroot tarball needs a live Pi. `TODO.md` carries it as a ruling request.

*Rev 1 — 2026-08-22; §9 R-4..R-6 and §10.1/§10.3/§10.4/§10.5 reconciled with the W0 skeleton, 2026-08-22.*
