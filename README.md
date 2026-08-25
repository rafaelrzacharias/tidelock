# tidelock

> **Tidal locking**: two bodies held in perfect, permanent lockstep by mutual physical force.
> The name three ways: *lockstep* — 8 peers bit-exact by construction; *tides* — a matter sim
> of fluids, pressure, heat and reaction; and the body that locks us is the *moon* — **Lua**.

A 2D game engine, a deterministic matter simulation (**Alloy**), and — later — a game.

## License — read this first

**This is a commercial, proprietary project. All rights reserved.** The source is public so it
can be *read* and so CI can run — that is all. No use, copying, modification, redistribution,
or derivative work of any kind is permitted without explicit written permission from the author
(see [`LICENSE`](LICENSE); `vendor/` libraries keep their own upstream licenses). External
contributions are not accepted: issues and pull requests from outside will be closed unread.

## The stack

- **Engine**: lean-C-style C++ (no STL, no RTTI, no exceptions, no inheritance, no
  destructors; a closed list of flat value templates). One static exe; modules are folders /
  static libs with manifest-ordered registration.
- **Game layer**: **Luau** — data, meaning, gameplay; script reload is the iteration loop.
  Authoritative state never lives in the Luau heap.
- **Determinism**: **fixed-point by construction** — a closed palette of Q-formats
  (`fx<Rep,FRAC>`), det math with FixPointCS-ported kernels, keyed stateless RNG, pinned
  integer hashing. No floats in authoritative state; cross-ISA (x86-64 ↔ aarch64) for free.
- **Sim**: **Alloy** — SDF solids, PBF liquids, cavity gases, fields, chemistry/fire, XPBD
  mechanics, AgentBody; integer quanta authoritative for everything conserved.
- **Netcode**: deterministic lockstep, 8 peers, no dedicated server; ENet transport
  (inputs on unreliable + redundancy window, never reliable channels).
- **Platform**: SDL3 (+ SDL_Render at v0, SDL_GPU reserved), SDL_ttf, Dear ImGui editor,
  stb_image / stb_sprintf, Monocypher (crypto), rapidhash, own containers/arenas/ECS + X-macro
  reflection.

## Status

**Design complete, pre-code.** The founding ruling is **`docs/PIVOT-DESIGN.md`**; every system
now has its own design doc — start at **`docs/README.md`** (the map and reading order), then
`CLAUDE.md` (how to work here) and `TODO.md` (the build queue). Next milestone: **Gate 0** —
the headless fixed-point XPBD+PBF convergence and cost bench (`docs/GATE0-BENCH.md`). Then
foundation layer → ECS → v0 ("window + moving sprite + 60 Hz") → Hovel (the 3-machine lockstep
harness) → Alloy.

## Lineage

Successor to the **ore / foundry** program (custom deterministic language + engine designs).
The Ore language is retired; the design corpus was harvested into `docs/` on 2026-08-22 — each
doc names its foundry source once, at the top. The sibling `../foundry` repo is history (plus the
program-strategy and game-candidate docs, which are not engine systems and stay there until a
game repo owns them).

## Working rules (carried from the program)

- Durable context lives in committed files only; commit **and push** every time (two-PC sync).
- One feature per commit. Test infra first. No commit without tests.
- Fail loudly; no silent fallbacks. Root cause, no workarounds.
- Line endings: `.gitattributes` is the authority (`*.bat`/`*.cmd` CRLF, else LF).
- Rafael is sole author — no co-author trailers on commits.

## Intended layout (created as built, not upfront)

```
src/foundation/   fx palette + det math, arenas, containers, RNG/hash, StrView/interner, jobs
src/core/         ECS + reflection, events, input, assets + data tables, loop/time, Luau glue
src/sim/          Alloy (own static lib, symbol-audited)
src/render/       render2d: camera, extract, sort key, sprite batch, the sim view
src/net/          lockstep netcode (ENet)
src/platform/     SDL3 seam (+ headless impl)
src/editor/       ImGui tooling (dev builds only)
app/              main() + the one wiring file (module registration order)
script/           Luau — game data + meaning
vendor/           SDL3, SDL_ttf, imgui, luau, enet, stb, monocypher, rapidhash (own TUs)
tools/            offline only (audit, fingerprint, luauc, fxcheck) — exempt from the C++ subset
tests/            runner + driver + determinism harness + Gate 0 bench + Hovel
docs/             PIVOT-DESIGN.md (the founding ruling) + one design doc per system
```
