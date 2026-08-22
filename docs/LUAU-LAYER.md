# The Luau layer — data, meaning, iteration (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §9. Expands `PIVOT-DESIGN.md` §7.
> **Owns:** `src/core/script/` (VM setup, bindings, reload, trampolines), `script/` (the game).
> **Role:** games bring **data + meaning** — tables, tuning, gameplay systems, spawn logic, UI.
> Script reload is the iteration mechanism (replaces game-DLL hot reload, which is deleted along
> with its `migrate(T)`/layout-hash problem class).

---

## 0. The state boundary — the load-bearing rule (DECIDED)

> **Authoritative state never lives in the Luau heap.** Scripts read the world and write only
> through components (reflection glue) and the ordered command channel. Script-side tables are
> transient working data, reconstructible from world state, never carried across ticks.

Rationale: the Luau heap is not in the registered arena set; script-held state would survive
rollback while the world rewinds — a desync generator per-arena hashing is structurally blind to
(the singleton problem, bigger). The rule makes rollback, hashing, snapshots and saves correct for
scripts *by construction*, with zero Luau-specific snapshot machinery.

Enforcement: (1) the sim VM's globals are frozen after init (`setreadonly`) — a script cannot
create a global; (2) module-level upvalues are allowed only for constants and binding handles
(review + a dev-tier heuristic that hashes the VM's reachable table graph at tick start/end and
warns on growth across ticks); (3) the dual-sim test runs two sims with two VMs — a script that
secretly keeps state diverges the second world.

Persistent script state = a component (Luau-declared, `ECS.md` §6.1) or a singleton component.

---

## 1. Three VMs (DECIDED)

| VM | Library set | Determinism | Allocator | Codegen |
|---|---|---|---|---|
| **sim** | `ipairs`, `sortedpairs` (ours), `table` (array ops), `string` (pure fns only), `fx` (det math bindings), engine bindings (§3). **Removed:** `math` (stock), `os`, `io`, `debug`, `pairs`/`next`, `coroutine`, `string.rep`, `require` beyond the init phase, `loadstring` | inside the lockstep contract; bytecode in the fingerprint | own `mem_pool`, budgeted | **interpreter only** (native codegen is another codegen surface) |
| **ui/editor** | stock Luau + ImGui/draw/text bindings + read-only world access; `pairs` allowed | free | own pool | NCG allowed |
| **data** | stock Luau minus `os`/`io`; used once per table compile then destroyed (`ASSETS-AND-DATA.md` §3) | its *output* is hashed | throwaway | — |

The sim VM and the UI VM never share a `lua_State`; the UI VM reads the world through the same
read bindings the inspector uses and can only *write* by issuing commands (which are sealed).

### 1.1 Why `pairs` is removed from the sim VM (alternatives recorded)

| Option | Determinism | Cost | Verdict |
|---|---|---|---|
| allow `pairs` | Luau's hash-part order is deterministic for identical bytecode + identical op sequence (string hashing is unseeded), but **order-fragile**: an unrelated insertion reorders iteration; an effect whose order leaks into state is a latent desync | free | rejected |
| allow `pairs`, ban by review | same risk, unenforced | free | rejected |
| **remove `pairs`/`next`; provide `ipairs` + `sortedpairs(t)`** (sorts keys: numbers first ascending, then strings bytewise, into a scratch array) | order is a pure function of the key set | O(n log n) per walk; sim scripts iterate arrays and components, not dictionaries, so the cost is rare | **chosen** |

---

## 2. Numbers and fx (DECIDED)

Luau numbers are f64: `+ − ×` on integers are exact while results stay in ±2^53, so plain script
arithmetic on counts/ids/quanta is safe. **fx values cross as raw integer bits** (`v`), boxed as
`fx` userdata-free integers with the row known from the binding's signature — never as doubles of
the scaled value. `/` is never used on sim quantities (it yields a non-integer); `fx.div` is the
tool. Every binding that receives an integer checks `x == floor(x)` and range, and fails loud
otherwise. Every palette row is 32-bit and exactly representable; if the rung-4 fallback ever
introduces `fx<i64,32>` positions, this section gains a boxed-i64 rule.

`fx` module: `fx.pos(int_raw)`, `fx.to_raw`, `fx.mul_pos_vel_dt`, `fx.sqrt`, `fx.sincos`,
`fx.atan2`, `fx.lerp`, `fx.rng(key...)` — thin over `det_math.h`; the mixed-op table is the
binding list (no implicit combination exists in Luau either).

---

## 3. Bindings (DECIDED shape)

Two tiers, deliberately separate:

- **Hot gameplay bindings are hand-written** against the Luau C API: world queries
  (`ecs.each(Comp)` cursor iteration, `ecs.get(e, Comp)` → a lightweight accessor), commands
  (`ecs.spawn/destroy/add/remove`, `alloy.carve/spawn/impulse/…`), queries (`alloy.raycast`,
  `alloy.cavity_at`), `input.frame(slot)`, `events.read(T)`/`events.emit(T, …)`, `data.*`,
  `fx.*`, `log.*`. Each call is one C function; no per-call allocation.
- **Reflection-driven generic access** (`ecs.field(e, Comp, "hp")`) exists for the editor/tools
  VM and for cold paths; it walks `FieldInfo` by name hash. Hot paths never use it.

**Handles are tagged lightuserdata** (per-domain `lua_setlightuserdataname` tags): zero
allocation, typed (an `Entity` is not a `BodyHandle`), `nil` for null. Alternatives: plain
integers (untyped — rejected), full userdata (allocates — rejected).

Component access from Luau: `ecs.get(e, Health)` returns a *proxy* bound to `(column, dense
index, tick)` — reads/writes go straight to the column through the field table; the proxy is
invalid after the tick (debug: asserts on the tick stamp). Bulk iteration hands `(entity, proxy)`
pairs in packed order. A Luau system that needs speed over thousands of entities is a C++ system
— the binding tax is the signal to promote.

---

## 4. Systems and registration from Luau (DECIDED)

```lua
ecs.component("Health", { {"hp","i32"}, {"hp_max","i32"} })
ecs.system("regen", "UPDATE", { reads = {"Health"}, writes = {"Health"}, after = {"damage"} },
  function(w) for e, h in ecs.each(Health) do if h.hp < h.hp_max then h.hp = h.hp + 1 end end end)
```

`ecs.system` registers a C++ trampoline with a `SystemDesc` (same schedule, same ordering rules
as C++ systems — `ECS.md` §3); the trampoline calls the Luau function with the world access table.
Registration happens in the script's init phase (before the world's first tick) in **declaration
order**, which is the same on every peer.

---

## 5. Runtime rules (DECIDED)

- **GC:** Luau's allocator is hooked to the VM's pool; `lua_gc(LUA_GCSTEP, n)` runs each tick with
  an explicit step budget. GC timing cannot change values (state isn't in the heap) but its CPU
  spike can blow a frame; the profiler reports it.
- **Interrupt callback** on the sim VM with a per-tick instruction budget: over budget =
  `TL_FATAL` in netcode tier (a runaway script is a deterministic hang on every peer — fail loud,
  with the traceback), a pause + console in dev.
- **Errors:** a runtime error in a sim script is deterministic (every peer hits it) → `TL_FATAL`
  with traceback in netcode/ship; dev pauses the sim and opens the console. `pcall` is available
  to scripts for their own recoverable paths.
- **Script reload (dev only):** a sealed command at the next barrier: reload bytecode, re-run
  module init, re-register systems (schedule rebuild). Component layouts may not change across a
  reload without a save→load migration cycle (`ASSETS-AND-DATA.md` §5) — the reload command
  refuses a layout change and tells you to use "reload with migration". World state survives
  because it was never in Luau. Refused during a lockstep session (fingerprint change).
- **Coroutines:** removed from the sim VM (§9 R-1); available in the UI VM.

---

## 6. Bytecode and the fingerprint (DECIDED)

- Scripts are compiled with the **vendored Luau compiler at a pinned version and pinned options**
  (`-O2`, debug level 1 for line info, no NCG). `netcode`/`ship` tiers embed precompiled `.luac`
  produced by `tools/luauc` at build; `dev` compiles on load with the same compiler binary — the
  output is identical by construction (same compiler, same options).
- The sim VM's bytecode bytes, in load order, hash into the **build fingerprint**
  (`BUILD.md` §5). UI/editor scripts do not (they cannot affect state).

---

## 7. Debugger scope (RULED 2026-08-21 — ceiling = Tier 1)

| Tier | What | When |
|---|---|---|
| **0** | tick-stamped script logging to the ImGui console, error traces with file:line, record→replay scrubbing (determinism *is* time-travel debugging) | v0 tooling |
| **1** | break-and-inspect: gutter breakpoints in ImGuiColorTextEdit via Luau's native debugger interface (`lua_breakpoint` / debug interrupt / single-step — Roblox Studio's own path, integration not research); pause the whole sim on hit; stack + locals/upvalues panel; step-line/in/resume. Dev only, compiled out elsewhere; interpreter-only composes with it | when real gameplay scripting starts |
| 2 | IDE-class: watches, conditional breakpoints, live variable editing | **rejected** unless daily scripting demonstrably outgrows Tier 1 — a breakpoint shows one moment, a replay shows every moment repeatably |

---

## 8. Why Luau (recorded — PIVOT §1 decided it; this is the matrix so it isn't re-run)

| | Luau | Lua 5.4 | own VM | C++-only gameplay |
|---|---|---|---|---|
| determinism | unseeded string hash; sandboxable; interpreter deterministic; NCG optional | `pairs` order + GC observables (Factorio patched it) | deterministic by construction, years of work | native |
| sandbox / mods | production sandbox (Roblox); `setreadonly`, interrupt, allocator hook | weak | ours to build | n/a |
| iteration | reload in ms; typed; linter | reload in ms | — | 10 s rebuild |
| cost | C++ lib, vendored, `LUA_USE_LONGJMP` | pure C | huge | zero |
| verdict | **chosen** | rejected | rejected | the C++ side exists for hot paths; the *game* is data + meaning, which wants a language that reloads |

---

## 9. Rulings (closed 2026-08-22 — nothing open)

- **R-1 `coroutine` is removed from the sim VM.** A coroutine that yields across ticks is heap
  state by definition; "within-tick only" would need a runtime check for a convenience no sim
  script needs (sequencing over ticks is a component state machine). The UI VM keeps coroutines.
  §1's table and §5's coroutine bullet are superseded by this ruling.
- **R-2 `string` stays in the sim VM, pure functions only** (`format`, `sub`, `byte`, `len`,
  `find` without patterns that allocate unboundedly — the stock library is fine; `string.rep`
  is removed as an allocation bomb). Strings never enter state; they feed `log.*` and ids.
- **R-3 Luau-declared components may register a custom inspector draw** (`ui.inspect("Health",
  fn)` in the UI VM). Dev only; the generic walker remains the mechanism and the fallback.

*Rev 1 — 2026-08-22.*
