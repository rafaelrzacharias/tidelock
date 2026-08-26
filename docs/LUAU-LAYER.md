# The Luau layer — data, meaning, iteration (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §9. Expands `PIVOT-DESIGN.md` §7.
> §10 is the implementation specification (file layout, VM construction, binding signatures,
> proxies, reload, bytecode pipeline, tests); it is placed before the rulings by convention.
> **Owns:** `src/script/` (VM setup, bindings, reload, trampolines), `script/` (the game).
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
| **data** | stock Luau minus `os`/`io` **and minus `math.random`/`math.randomseed`** (ruled 2026-08-26 — Luau seeds its PCG from `uintptr_t(L) ^ time(NULL) ^ clock()`, and this VM's output is hashed, so a single draw makes a peer-divergent table that surfaces as a fingerprint mismatch instead of an error at the mistake); used once per table compile then destroyed (`ASSETS-AND-DATA.md` §3) | its *output* is hashed | throwaway | — |

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

`fx` module: per-row literal constructors `fx.pos(12.5)` (exactly-representable arguments only,
`ASSETS-AND-DATA.md` §7 R-2), `fx.raw(bits)`, `fx.mul_pos_vel_dt`, `fx.mul_q`, `fx.dist`,
`fx.sincos`, `fx.atan2`, `fx.lerp`, `fx.rng_below`/`fx.rng_q` — thin over `det_math.h`; the
mixed-op table is the binding list (no implicit combination exists in Luau either). The full
signature list with argument checks is §10.3.

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
invalid after the tick (the tick-stamp compare is one instruction and runs in every tier, see
§10.5). Bulk iteration hands `(entity, proxy)` pairs in packed order, one proxy per loop
(§10.5). Handle tags: §10.4. A Luau system that needs speed over thousands of entities is a C++ system
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
order**, which is the same on every peer. Field-spec grammar, the packer, the trampoline's
dispatch and error path: §10.6.

---

## 5. Runtime rules (DECIDED)

- **GC:** Luau's allocator is hooked to the VM's pool; `lua_gc(LUA_GCSTEP, n)` runs each tick with
  an explicit step budget. GC timing cannot change values (state isn't in the heap) but its CPU
  spike can blow a frame; the profiler reports it.
- **Interrupt callback** on the sim VM with a per-tick instruction budget: over budget =
  `TL_FATAL` in netcode tier (a runaway script is a deterministic hang on every peer — fail loud,
  with the traceback), a pause + console in dev. Luau's interrupt fires at safepoints, not per
  instruction; the budget counts safepoints, which is equally deterministic (§10.2 step 8).
- **Errors:** a runtime error in a sim script is deterministic (every peer hits it) → `TL_FATAL`
  with traceback in netcode/ship; dev pauses the sim and opens the console. `pcall` is available
  to scripts for their own recoverable paths.
- **Script reload (dev only):** a sealed command at the next barrier: reload bytecode, re-run
  module init, re-register systems (schedule rebuild). Component layouts may not change across a
  reload without a save→load migration cycle (`ASSETS-AND-DATA.md` §5) — the reload command
  refuses a layout change and tells you to use "reload with migration". World state survives
  because it was never in Luau. Refused during a lockstep session (fingerprint change).
  Procedure, layout-hash comparison and the migration path: §10.8. Per-tick GC/budget
  sequence: §10.7.
- **Coroutines:** removed from the sim VM (§9 R-1); available in the UI VM.

---

## 6. Bytecode and the fingerprint (DECIDED)

- Scripts are compiled with the **vendored Luau compiler at a pinned version and pinned options**
  (`-O2`, debug level 1 for line info, no NCG). `netcode`/`ship` tiers embed precompiled `.luac`
  produced by `tools/luauc` at build; `dev` compiles on load with the same compiler binary — the
  output is identical by construction (same compiler, same options).
- The sim VM's bytecode bytes, in load order, hash into the **build fingerprint**
  (`BUILD.md` §5). UI/editor scripts do not (they cannot affect state). Load order = bytewise
  path order of `script/sim/**` + `script/lib/**`; `luauc` CLI, manifest format, compile options
  struct and chunkname convention: §10.9.

---

## 7. Debugger scope (RULED 2026-08-21 — ceiling = Tier 1)

| Tier | What | When |
|---|---|---|
| **0** | tick-stamped script logging to the ImGui console, error traces with file:line, record→replay scrubbing (determinism *is* time-travel debugging) | v0 tooling |
| **1** | break-and-inspect: gutter breakpoints in ImGuiColorTextEdit via Luau's native debugger interface (`lua_breakpoint` / debug interrupt / single-step — Roblox Studio's own path, integration not research); pause the whole sim on hit; stack + locals/upvalues panel; step-line/in/resume. Dev only, compiled out elsewhere; interpreter-only composes with it. API calls: §10.10 | when real gameplay scripting starts |
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

## 10. Implementation specification (rev 1, 2026-08-22 — build from this, not from §0–§9)

Engine side obeys `CPP-SUBSET.md`; the TUs in `src/script/` are the Luau wrap module and are
the only place a Luau header or a `lua_State*` may appear (`ARCHITECTURE.md` §3). Everything above
(`app/`, `editor/`) sees `ScriptVm*` and our types.

### 10.1 File layout

```
src/script/
  script.h        public: ScriptVm, script_create_sim/ui/data, script_tick_begin/end, script_reload,
                  script_load_manifest, ErrCode enum SCRIPT_*; no Luau types
  vm.h / vm.cpp   lua_State lifecycle per VM kind; allocator; interrupt; useratom; GC step; error path
  sandbox.cpp     library whitelist, global removal, sortedpairs, tostring/format replacements, freeze
  handles.h       tag numbers (§10.4), push/check helpers for every domain
  proxy.cpp       component / event / data-row / input-frame proxies (userdata + metatables)
  bind_fx.cpp     fx.*            bind_ecs.cpp    ecs.*           bind_alloy.cpp  alloy.*
  bind_input.cpp  input.*         bind_events.cpp events.*        bind_data.cpp   data.*
  bind_log.cpp    log.*           bind_ui.cpp     ui.*, draw.*, console.* (UI VM only; links tl_editor)
  trampoline.cpp  Luau systems as SystemDesc; the two bracket systems (§10.7)
  reload.cpp      dev reload + migration (§10.8); compiled out in netcode/ship
  debug.cpp       Tier 0/1 hooks (§10.10); compiled out in netcode/ship
  loader.cpp      manifest, require, luau_load, dev on-load compile (§10.9)
script/
  sim/main.luau   entry (the only file the engine loads by name); sim/input.luau; sim/components/;
                  sim/systems/            — sim VM, fingerprinted
  ui/main.luau    entry; ui/panels/       — UI VM, not fingerprinted
  data/*.luau     one file per table; each returns a Luau table — data VM
  lib/*.luau      pure modules (no bindings, no VM-specific globals) requirable from any VM; a lib
                  file reachable from sim/ is in the sim manifest and therefore fingerprinted
tools/luauc/      the compiler CLI (§10.9); links vendor/luau Compiler + Ast; exempt from the subset
tests/script/     §10.11
```

### 10.2 VM construction sequence

One function per VM kind; all three call the common core in this order. A step that fails
returns `Result<ScriptVm*>` with `SCRIPT_*` codes; no partial VM survives.

1. **Pool.** `mem_pool` over its own `VMemArena` reserve from the `app/` reserve table
   (sim/UI 64 MB each, `MEMORY.md` §6; data VM: its own entry, decommitted after compile). The
   pool's budget is part of the reserve table, hence fingerprinted (`MEMORY.md` §7 R-2).
2. **`lua_newstate(script_alloc, pool)`**, `void* script_alloc(void* ud, void* ptr, size_t osize,
   size_t nsize)`: `nsize == 0` → `pool_free(ud, ptr, osize)`, return `NULL`; `ptr == NULL` →
   `pool_alloc(ud, nsize)`; else `pool_realloc(ud, ptr, nsize)` (`osize` is accepted for ABI compatibility and ignored: `mem_pool` recovers the old size from the block's page header, `MEMORY.md` §8.6). Over budget →
   **return `NULL`**; Luau raises its memory error (`LUA_ERRMEM`, message `"not enough memory"`),
   which unwinds to the nearest protected call — always ours (§10.6 trampoline, §10.9 init
   `pcall`) — and takes the error path: `TL_FATAL` in netcode/ship, pause + console in dev. The
   counter the profiler reads (`luau_pool_used`) is the pool's.
3. **Libraries** — explicit `luaopen_*` calls, never `luaL_openlibs` on the sim VM:
   | VM | opened |
   |---|---|
   | sim | `luaopen_base`, `luaopen_table`, `luaopen_string` |
   | ui | `luaL_openlibs` (everything) |
   | data | the explicit list minus `luaopen_os` (**corrected 2026-08-26, w2-luau-vm, measured at the 0.696 pin:** Luau's `linit.cpp` DOES open `os`; there is no `luaopen_io` in Luau at all. Each VM opens its list explicitly, so the opened set is readable in `sandbox.cpp` and cannot drift when upstream adds a library) |
4. **Removals (sim VM)** — `lua_pushnil` + `lua_setfield` on the named table, in this order, each
   asserted absent afterwards by the sandbox test: `_G.math`, `_G.os`, `_G.io`, `_G.debug`
   (never opened, asserted anyway), `_G.pairs`, `_G.next`, `_G.coroutine`, `_G.loadstring`,
   `_G.collectgarbage`, `_G.gcinfo`, `_G.getfenv`, `_G.setfenv`, `_G.newproxy`, `_G.print`,
   `_G.rawequal`, `_G.rawget`, `_G.rawset` (**added 2026-08-26**: `CANON.md` "Luau sim VM" is the
   home of this list and already named the three — the proxies forbid raw access — and this list
   had drifted from it), `string.rep`, `table.foreach`, `table.foreachi` (they call `next`).
   The "asserted absent afterwards" check runs in `script_sandbox_open` itself, not only in the
   test: a removal that silently did nothing is a hole in the sandbox, and a hole found by a test
   nobody ran is not a gate (`LESSONS.md`). VM creation fails with `SCRIPT_ERR_SANDBOX`. `_G.require` is installed
   in step 7 and removed at the end of init (§10.9). Data VM removes `os`, `io`, `loadstring`,
   `getfenv`, `setfenv`. UI VM removes nothing.
5. **Replacements (sim VM):** `tostring` → C function returning the type name for tables,
   functions and userdata (the stock form prints an address — a nondeterministic value a script
   could branch on); `string.format` → a thin C wrapper that routes `%s` through that `tostring`.
   `sortedpairs` (§10.2.1) is added to `_G`.
6. **Binding tables** (§10.3–§10.6) created with `lua_createtable(L, 0, n)` + `lua_pushcfunction`
   (`lua_pushcclosurek` for closures), set as globals: `fx`, `ecs`, `events`, `input`, `alloy`,
   `data`, `log` (sim); the UI VM gets the read-only subset of `ecs`/`alloy`/`data` plus `ui`,
   `draw`, `console`. `log.info` is what `print` would have been.
7. **`require`** installed (§10.9); `_G.world` = the `WorldView` table (§10.6).
8. **Callbacks:** `lua_Callbacks* cb = lua_callbacks(L)`; `cb->interrupt = script_interrupt`;
   `cb->useratom = script_useratom`
   (**RULED 2026-08-26 — RR-19:** Luau's `useratom` carries no context pointer and no
   `lua_State*`, so the process `Interner` is reached through one exempted pointer in
   `src/script/atom.cpp`, named in `tools/audit/static_allow.txt`. It is a LOOKUP, never an
   insert: a string a script builds at runtime must not be able to grow a capped,
   fingerprint-adjacent interner, so an unregistered name yields −1. A VM created with a null
   interner installs nothing and `script_useratom_installed()` says so); `cb->panic = script_panic` (`TL_FATAL` — reached only on an
   error outside any protected call, which is a bug in this module); `cb->userthread = NULL`
   (sim has no coroutines; the dev debugger thread is created by C, §10.10). Dev: `debugbreak`,
   `debugstep`, `debuginterrupt` (§10.10).
   - **Interrupt / instruction budget.** Luau calls `interrupt(L, gc)` at *safepoints* (function
     entry, loop back-edges, and from the GC with `gc >= 0`); it has no per-instruction hook. The
     budget therefore counts safepoints: `vm->budget_left -= 1` on each call with `gc < 0`; at
     `0` → `luaL_error(L, "script budget exceeded in %s", running_system_name)` which takes the
     normal error path (fatal in netcode — a runaway is a deterministic hang on every peer — pause
     + console in dev). The count is a pure function of the bytecode executed, so every peer trips
     on the same safepoint. ``script.budget_safepoints`` (sim, per tick) is a `SIM` cvar
     (fingerprinted). The UI VM has a per-frame budget that only logs; the data VM has a
     per-compile budget that fails the compile.
   - **`useratom(const char* s, size_t len) → int16_t`:** `intern(StrView{s,len})` → `StrId`
     if already registered in the interner, else `-1`. Only names registered at init (component
     fields, actions, tables, event types) get atoms; `lua_tostringatom` then yields the field's
     `StrId` with no hashing per access. `StrId` is `u16`; the interner is capped at 32767 entries
     so the atom is always non-negative. Atoms are process-stable, never serialized, never in state.
9. **Codegen:** `luau_codegen_create(L)` is called only for the UI VM, and only when
   `luau_codegen_supported()`; every UI chunk is passed through `luau_codegen_compile` after
   `luau_load`. Sim and data VMs never call `luau_codegen_create` — the interpreter is the only
   executor (§1). **The CodeGen library is NOT vendored as of 2026-08-26** (ruling request
   RR-20 in `TODO.md`, with the measured build-time cost), so there is no
   `luau_codegen_supported()` in the binary to ask; `script_codegen_available()` reports that
   fact rather than falling back to the interpreter in silence.
10. **Run init** (§10.9): `main.luau` executes under `lua_pcall`; `ecs.component`/`ecs.system`/
    `input.action`/`ecs.event` are legal only while `vm->init_open` is true.
11. **Seal:** `lua_setsafeenv(L, LUA_GLOBALSINDEX, true)` then `lua_setreadonly(L, idx, 1)` on
    `_G`, on every binding table, on `WorldView`, on `string`/`table`, and on the metatables of
    §10.5. `require` removed first. After this step the sandbox test asserts a global assignment
    raises `"attempt to modify a readonly table"`.

#### 10.2.1 `sortedpairs(t)`

C function. Collects keys with `lua_next` into a scratch array (push-marked, popped on return) of
`TValue`-free records `{ kind: u8 (0 = number, 1 = string); double n; const char* s; u32 len; }`;
sorts with a stable bottom-up merge local to `sandbox.cpp` (**corrected 2026-08-26**: `CONTAINERS.md` §4's `sort_u32_kv`/`sort_u64_kv` is an LSD *radix* over an INTEGER key with no comparator, and §6 puts the only comparison sort in `tools/` — neither can express a total order over mixed number/string keys, so the one place that needs it carries it): numbers before strings;
numbers ascending by value (NaN is impossible — integers only; a non-integer key is a Luau error);
strings bytewise (`memcmp` over `min(len)`, shorter first on tie). Keys of any other type (table,
boolean, userdata) → error `"sortedpairs: unsupported key type"`. Returns an iterator C closure
over a Luau table copy of the sorted key array (one allocation per walk; the record array is a
Luau userdata, so its bytes come from the VM's own pool and are collected with the walk — no
engine arena is reachable from a binding, and `pool_alloc` is barred outside `vendor_glue/`). The iterator reads `t[key]` by `lua_rawget`, so a value set to `nil` mid-walk is skipped,
never re-ordered. Same code in all three VMs.

### 10.3 Numbers and the `fx` binding contract

Luau never sees a scaled double. Every fx value is the raw `i32` bits of its row, carried as a
Luau number; the row is fixed by the binding's signature, not by the value. Comparison (`<`, `==`)
on raw bits of the same row is correct as-is. `+`/`-` on raw bits of the same row are exact
(|result| < 2^53) and are range-checked when written back (below). `*` and `/` between fx values
are **never** written in script (the mul/div shift is what the bindings are for); `//` and `%`
on plain integers are fine. Review rule, plus the dev-tier lint in `tools/audit` that greps
`script/sim/**` for `/` outside `//` and comments.

**Argument checks — one helper, used by every binding:** `i64 check_int(L, idx, lo, hi)`:
`lua_type == LUA_TNUMBER` and `x == floor(x)` and `lo <= x <= hi`, else `luaL_argerror` with the
function name, the argument index, and the bound. Rows use `[INT32_MIN, INT32_MAX]`; handles use
their width; counts use their declared range.

| Luau | C++ | Notes |
|---|---|---|
| `fx.pos(n) fx.vel(n) fx.invmass(n) fx.stiff(n) fx.q(n) fx.angle(n) fx.omega(n) fx.scalar(n)` | `from_literal<R>` | **literal constructors** (`ASSETS-AND-DATA.md` §7 R-2): `n` is a Luau number; accepted iff `n × 2^FRAC` is an integer (exact double test: `ldexp(n, FRAC)` equals its floor and |n| < 2^(31−FRAC)) — then that integer is the raw value; else error naming the row. No rounding ever happens here. Valid in every VM; in the data VM the error is decorated with table/row/field |
| `fx.raw(bits)` | identity | `check_int` to i32. The escape hatch for computed values |
| `fx.H` `fx.INV_H` `fx.G_SUBSTEP` `fx.TEXEL` `fx.V_MAX_WORLD` | constants | raw bits / plain ints from `fx_palette.h`; `fx.POS=18, VEL=20, INVMASS=18, STIFF=30, Q=30, ANGLE=30, OMEGA=22, SCALAR=16` frac-bit constants for `fx.str` only (`CANON.md` "The fx palette" is the home; `OMEGA` moved to 22 at palette rev 2) |
| `fx.mul_q(q, a) → a's row` | `mul<A>(q_t, A)` | row-independent: shift is always 30 |
| `fx.mul_scalar(s, a) → a's row` | `mul<A>(scalar_t, A)` | shift always 16 |
| `fx.div_q(a, b) → q` | `div<q_t>(A, A)` | same-row operands; `b == 0` → error |
| `fx.mul_pos_vel_dt(x, v) → pos` | `x + mul<pos_t>(v, H)` | the integrate step; `dt_t` is only `H` so no dt argument exists |
| `fx.vel_from_delta(dx) → vel` | `mul_int<vel_t>(pos_t, INV_H)` | |
| `fx.dist(x0,y0,x1,y1) → pos` | `sqrt<pos_t>(fx<i64,36>)` | the only path to `pos×pos` |
| `fx.normalize(dx, dy) → qx, qy` | `normalize(vec2<pos_t>)` | zero vector → `0, 0` |
| `fx.sincos(a) → qs, qc` / `fx.atan2(y, x) → angle` | `det_math.h` | `atan2` takes `pos_t`; a `q_t` variant is `fx.atan2_q` |
| `fx.lerp(a, b, t) → a's row` | `lerp<A>(A, A, q_t)` | |
| `fx.sat_add(a,b) fx.sat_sub(a,b)` | `sat_add/sat_sub` on i32 | quanta paths; plain `+` on quanta is a review rejection |
| `fx.rng_below(carrier, draw, n) → [0,n)` / `fx.rng_q(carrier, draw) → q` | `rng_for(w->seed, w->tick, sys_id, carrier, draw)` then `rng_below`/`rng_q` | `sys_id` = the running trampoline's id (§10.6); outside a system → error. `carrier` is an integer (≤ 2^53) or an `Entity` (its bits); `draw` is the script's local per-carrier counter |
| `fx.str(v, frac) → string` | integer decimal, local to `bind_fx.cpp` (**corrected 2026-08-26**: `fmt_buf` is still a `TL_FATAL("unimplemented")` stub blocked on `vendor/stb_sprintf`, `CONTAINERS.md` §8.6b; the conversion is integer-only and needs no formatter — sign, integer part, then nine fractional digits developed by repeated multiply-by-ten on the remainder, truncated, which is stated rather than discovered) | `log` only; UI VM also has `fx.to_f64` — absent from the sim VM |
| `fx.imin fx.imax fx.iabs fx.iclamp` | integer | `math` is gone; these are checked integer ops |

### 10.4 Handles — tagged lightuserdata

`lua_pushlightuserdatatagged(L, (void*)(uintptr_t)bits, TAG)`; null handle (`bits == 0`) pushes
`nil`; `nil` argument reads as the null handle where a binding documents "nullable". Readers use
`lua_tolightuserdatatagged(L, idx, TAG)`; a `NULL` result on a non-nil argument → `luaL_typeerror
(L, idx, name)` with the domain name registered by `lua_setlightuserdataname(L, TAG, name)` at
VM creation. Equality `==` in Luau compares pointer + tag → handle equality. Never an allocation.

| Tag | Name | Payload |
|---|---|---|
| 1 | `Entity` | `Entity.bits` (u32) |
| 2–6 | `Body` `Constraint` `Plant` `Cavity` `Basin` | Alloy handle bits per `ALLOY.md` §1.1 |
| 7–10 | `Tex` `Font` `Audio` `DataTable` | `Handle<_,12,4>.bits` |
| 16 | `Component` | `ComponentId + 1` (id 0 is valid; 0 must stay null) |
| 17 | `EventType` | index + 1 into the world's event table (the `NameHash` is 64-bit; the index is the stable in-process key) |
| 18 | `Action` | `ActionId + 1` |

Tags are `< LUA_LUTAG_LIMIT`; the table above is the closed list; `handles.h` has one
`push_*`/`check_*` pair per row. Component kinds map to tags 1–10 for `XH` fields.

### 10.5 Proxies

```cpp
struct Proxy { World* w; ComponentId c; u16 _pad0; u32 dense; u64 tick; };   // 24 B, full userdata
```

Created with `lua_newuserdatatagged(L, sizeof(Proxy), UTAG_PROXY)`; one metatable per userdata
tag, installed once at VM creation with `lua_setuserdatametatable(L, UTAG_PROXY, -1)`
(`__index`, `__newindex`, `__tostring`, `__metatable = false`; readonly). Tags: `UTAG_PROXY`
(component), `UTAG_EVENT` (event read buffer row — `c` is the event index, writes error),
`UTAG_ROW` (data-table row — `c` is the table id, `tick` is ignored, writes error), `UTAG_INPUT`
(`InputFrame`, §10.6). One `FieldInfo` dispatch serves all four.

- **Validity:** `__index`/`__newindex` first check `p->tick == p->w->tick` (one compare, every
  tier — a stale proxy is a logic error whether or not asserts are on) and `p->dense <
  column_count(w, c)`; failure → error `"stale proxy <Comp>"`. Dense indices are stable within a
  tick because structure changes only at barriers (`ECS.md` §4); `world_flush` is not bound.
- **Field lookup:** `lua_tostringatom(L, 2, &atom)`; `atom < 0` → error `"<Comp> has no field
  <key>"`. Per component the registrar builds `FieldAtom { StrId atom; u16 field; }[]` sorted by
  atom (binary search; fields ≤ 64 in practice). `__newindex` in the UI VM always errors.
- **Kind switch** (read → push / write → `check_int` + store, through `FieldInfo.offset`):
  `i8..i64/u8..u64` → integer (`i64`/`u64` beyond 2^53 is an error on read: "not representable");
  `bool` → boolean; every fx row → raw `i32` bits as an integer (read: `lua_pushinteger`; write:
  `check_int` to i32 — the only write-side check, `ECS.md` §9 R-2); `Entity`/handle domains →
  tagged lightuserdata of §10.4 (write accepts that tag or `nil`); `StrId` → the interned string
  (read) / a string interned at init only (write: must already be interned, else error).
  **Fixed arrays** (`count > 1`): `p.flags` errors with "array field: use ecs.get_at";
  `ecs.get_at(p, "flags", i)` / `ecs.set_at(p, "flags", i, v)` with `1 <= i <= count` do the
  element access through the same switch (no sub-proxy allocation).
- `ecs.entity(p) → Entity` (from `world_entities`), `ecs.component_of(p) → Component`.
- **`ecs.each(Comp)`** returns `(iter, proxy, nil)` for the generic `for`: `proxy` is one
  `Proxy` allocated per loop with `dense = 0xFFFFFFFF`; `iter(proxy, _)` does `dense += 1`; if
  `dense >= count` (count read at loop start; exact, structure cannot change within the tick)
  returns `nil`; else returns `(Entity, proxy)`. Zero allocation per element; packed order 0..n−1.
  Mutating through the proxy during the loop is legal (it is the column, not a copy).
- **`ecs.get(e, Comp)`** → `world_get(w, e, c)` probe; absent → `nil`; present → a fresh `Proxy`
  (one 24 B allocation; the cold-path cost §3 names). `ecs.has(e, Comp)` → boolean, no
  allocation. `ecs.singleton(Comp)` → `Proxy{dense = 0}` on a `SINGLETON` column; error otherwise.
- **Commands** map 1:1 to `ECS.md` §4: `ecs.spawn() → Entity` (`world_spawn`: reserved id now),
  `ecs.destroy(e)`, `ecs.add(e, Comp, init?)` (the payload is built on scratch by walking
  `FieldInfo` in table order — each field read from `init` by name via `lua_rawgetfield`; absent →
  the declared default; an `init` key that is not a field → error — then `world_add`),
  `ecs.remove(e, Comp)`, `ecs.alive(e) → boolean`. Reads of a reserved-but-unrealized entity
  return `nil`/absent until the barrier, exactly as in C++.
- **Cold reflection** (`ecs.field(e, Comp, "hp")`, `ecs.set_field`) is the same dispatch without
  the proxy; UI VM and tools only.

### 10.6 Luau systems, events, input, alloy, data, log

**`ecs.component(name, fields, opts?) → Component`** (`ECS.md` §6.1 spells it
`ecs.declare_component`; same binding, `ecs.component` is the name). Init only. `fields` is an
array of `{ name, kind [, count] [, default = int] }`; `kind` is a string from the closed enum:
`i8 i16 i32 i64 u8 u16 u32 u64 bool pos vel invmass stiff q angle omega scalar Entity Body
Constraint Plant Cavity Basin Tex Font Audio DataTable StrId`; `count` 1..255; `default` an
integer (raw bits for fx rows; `0` when absent; handles and `StrId` have no default but null/0).
`opts = { singleton = true }` registers a `SINGLETON` column. **Packer:** offset = `align_up
(offset, align(kind))`, natural alignment = size of the scalar kind (arrays align as their
element); every interior gap is emitted as an explicit `_padN` `u8[gap]` field so `sizeof == Σ
field sizes` holds; tail pad to the max alignment, also explicit. The `FieldInfo[]` goes into the
permanent (non-registered) arena; `ComponentInfo.name = hash(name)`; `world_register_component`.
Then `_G[name] = Component handle` (globals are writable during init) — a name already bound as
a global, or already registered by C++ (`NameHash` collision), is an error. Field names and
`name` are interned here so they have atoms. Declaration order = registration order.

**`ecs.event(name, fields) → EventType`** — same spec, `world_register_event`.

**`ecs.system(name, phase, deps, fn)`** — init only. `phase` ∈ `"FIRST" "PRE_UPDATE" "UPDATE"
"POST_UPDATE" "LAST"` (render phases are not legal from the sim VM); `deps = { reads = {...},
writes = {...}, before = {...}, after = {...} }` where reads/writes entries are Component handles
or names and before/after are system name strings; `fn` is a function. Effects: `int ref =
lua_ref(L, fn_idx)`; `LuauSystem { NameHash label; int fn_ref; u32 rng_sys_id; u16 ordinal; }`
appended to `vm->systems` (permanent arena, max 256); `SystemDesc { fn = script_trampoline, label,
phase, reads, writes, before, after, flags = SYS_LUAU }` built on the permanent arena
(`Span`s point into it) → `world_register_system`. `rng_sys_id = RNG_SYS_LUAU_BASE + ordinal`
(`rng_systems.h` reserves a 256-wide block; ordinal = declaration order, identical on every peer).

**Trampoline.** `SystemFn` receives only `World*`, so the schedule publishes the system it is
about to run: `w->sched.running = { index, label }` before every call (the profiler auto-scope
needs the same field). `script_trampoline(World* w)`: `vm = w->script_sim`; `sys = vm->systems
[desc_of(running).ordinal]` (the desc carries the ordinal in `flags >> 16`); `vm->running = sys`;
toggle `WorldView` readonly off, set `tick` (u64 → number; exact below 2^53) and `phase`, readonly
on; `lua_getref(L, fn_ref)`; push `WorldView`; `lua_pcall(L, 1, 0, errfunc)` where `errfunc` is
the traceback handler pushed once at VM creation (`lua_debugtrace` appended to the message).
Non-zero status: netcode/ship → `TL_FATAL("%s", msg)`; dev → `TL_LOG_ERR`, `script_pause(w)`
(the loop stops ticking; console opens with the trace; `script.resume`/`script.reload` continue)
and the remaining systems of the tick are skipped. `vm->running = NULL` after. The dev tier runs
the call through `lua_resume` on a C-created thread instead of `lua_pcall` so a breakpoint can
suspend it (§10.10); the netcode tier never creates that thread.

**`events.emit(T, init)`** — builds the POD as `ecs.add` does, `eq_emit`. **`events.read(T)`** —
iterator over `eq_read` (last tick's buffer, immutable): `(iter, proxy{UTAG_EVENT, dense=-1},
nil)`, yields one read-only proxy per row; `events.count(T)`.

**`input.frame(slot) → InputProxy`** (`UTAG_INPUT`: `{ const InputFrame* f; u64 tick; u8 slot; }`,
one allocation; scripts fetch it once per system): `__index` atoms `tick`, `pointer_x`,
`pointer_y` (raw `pos_t` bits), `slot`; `input.down(fr, a) input.pressed(fr, a)
input.released(fr, a) → boolean`, `input.value(fr, a) → -127..127` where `a` is an `Action`
handle (hot) or a name string (cold, hashed). `input.live_mask() → int`, `input.local_slot()`
(from the `PeerSlots` singleton). **`input.action(name, kind, class) → Action`** (init only;
`kind ∈ DIGITAL|ANALOG`, `class ∈ LATCHED|AXIS|EDGE`; `ActionId` dense, `>= MAX_ACTIONS` →
error) and **`input.bind(action, spec, opts?)`** (`spec` string per `INPUT.md` §2; `opts` keys
`deadzone` (string), `sensitivity` (number, converted to `f32` in `bind_input.cpp` — core, not
sim; binding data never reaches the sim), `socd`). Both live in `script/sim/input.luau` so the
action list is identical on every peer; `bind` data is per-machine and not fingerprinted.

**`alloy.*`** (`ALLOY.md` §9.2 — shapes; this is the v0 signature list, to be re-cut against an
ALLOY §14 when it exists). Every edit binding fills one `EditCmd` record (a `WIRE_STRUCT`) on
scratch and appends it to the tick's `EditBuffer` with order key `(tick, source_slot, seq)`:
`source_slot` = the peer slot for `move_intent`, else the game channel `MAX_PEERS`; `seq` =
`vm->edit_seq++` (reset per tick). Edits: `alloy.carve(brush)`, `alloy.stamp(brush)`,
`alloy.spawn_body(desc) → Body`, `alloy.spawn_particles(desc)`, `alloy.spawn_agent(desc) → Body`,
`alloy.spawn_plant(desc) → Plant`, `alloy.despawn(h)`, `alloy.impulse(region, detonate_tick)`,
`alloy.heat(region, quanta)`, `alloy.current(region, quanta)`, `alloy.constraint(desc) →
Constraint`, `alloy.constraint_break(h)`, `alloy.move_intent(slot, agent, intent)`. `brush`/
`desc`/`region`/`intent` are tables read by a per-shape reader that walks the record's field list
(the `TL_WIRE_STRUCT` table — same packer, same errors as `ecs.add`); fx fields are raw bits;
material/species references are names resolved through `data` ids at the call. Handles returned
by spawn are reserved now, realized at the pass boundary. Queries (pure, immediate):
`alloy.raycast(x0,y0,x1,y1, mask) → hit?, hx, hy, nx, ny, Body|nil`, `alloy.shapecast(desc)`,
`alloy.supported(b) → boolean`, `alloy.same_cavity(x0,y0,x1,y1) → boolean`,
`alloy.circuit_live(h) → boolean`, `alloy.medium_at(x,y) → species id, immersion q`,
`alloy.cavity_at(x,y) → Cavity|nil`, `alloy.light_at(x,y) → q`. UI VM: queries only.

**`data.*`** (read-only, every VM except data): `data.table(name) → DataTable`, `data.id(t, name)
→ int` (`SortedMap` lookup; unknown → error), `data.count(t)`, `data.row(t, id_or_name) →
RowProxy` (`UTAG_ROW`, the POD row through the schema's `FieldInfo`). Sugar such as
`data.material("granite")` is `script/lib/data.luau` over `data.row`.

**`log.*`:** `log.info(s) log.warn(s) log.err(s)` → `TL_LOG(level, "%s:%d %s", chunk, line, s)`
with `lua_getinfo(L, 1, "sl", &ar)`; tick-stamped by the sink. Formatting is script-side
(`string.format`). `log.debug` is a no-op function in netcode/ship (arguments still evaluate).

### 10.7 Per-tick sequence

Two C++ systems from `tl_script`, registered by the wiring file first and last so their positions
are fixed without `before`/`after`:

1. **`sys_script_begin`** (`FIRST`, first): `vm->budget_left = `script.budget_safepoints``;
   `vm->edit_seq = 0`; `lua_gc(L, LUA_GCSTEP, `script.gc_step_kb`)` (a `SIM` cvar; the step is
   inside the tick so its cost is measured by the `luau_gc_us` counter); dev: record
   `LUA_GCCOUNT`/`LUA_GCCOUNTB` as `heap_before`.
2. Trampolines run wherever the schedule put them (§10.6).
3. **`sys_script_end`** (`LAST`, last — after the checkpoint hash; the heap is not state, so
   position is irrelevant to the hash): dev only, behind cvar `script_leak_check` (default on):
   `lua_gc(L, LUA_GCCOLLECT, 0)`; `reachable = LUA_GCCOUNT*1024 + LUA_GCCOUNTB`; if `reachable >
   vm->last_reachable` then `vm->growth_ticks++` else `vm->growth_ticks = 0`; `vm->last_reachable
   = reachable`; `growth_ticks == SCRIPT_LEAK_TICKS (60)` → `TL_LOG_WARN` once per 600 ticks with
   the byte delta and the names of the globals/upvalues that grew (a `lua_next` walk over `_G` and
   the registered system closures' upvalues via `lua_getupvalue`, comparing table lengths to the
   previous walk — the "cheap heuristic" §0 promises; false positives are possible, silence is
   not). The full collect costs ~0.1 ms on the script-sized heap; the cvar turns it off for
   profiling runs.

Nothing in this sequence can change state; it exists for cost control and leak detection.

### 10.8 Reload (dev only — `reload.cpp`)

`script.reload` (console; sealed, tick-stamped, recorded in the replay log with the new
manifest's BLAKE2b so a replay can refuse a mismatch). Applied at the end-of-tick barrier:

1. **Refuse** when a lockstep session is active (`net_session_active(w)`) → `SCRIPT_ERR_LOCKSTEP`
   (the fingerprint would change mid-session). Refuse during the init phase.
2. **Compile** every file in the sim manifest with the on-load compiler (§10.9). Any compile
   error → log all of them, abort; the running VM is untouched.
3. **Tear down:** `lua_close(old)`; `pool_reset` (the VM's reserve is decommitted and reused).
   The old `LuauSystem` table is dropped; `world_unregister_systems(SYS_LUAU)` removes every
   trampoline descriptor; the schedule is rebuilt after step 5.
4. **Rebuild:** §10.2 from step 2 with `vm->reloading = true`.
5. **Init in re-registration mode.** `ecs.component(name, …)` computes `layout_hash =
   rapidhash(name_hash ‖ per field (name_hash, kind, offset, size, count))` — the same bytes the
   fingerprint uses — and looks the name up among registered components:
   - found, equal hash → bind to the existing column (no registration); the column's data is
     untouched.
   - found, different hash → **refuse**: error names the component and the first differing field
     ("use `script.reload --migrate`"); the whole reload aborts. With the world's VM gone, the sim
     stays paused with the error in the console until the next `script.reload` succeeds — state
     is intact because it was never in Luau.
   - not found → a new column (allowed; the registry order changes, which the fingerprint log
     reports). A previously declared component that is no longer declared → refuse (its column
     would be orphaned) unless `--migrate`.
   `ecs.system` re-registers; `input.action` must reproduce the same list (a changed action map
   is refused the same way — it is wire format).
6. **`--migrate`:** before step 3, the save encoder (`ASSETS-AND-DATA.md` §5) encodes every
   Luau-declared column into a dev arena (name-keyed, in memory); step 5 registers the new layouts
   unconditionally (old columns released); after init the decoder restores each component by name
   with the alias/default rules; a field whose kind changed is a refusal (as in a file load) —
   then the reload aborts *after* the world was torn down, so `--migrate` first writes a real save
   file to `pref_path/reload-backup.sav` and tells the user where it is.
7. Schedule rebuild (`ECS.md` §3), `session_fingerprint` recomputed and logged old→new
   (`BUILD.md` §5), `post_restore`-style barrier: proxies from the old VM cannot exist (the VM is
   gone). `data.reload` is the separate command from `ASSETS-AND-DATA.md` §3.5 and may be chained.

### 10.9 Bytecode pipeline

`tools/luauc --docs <out_dir>` also emits the **Luau binding reference** (`script/docs/<table>.md`, one
page per binding table: signature, argument checks, the C++ contract it maps to, VM availability)
from the same binding descriptor tables the C++ registers — generated and committed, never hand-
written, so the game-author docs cannot drift from the bindings (`CPP-SUBSET.md` §6).

`tools/luauc`: `luauc -O2 -g1 --root script --out out/luac --manifest out/luac/manifest.tsv
script/sim script/lib`. Inputs: the directories, walked recursively; file set = `*.luau`; **order
= bytewise-sorted path relative to `--root` with `/` separators** — this is the load order the
fingerprint uses. Per file: `lua_CompileOptions { optimizationLevel = 2, debugLevel = 1,
typeInfoLevel = 0, coverageLevel = 0, vectorLib = vectorCtor = vectorType = NULL, mutableGlobals =
NULL, userdataTypes = NULL }` → `luau_compile(src, len, &opts, &outsize)`; a compile error (the
returned bytecode starts with a `0` byte and carries the message) fails the tool with the path
and message. Output `<out>/<relpath>.luac` plus `manifest.tsv`: one line per file `relpath \t
blake2b256_hex \t byte_len`, in the same order, and a trailing line `luau_version \t <vendor
commit from vendor/VERSIONS> \t options \t -O2 -g1`. `--check` recompiles and compares against an
existing manifest (CI: the dev on-load compile and the build-time compile must be byte-identical).

`build_id` (`BUILD.md` §5) consumes the manifest: `tools/fingerprint` hashes the `.luac` bytes in
manifest order (verifying each file's BLAKE2b first). The `netcode`/`ship` exes embed the
bytecode as a generated TU (`const u8 TL_LUAC_<n>[]` + a `{ relpath, ptr, len }` table in
manifest order). Dev: `loader.cpp` reads `script/sim/**` + `script/lib/**` at startup, sorts the
same way, compiles with the identical `lua_CompileOptions` (the struct is one `constexpr` shared
by `luauc` and `loader.cpp` through `src/script/compile_opts.h`), and computes the manifest
in memory; the dev log prints the would-be `build_id` contribution so a dev build and a netcode
build of the same tree print the same value.

**Loading:** `luau_load(L, chunkname, data, size, 0)` with `chunkname = "=" + relpath`
(`=script/sim/systems/regen.luau` — the `=` makes Luau print it verbatim in tracebacks). Non-zero
return → the error string is on the stack → `SCRIPT_ERR_LOAD` with it. The main closure is kept:
`vm->chunks: Map<NameHash(relpath), int ref>` (for §10.10 breakpoints). `require(relpath)`
(init only): looks the path up in the manifest table (unknown path → error, never a filesystem
read at runtime), loads once, `lua_pcall`s it, caches the returned value in `vm->modules`, returns
it. The entry is `main.luau`; a manifest file never `require`d is a warning (it is still in the
fingerprint). Bytecode-version mismatch between the vendored compiler and VM is impossible by
construction (same tree); `luau_load` still reports it as a load error.

### 10.10 Debugger hooks (Tier 0/1 — `debug.cpp`, `#if TL_DEV`)

- **Tier 0:** `log.*` (§10.6); error traces from the trampoline's `errfunc` (`lua_debugtrace`);
  the Replay panel (`TOOLING.md` §7) — nothing Luau-specific.
- **Tier 1:** `lua_breakpoint(L, funcidx, line, enabled)` on the chunk's main closure (`vm->
  chunks`; Luau walks nested protos, so a line anywhere in the file resolves; the returned line is
  the one actually set — the editor gutter moves to it). `lua_callbacks(L)->debugbreak =
  on_break` fires on hit: it records `(chunk, line, level)` and calls `lua_break(L)`, which makes
  the in-flight `lua_resume` (§10.6 dev trampoline) return `LUA_BREAK`; the trampoline sets
  `w->script_paused = true` and returns; the frame loop keeps rendering and ImGui, and stops
  ticking. **Step:** `lua_singlestep(L, 1)` + `cb->debugstep = on_step` (same break path on the
  next line; "step over/out" compare `lua_stackdepth`); **resume:** `lua_singlestep(L, 0)` and
  `lua_resume(thread, NULL, 0)` from the loop, continuing the interrupted system, then the rest of
  the schedule. **Inspect:** `lua_getinfo(L, level, "nsl", &ar)` per frame, `lua_getlocal(L,
  level, n)` / `lua_getupvalue(L, funcidx, n)` for the panel, values rendered through the same
  kind switch as the inspector (a proxy shows its fields). `cb->debuginterrupt` is unused (no
  script-created coroutines). Netcode/ship: `debug.cpp` is not compiled, the trampoline uses
  `lua_pcall`, no thread exists; `lua_breakpoint` is never called. Tier 2 stays rejected (§7).

### 10.11 Tests (`tests/script/*.test.cpp`, tag `script`)

| Test | Asserts |
|---|---|
| `sandbox_removed_globals` | every name in §10.2 step 4 is `nil` in the sim VM; `pairs` works in the UI VM; `os` is absent from the data VM |
| `sandbox_readonly` | after init: global assignment, `rawset(_G, …)`, `fx.x = 1`, `setmetatable(proxy, …)` all raise |
| `sandbox_tostring` | `tostring({})` == `"table"`; `string.format("%s", {})` == `"table"` |
| `sortedpairs_order` | keys `{3, "b", 1, "a", 2}` iterate `1,2,3,"a","b"`; non-integer/table key → error; deletion mid-walk skips |
| `fx_literals` | `fx.pos(12.5)` == `12.5 × 2^18`; `fx.pos(0.1)` errors; `fx.q(2)` errors (range); `fx.raw(2^31)` errors; `fx.mul_q` vs `mul<pos_t>` over a seeded sweep (`TL_EXPECT_EQ` on raw bits) |
| `fx_argument_checks` | every `fx.*` rejects `1.5`, `"1"`, `nil`, out-of-range — generated from the binding table |
| `proxy_rw_per_kind` | a component with one field of every kind; write via proxy, read via `world_get<T>`; handles round-trip as tagged lightuserdata; `nil` writes a null handle; array via `get_at/set_at`; `i64` > 2^53 read errors |
| `proxy_stale` | a proxy kept in an upvalue across a tick errors on next access with the component name |
| `each_order_and_count` | `ecs.each` visits packed order 0..n−1, identical to `world_column` iteration; spawns during the loop are invisible until the barrier |
| `system_ordering_parity` | a Luau system and a C++ system with the same `before/after` declarations produce the same schedule position; a Luau-declared cycle → `TL_TEST_EXPECT_FATAL` |
| `component_packer` | `{i8, i32, i8[3], Entity}` → offsets `0,4,8,12`, explicit `_pad` fields, `sizeof == 16`, layout hash equals the C++ `TL_COMPONENT` of the same fields |
| `dual_sim_two_vms` | `TL_ASSERT_DETERMINISTIC` with the regen scene: two worlds, two sim VMs, identical per-column hashes 600 ticks; then a deliberately stateful script (upvalue table) diverges |
| `budget_trip` | `while true do end` → netcode child process: fatal with `"script budget exceeded"`; dev: `script_paused` set, the tick's remaining systems skipped |
| `memory_exhaustion` | a 1 MB pool + `table.create(1e6)` → `"not enough memory"` on the same error path; the pool counter returns to baseline after `lua_close` |
| `reload_same_layout` | edit a system's body, `script.reload`: column data intact, new behaviour next tick, `session_fingerprint` unchanged |
| `reload_layout_change` | add a field: plain reload refused with the component name; `--migrate`: data restored by name, new field at default, backup save exists |
| `reload_in_lockstep_refused` | with a fake active session → `SCRIPT_ERR_LOCKSTEP` |
| `bytecode_identity` | `luauc` output for the fixture tree `MEM_EQ` the dev on-load compile, file by file; manifest hashes match |
| `events_roundtrip` | `events.emit` in tick N visible to `events.read` in N+1 only, cleared in N+2 |
| `input_proxy` | Script producer presses `jump` at tick 10: `input.pressed` true at 10, `down` true at 11, `released` at the release tick |
| `interner_atoms` | a registered field name gets a non-negative atom; an unregistered string gets `-1` and the proxy error names it |

### 10.12 Build order and done criteria

1. `vm.cpp` + `sandbox.cpp` + `handles.h` with `sandbox_*`, `sortedpairs_order` — a VM that runs
   a string. 2. `bind_fx.cpp` + `fx_*` tests. 3. `proxy.cpp` + `bind_ecs.cpp` (components,
   packer, each/get, commands) + proxy/packer/each tests. 4. `trampoline.cpp` + the bracket
   systems + `system_ordering_parity`, `budget_trip`, `memory_exhaustion`, `dual_sim_two_vms`.
   5. `bind_events.cpp`, `bind_input.cpp`, `bind_data.cpp`, `bind_log.cpp` + their tests.
   6. `tools/luauc` + `loader.cpp` + `bytecode_identity`; `build_id` wired. 7. `bind_alloy.cpp`
   (lands with Alloy's edit buffer). 8. `reload.cpp` + reload tests. 9. `bind_ui.cpp`, `debug.cpp`
   (with the Scripts panel, `TOOLING.md` §1).

**RR-18, ruled 2026-08-26 (Rafael) — the in-process compile works in every tier.** Luau's
*Compiler* exposes no allocator hook and allocates with global `operator new` (measured: 32 calls
per `luau_compile`; the *VM* makes zero — it is fully pooled). `MEMORY.md` §1.5 records the answer:
the **shared vendor pool** (`PLATFORM.md` §9.5's `pool_vendor`) serves those allocations for the
duration of one compile and nothing else in the process is inside that window. §10.9's dev on-load
compile is unblocked.

`ScriptVmDesc.compile_pool` is that pool, and it is **required** — a null one is
`SCRIPT_ERR_BAD_ARG` at creation, never a fall back to the VM's own pool (the binding the D2
ruling removed, 2026-08-26). Before the window opens, `load_chunk` requires
`SCRIPT_COMPILE_HEADROOM_MIN` (256 KB) `+ 128 B per source byte` of headroom and refuses with
`SCRIPT_ERR_COMPILE` otherwise; both constants are derived from the measured cost (90.66x the
source size at 1 KB, 49.83x at 64 KB, ~88 KB floor) with ~3x margin on the floor and ~1.4x on the
worst ratio. The refusal exists so the `TL_FATAL` in `vendor_new.cpp` stays what it is meant to be
— a genuine bug — instead of the only outcome available for an ordinary large source.

**Done** when: every §10.11 test passes in `dev` and `netcode` (fatal-expected ones in child
processes); the symbol audit shows `lua_*`/`luau_*` symbols only in `tl_script` (`tools/audit/symbols.py --wrap-lib`, built 2026-08-26 — it checks DEFINED as well as undefined names, because a hand-written `extern` needs no header and is precisely the shape the include firewall cannot see); no Luau header outside `src/script/` (`tools/audit/includes.py`'s `SYS_ALLOW_DIRS` + `BACKEND_HEADERS`, both halves with their own planted violations); `TL_ASSERT_NO_TICK_ALLOC` holds for a tick with Luau
systems running (pool allocations are inside the Luau pool, outside the registered set, and the
guard watches registered arenas + scratch only — the pool counter is asserted separately to be
flat across a steady-state tick after the GC step); the regen scene is bit-identical across the `CANON.md`
target matrix (the Pi left the program 2026-08-25; the four hosted CI legs carry the cross-ISA
evidence).

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
