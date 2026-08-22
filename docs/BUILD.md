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

## 5. The build fingerprint (DECIDED — replaces build-hash + `@layout_hash` + `@fast_math` fingerprinting)

Two halves, one 64-bit + 256-bit pair:

**Build-time (embedded by `tools/fingerprint` into a generated TU):**
`BLAKE2b(compiler version string, full flag set per tier, git tree hash of src/ + vendor/ +
script/ + toolchain/VERSIONS, FX_PALETTE_REV, the precompiled sim-script bytecode bytes in load
order)`.

**Init-time extension (computed once the world is built):**
`BLAKE2b(build_fingerprint, reflection field tables in registration order (name-hash + kind +
offset), arena registry order, action map (names + kinds + classes), compiled data tables hash,
SIM-flagged cvar values)`.

The init-time value is what the handshake compares (`NETCODE.md` §15) and what snapshots and
checkpoints are stamped with. Two peers that differ in *any* input cannot join a session; a
snapshot from a different fingerprint cannot be restored. In dev, script reload and data reload
recompute the extension and log the change; in a lockstep session they are refused.

---

## 6. Luau compilation

`tools/luauc` (links the vendored Luau compiler) compiles `script/sim/**/*.luau` into `.luac`
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

*Rev 1 — 2026-08-22.*
