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

**Building — W2/W3 of the wave plan (status re-dated 2026-08-27; `TODO.md` and `docs/ROADMAP.md`
are the live truth, this paragraph is only a pointer).** The design corpus is complete
(`docs/PIVOT-DESIGN.md` is the founding ruling; `docs/README.md` is the map). Shipped: the W0
skeleton and audits; the full W1 foundation (fx palette + det math, arenas/registry,
containers, RNG/hash, headless platform, test runner, tooling runtimes, jobs); Gate 0 run and
ruled (`FX-PALETTE.md` rev 2, 2026-08-25); the W2 ECS and netcode Phase 1 merges; the W2
luau-vm and vendor merges (2026-08-26 — Luau 0.696, SDL3, SDL_ttf + FreeType, ImGui, ENet,
Monocypher and stb, every allocator pooled and gated); four-leg hosted CI ({Windows, Linux} ×
{x86-64, arm64}) green on `main`; and the W3 **render2d** lane (merged 2026-08-27, PR #13 —
camera, extract, queue/sort/batch, sprite and debug draw, the SDL present path). In flight: the
W3 slack lanes loop+input and assets+data (launched early on the spare non-Fable budget, ruled
2026-08-26); editor chains after render2d; alloy-substrate launches at the weekly reset.
Next: W3 — alloy-solver ★ and the v0 lanes.

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

## Layout

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
