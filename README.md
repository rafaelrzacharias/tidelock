# tidelock

> **Tidal locking**: two bodies held in perfect, permanent lockstep by mutual physical force.
> The name three ways: *lockstep* — 8 peers bit-exact by construction; *tides* — a matter sim
> of fluids, pressure, heat and reaction; and the body that locks us is the *moon* — **Lua**.

A 2D game engine, a deterministic matter simulation (**Alloy**), and — later — a game.

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
  stb_image / stb_sprintf, Monocypher (crypto), own containers/arenas/ECS + X-macro
  reflection.

## Status

**Design complete, pre-code.** The founding ruling — every decision, the fx palette, the
frozen Gate 0 bench spec, and the recovery ledger — is **`docs/PIVOT-DESIGN.md`** (read it
first). Next milestone: **Gate 0** — the headless fixed-point XPBD+PBF convergence and cost
bench. Then foundation layer → ECS → v0 ("window + moving sprite + 60 Hz") → Hovel (the
3-machine lockstep harness) → Alloy.

## Lineage

Successor to the **ore / foundry** program (custom deterministic language + engine designs).
The Ore language is retired; the design corpus is harvested, not abandoned — until the doc
sweep migrates them, the surviving design docs live in the sibling **`../foundry`** repo:
`ALLOY-DESIGN.md` (the sim — mechanisms survive whole), `NETCODE-DESIGN.md` (rev 3 —
transport-agnostic, survives whole), `DETERMINISM-DESIGN.md` (ordering rules + test
harness), `FOUNDRY-*.md` (architecture decisions, superseded where PIVOT-DESIGN says so).

## Working rules (carried from the program)

- Durable context lives in committed files only; commit **and push** every time (two-PC sync).
- One feature per commit. Test infra first. No commit without tests.
- Fail loudly; no silent fallbacks. Root cause, no workarounds.
- Line endings: `.gitattributes` is the authority (`*.bat`/`*.cmd` CRLF, else LF).
- Rafael is sole author — no co-author trailers on commits.

## Intended layout (created as built, not upfront)

```
src/foundation/   fx palette + det math, arenas, containers, RNG/hash, StrView/interner
src/core/         ECS + reflection, events, input, assets, time
src/sim/          Alloy
src/net/          lockstep netcode (ENet)
src/platform/     SDL3 seam
src/editor/       ImGui tooling (dev builds only)
script/           Luau — game data + meaning
vendor/           SDL3, imgui, luau, enet, stb, monocypher (compiled once, own TUs)
tools/            offline-only (exempt from the C++ subset)
tests/            runner + determinism harness + Gate 0 bench
docs/             PIVOT-DESIGN.md (the founding ruling) + future design docs
```
