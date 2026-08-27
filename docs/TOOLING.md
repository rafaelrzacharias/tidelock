# Tooling — editor shell, inspector, console, profiler, probes, logging, crash (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §10. Harvests Layr's born-instrumented
> discipline (`FOUNDRY-EXTRACTION.md`, `LAYR-ANALYSIS.md` §4) and ENGINE-DESIGN F1's dev-tooling
> half, on Dear ImGui. Built **right after v0** (dev surface early, not at "layer 10").
> **Owns:** `src/editor/` (dev tiers only; compiled out of `netcode`/`ship` by absence) and the
> macro headers `foundation/tl_{assert,log,prof,probe}.h` (all tiers; tier-gated expansions).
> Implementation spec: §9.

---

## 0. Principles

- **Zero cost by absence.** Every probe/log/profile call site is a macro that compiles to nothing
  (argument evaluation included) outside the tier that enables it. No runtime `if (dev)`.
- **One substrate, five views.** The ECS reflection tables + the command channel + event queues
  power the inspector, console/cvars, watches, probes, and desync diffs. Not five systems.
- **Tools never poke sim state.** Every mutation is a command applied at a barrier
  (`ECS.md` §4); every sim-affecting console command is a sealed, tick-stamped command. That is
  what keeps an edited session replayable and a lockstep session honest.
- **ImGui is dev UI only.** Game UI is Luau over render2d primitives (`RESERVED-SEAMS.md` §2).

---

## 1. The shell (DECIDED)

Dear ImGui (SDLRenderer3 backend at v0; docking branch), one dockspace, panels registered in a
table (`{ name, draw_fn, default_open }`) — menus are data. Panels: **Inspector**, **Console**,
**Log**, **Profiler**, **Probes**, **Replay**, **Scripts** (ImGuiColorTextEdit + Tier 0/1 debugger),
**World** (entity list, singleton components, arena registry with sizes/hashes), **Sim** (Alloy
views: chunk dirty map, island list, cavity graph, per-pass timings), **Net** (Hovel: peers, RTT,
quorum, epoch, log, impairment shim controls). Layout persisted in `pref_path`. Editor input reads
the raw event stream at render rate and masks what it captures from the Live producer
(`INPUT.md` §5). **Dragging a panel out into its own OS-level window is out of scope for v0** —
docking *within* the one OS window is unaffected, but see the corrected R-2 in §10: SDL_Renderer
cannot back ImGui multi-viewport, so pop-out is deferred to a post-v0 backend migration.

---

## 2. Inspector (DECIDED)

The generic walker: for a selected entity, every component's `FieldInfo` table → a widget per
kind (fx rows show the decimal value *and* raw bits; handles show name + generation + a "go to"
link; `StrId` shows the interned name). Edits are queued as `world_set_field` commands. Optional
per-component custom-draw hook (curve editors etc.) and per-system `debug_draw(World*)` for
overlays — hooks are the override, the walker is the mechanism. Singletons and Luau-declared
components appear identically. Luau hooks (UI VM) may add custom draws.

---

## 3. Console, cvars, commands (DECIDED)

- **Command registry:** `console_register("spawn", fn, "spawn <name> <x> <y>", arg_hints)` from
  C++ and `console.command(...)` from Luau (UI VM); history; tokenizer; per-arg completion; a
  command returns a `Result` (never error-by-scanning-output). Sim-affecting commands are emitted
  as sealed commands.
- **Cvars:** a C++-registered reflected table (`TL_CVAR(f32, render_zoom, 1.0f, 0, "…")`) with flags
  `ARCHIVE | CHEAT | READONLY | SIM` — a `SIM` cvar is part of the fingerprint (it changes sim
  behaviour; a lockstep session refuses a change).
- **Luau REPL** in the UI VM (read-only world access + commands). Dot-path introspection
  (`player.Transform.x`) reuses the reflection walker.
- **Watches:** `watch <path>` renders a live overlay; bound through field tables, not strings
  per frame.

---

## 4. Logging (DECIDED)

`TL_LOG(level, "fmt", ...)` → stb_sprintf into a fixed buffer → sinks: memory ring (feeds the
Log panel and the crash report), stderr (soaks read it live), file. Per-sink levels; every line
is **tick-stamped** (sim tick, not wall-clock, so two peers' logs align) plus a wall-clock stamp
for soaks. Compiled out by level per tier. No heap.

---

## 5. Profiler and probes (DECIDED — hand-rolled; Tracy excluded: C++ client compiled into the runtime, unpublished protocol)

- **Scopes:** `TL_PROF_SCOPE("alloy.pass3")` macro (no RAII — explicit `begin/end` pair, the
  macro expands both around a block); hierarchical tree per frame; pre-seeded keys so steady state
  allocates nothing; a **job/chunk id field** so a task is traceable across workers; 60-frame
  ring; trace-to-disk for deep sessions as **Chrome trace-event JSON** (loads in Perfetto and
  speedscope, zero deps). Every system and every Alloy pass is auto-scoped by the scheduler.
  Counters (`draw_calls`, `particles_awake`, `luau_gc_us`). GPU timing: none at v0 (SDL_Render
  exposes nothing); fence-timed whole-submission later behind the `gpu` wrap.
- **Probes:** `TL_PROBE_LOG(key, value, every_n_ticks)`, `TL_PROBE_ON_CHANGE(key, value, eps)`,
  `TL_PROBE_MARK(key)`, `TL_PROBE_ASSERT(key, value, lo, hi)` — throttled by **tick count, never
  wall-clock**; TSV sink with per-key summary at shutdown (count/changes/min/max/mean/first/last);
  named probe profiles toggled from the console. A disabled probe is a branch; an enabled one a
  field read; outside dev it is nothing.
- **Desync diff:** given two snapshots (or a snapshot and a live world), walk every arena's
  reflection tables and print the first N differing fields — the `DETERMINISM.md` §7 workflow's
  step 3, as a panel and a CLI.

---

## 6. Crash pipeline (DECIDED)

Hand-rolled, pure C: Windows SEH filter → spawn a separate dumper process → `MiniDumpWriteDump`;
Linux signal handlers + a pre-reserved alloc-free path. The report carries: build fingerprint,
log-ring tail, sysinfo, world census (entity/particle/body counts), last profiler frames, **and the
latest snapshot + the inputs since** — a crash report is an auto-repro, not a screenshot of a
corpse. `TL_FATAL` routes through the same path.

---

## 7. Replay and time travel (DECIDED — Tier 0)

The **Replay** panel drives the `Replay` producer with keyframes (snapshot every N ticks into a
dev arena): scrub bar, play/pause/speed, seek = nearest keyframe + re-sim forward (bit-exact by
construction). Pairs with the inspector: scrub to a tick, inspect any field. This is why Tier 2
script debugging is not needed. Record is always on in dev (ring-bounded); "save replay" writes
the `RecordedInput` file + keyframes.

---

## 8. Sim-specific tools

Float-shadow toggle (dev builds with the shadow solver): per-pass max error column in the Sim
panel. Impairment shim controls (Hovel). Arena registry view with per-tick hashes and the
arena-offset guard's last report. Luau VM panel: pool usage, GC step time, instruction budget.

---

## 9. Implementation specification

Scope: the four macro headers, the runtimes behind them, `src/editor/`, the crash and replay
pipelines, tests. `f32`/`f64` are legal in `editor/` and in the non-det foundation runtimes
(`log.cpp`, `prof.cpp`, `probe.cpp`); the macro *headers* contain no float token, though only
`tl_assert.h` is the one a sim TU may actually include (`CPP-SUBSET.md` §9 R-3) - `tl_log.h`/
`tl_prof.h`/`tl_probe.h` stay barred by the non-det-stem rule like any other tooling header. Those
runtimes' real io and their ring/frame/key-table state are the named exception of `CPP-SUBSET.md`
§1 and §9 R-4 (RR-7): the implementation stems listed on `TL_FOUNDATION_TOOLING`
(`src/foundation/CMakeLists.txt`, the one home - not restated here) are the only non-det stems
allowed writable static storage and `<stdio.h>`/`<stdlib.h>`/`<stdarg.h>`, because none of them is
hashed, snapshotted, or part of a world's registered arena set. Tier gates: `TL_DEV` (1 in
`dev`/`debug`, 0 otherwise, `BUILD.md` §3) and `TL_LOG_MIN` (`debug`/`dev` 0, `netcode` **and**
`ship` 2). **`netcode` and `ship` share one compiled floor, INFO+** (ruled 2026-08-24, `TODO.md`):
the earlier `ship` 3 made the two tiers compile different code, which is the thing `BUILD.md` §3's
parity rule exists to prevent; unifying them dissolves the tension instead of exempting it, so §3
stays absolute and `tools/audit/tier_parity.py` gains no allowed define. `ship` quiets further at
**runtime** through the log-level cvar (§3 - a non-`SIM` cvar). `TL_LOG_MIN` is still **derived
from the tier markers inside `tl_log.h`, never passed as its own `-D`**, so no define enters the
tier delta at all. Left undefined it is silently 0 to the preprocessor - which is what W1
tooling-rt first shipped, compiling `TL_LOG_TRACE` into `ship`. Nothing here is hashed; every
mutation of sim state is a command (§0).

### 9.1 File layout

| File | Contents | Tiers |
|---|---|---|
| `foundation/tl_assert.h` | `TL_ASSERT` (debug/dev), `TL_CHECK` (all), `TL_FATAL` (all) → `tl_fatal(file, line, msg)` → crash writer (§9.3.9) → `platform.crash.raise_fatal` | all |
| `foundation/tl_log.h` + `log.cpp` | `TL_LOG_{TRACE,DEBUG,INFO,WARN,ERR}` → `tl_log_write`; ring + stderr + file sinks | all (levels gated) |
| `foundation/tl_prof.h` + `prof.cpp` | `TL_PROF_SCOPE/BEGIN/END/SCOPE_W`, `TL_PROF_COUNTER_SET/ADD`; per-worker node buffers, frame ring | `TL_DEV` only (`prof.cpp` not built otherwise) |
| `foundation/tl_probe.h` + `probe.cpp` | `TL_PROBE_LOG/ON_CHANGE/MARK/ASSERT` (+ `_FX` variants); key table, TSV sink, summary | `TL_DEV` only |
| `core/cvar.h/.cpp` | `TL_CVAR`, `CvarTable` in `World`, `cvar_get_*`, `cvar_set` (command-routed when `SIM`) | all |
| `core/crash_report.cpp` | the writer: assembles the report into the crash arena, registered with `platform.crash.install` | all |
| `core/desync_diff.cpp` | the reflection diff walker (§9.3.8); CLI via `tl_driver --diff` | all |
| `editor/editor.h` | `Editor` (panel table, selection, capture mask, dev arena), `editor_init/frame/shutdown` | dev |
| `editor/shell.cpp` | ImGui context, dockspace, panel registry, `imgui.ini` in `pref_path`, capture-mask publish | dev |
| `editor/inspector.cpp` | the walker (§9.3.4) | dev |
| `editor/console.cpp` | registry, tokenizer, completion, history, cvar UI, Luau REPL hand-off | dev |
| `editor/dotpath.cpp` + `watch.cpp` | path resolution (§9.3.6), watch overlay | dev |
| `editor/log_panel.cpp` `profiler_panel.cpp` `trace_export.cpp` `probes_panel.cpp` `world_panel.cpp` `sim_panel.cpp` `net_panel.cpp` `scripts_panel.cpp` | one panel each; `net_panel` builds only when `tl_net` is linked | dev |
| `editor/replay_panel.cpp` + `keyframes.cpp` | keyframe ring, scrub (§9.3.10) | dev |

Macro expansions (the gate is the preprocessor, so arguments are never evaluated when off):
```cpp
// tl_log.h
enum LogLevel : u8 { LOG_TRACE = 0, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERR, LOG_FATAL };
void tl_log_write(u8 level, const char* file, u32 line, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
#if TL_LOG_MIN <= 0
#  define TL_LOG_TRACE(...) tl_log_write(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#else
#  define TL_LOG_TRACE(...) ((void)0)
#endif                                   // same pattern per level; TL_LOG_ERR always on; TL_LOG(level, ...) dispatches by constant level
// tl_prof.h
#if TL_DEV
#  define TL_PROF_BEGIN(lit)            tl_prof_begin(0, lit##_id, lit, 0xFFFFFFFFu)          // worker 0 = main thread, by contract
#  define TL_PROF_END()                 tl_prof_end(0)
#  define TL_PROF_SCOPE(lit)            for (u32 _tl_ps = (TL_PROF_BEGIN(lit), 0u); _tl_ps == 0u; TL_PROF_END(), _tl_ps = 1u)   // a block, no RAII; `return` inside is a review error (debug asserts depth == 0 at frame end)
#  define TL_PROF_SCOPE_W(scr, lit, job) for (u32 _tl_ps = (tl_prof_begin((scr)->worker, lit##_id, lit, (job)), 0u); _tl_ps == 0u; tl_prof_end((scr)->worker), _tl_ps = 1u)
#  define TL_PROF_COUNTER_SET(lit, v)   tl_prof_counter(lit##_id, lit, (i64)(v), 0)
#  define TL_PROF_COUNTER_ADD(lit, v)   tl_prof_counter(lit##_id, lit, (i64)(v), 1)
#else
#  define TL_PROF_BEGIN(lit) ((void)0)  /* …every macro → ((void)0); TL_PROF_SCOPE(lit) → nothing (the block still compiles as a plain block) */
#endif
// tl_probe.h — integer-only at the call site so sim TUs stay float-free; conversion happens in probe.cpp
#if TL_DEV
#  define TL_PROBE_LOG(lit, v, n)        tl_probe_log(lit##_id, lit, (i64)(v), 0, (u32)(n))
#  define TL_PROBE_LOG_FX(lit, fx, n)    tl_probe_log(lit##_id, lit, (i64)(fx).v, (u8)decltype(fx)::FRAC, (u32)(n))
#  define TL_PROBE_ON_CHANGE(lit, v, eps) tl_probe_on_change(lit##_id, lit, (i64)(v), (i64)(eps))
#  define TL_PROBE_MARK(lit)             tl_probe_mark(lit##_id, lit)
#  define TL_PROBE_ASSERT(lit, v, lo, hi) tl_probe_assert(lit##_id, lit, (i64)(v), (i64)(lo), (i64)(hi))
#else
#  define TL_PROBE_LOG(lit, v, n) ((void)0)   /* … */
#endif
// tl_assert.h
#define TL_FATAL(msg)        tl_fatal(__FILE__, (u32)__LINE__, (msg))                   // all tiers, [[noreturn]]
#define TL_CHECK(c)          ((c) ? (void)0 : tl_check_failed(__FILE__, (u32)__LINE__, #c))  // all tiers
#if TL_DEV
#  define TL_ASSERT(c)       ((c) ? (void)0 : tl_assert_failed(__FILE__, (u32)__LINE__, #c)) // each tier its own R-3 symbol (CPP-SUBSET.md §9), so the report names what fired
#else
#  define TL_ASSERT(c)       ((void)0)
#endif
```
`Scratch` carries no worker field: jobs passes `Scratch*` explicitly (`JOBS.md` §0), and the
per-worker prof/probe buffers that once motivated one are not built — if that consumer ever
lands, IT files for the field (ruled 2026-08-26; pulled by a consumer, never pushed on spec).
The probe/prof runtimes are `tl_*` symbols in the
non-det `tl_foundation` lib — the symbol audit's "own `tl_*` symbols" allowance covers the sim
lib's references to them in `dev`; in `netcode` the macros are empty and no symbol exists.

### 9.2 Structs

```cpp
// log.cpp
struct LogRecord { u64 tick; u64 wall_ms; const char* file; u32 line; u8 level; u8 len; u16 _pad0; char msg[224]; };  // 256 B
struct LogState  { RingBuffer<LogRecord> ring /*4096 slots, overwrite-oldest, dev arena*/; u8 min_level[3] /*ring, stderr, file*/;
                   u8 file_enabled; u16 _pad0; u32 staging_used; char staging[65536]; const u64* tick_ptr; ClockApi clock; FileApi file; StrView file_path; };
// prof.cpp
struct ProfNode  { u64 t_begin, t_end; NameHash key; const char* name; u32 parent; u32 job_id; u16 depth; u8 worker; u8 _pad0; u32 _pad1; };  // 48 B
struct ProfWorker { ProfNode nodes[8192]; u32 count; u32 stack[64]; u32 depth; u32 overflow; };          // per worker, 393 KB; overflow counts dropped scopes
struct ProfFrame { u64 frame; u64 tick; u64 t_start, t_end; u32 node_count; u32 dropped; i64 counters[256]; ProfNode nodes[16384]; };  // merged in worker order; nodes past 16384 are dropped and counted; 788,520 B
struct ProfState { ProfFrame ring[60]; u32 head; u32 count; ProfWorker workers[16]; ProfCounter counters[256]; u32 counter_count; u8 paused; u8 _pad0[3]; };  // ring ≈ 47.3 MB + workers ≈ 6.3 MB, dev arena; static_assert(sizeof(ProfFrame) == 788520)
struct ProfCounter { NameHash key; const char* name; i64 value; };                                   // 24 B
// probe.cpp
struct ProbeKey  { NameHash key; const char* name; u64 count, changes; f64 min, max, sum, first, last; i64 last_raw; u64 last_tick; u8 enabled, frac_bits, kind; u8 _pad0[5]; };  // 96 B
struct ProbeState { ProbeKey keys[1024]; Map<NameHash, u16> index; u32 count; u8 profile_mask; u8 _pad0[3]; char staging[65536]; u32 staging_used; StrView tsv_path; const u64* tick_ptr; };
// TSV row: "<tick>\t<key>\t<value>\n"   value = raw (frac 0) or "%.9g" of raw·2^-frac; MARK → empty value field
// summary (at shutdown, after a "#summary" line): "<key>\t<count>\t<changes>\t<min>\t<max>\t<mean>\t<first>\t<last>\n", keys in registration order
// cvar.h
enum CvarKind : u8 { CVAR_I32, CVAR_U32, CVAR_F32, CVAR_BOOL, CVAR_FX_RAW };
enum : u8 { CVAR_ARCHIVE = 1, CVAR_CHEAT = 2, CVAR_READONLY = 4, CVAR_SIM = 8 };
struct CvarDesc  { NameHash key; const char* name; const char* help; u32 default_bits; u8 kind; u8 flags; u8 frac_bits; u8 _pad0; };  // 32 B, constexpr
struct CvarTable { const CvarDesc* desc[256]; u32 bits[256]; u16 sorted[256] /*by key, built at init*/; u32 count; u32 _pad0; };   // in World (non-registered arena)
// `flags` is a parameter (amended 2026-08-27, w3-editor lane): the rev-1 expansion below hard-coded
// every cvar's flags to 0, silently contradicting this doc's own §3 ("with flags ARCHIVE | CHEAT |
// READONLY | SIM") and docs/CPP-SUBSET.md §7b's catalogue row, which already specified
// `TL_CVAR(type, name, default, flags, help)` - a doc-contradicts-doc bug the docs/CPP-SUBSET.md
// side was, in hindsight, the correct one; this section now matches it (docs/TODO.md carries the
// finding).
#define TL_CVAR(type, name, def, flags, help) constexpr CvarDesc CVAR_##name = { #name##_id, #name, help, cvar_bits_of<type>(def), cvar_kind_of<type>(), (flags), 0, 0 };
// modules list their CvarDesc* in a constexpr array; app/ registers the arrays in wiring order; lookup = binary search on key
// console.cpp
typedef Result<u32> (*ConsoleFn)(World*, u32 argc, const StrView* argv, Span<char> reply);   // value = bytes written to reply
struct ConsoleCmd { NameHash key; const char* name; const char* usage; ConsoleFn fn; const char* arg_hints[4]; u8 flags /*bit0 SIM_AFFECTING*/; u8 argc_min, argc_max, from_luau; u32 lua_ref; };  // 72 B; table 512, name-sorted u16 index for completion
// crash (core/crash_report.cpp) — written into a dedicated 16 MB crash arena committed at init; no allocation on the crash path
struct CrashHeader { u32 format_version; u32 section_count; u8 build_id[32]; u8 session_fingerprint[32]; u64 tick; u64 wall_ms;
                     u32 os, isa, reason /*1 FATAL · 2 SEH · 3 SIGNAL*/, code; u64 fault_addr; char msg[256]; };   // 368 B, TL_WIRE_STRUCT
struct CrashSection { u32 kind; u32 byte_len; };   // followed by payload, 8-aligned
// kinds / budget (16 MB total): 1 LOG_TAIL last 256 records (64 KB) · 2 SYSINFO ≤ 1 KB · 3 CENSUS ≤ 16 KB (entity count, per-component counts, particle/body/cavity/basin counts)
// · 4 PROF_FRAMES last 4 frames (≤ 3 MB) · 5 SNAPSHOT latest ring slot (≤ 12 MB, else omitted + flag bit in header.code) · 6 INPUTS_SINCE InputFrame[MAX_PEERS] per tick since the snapshot (608 B/tick)
// · 7 CALLSTACK 64 × u64 + module table (≤ 4 KB) · 8 DUMP_PATH (Windows .dmp path). File: <pref_path>/crash/<build_id hex8>_<wall_ms>.tlcrash
// keyframes.cpp
struct Keyframe { u64 tick; u64 world_hash; u32 slot; u32 input_offset; };                       // 24 B
struct KeyframeRing { Keyframe frames[64]; u32 head, count; u32 interval_ticks /*cvar replay_keyframe_ticks = 60*/; u32 _pad0; Snapshot* slots /*64, dev arena, sized as the rollback ring*/;
                      RingBuffer<InputFrame> inputs /*MAX_PEERS × 36000 ticks*/; u64 input_base_tick; };
```

### 9.3 Algorithms

**9.3.1 Profiler.** `tl_prof_begin(worker, key, name, job)`: `w = workers[worker]`; if
`w.count == 8192` → `w.overflow++`, push sentinel `0xFFFFFFFF` on the stack, return; else
`n = w.count++`; `nodes[n] = { ticks(), 0, key, name, parent = depth ? stack[depth−1] : NONE, job, depth, worker }`;
`stack[depth++] = n`. `tl_prof_end(worker)`: `n = stack[--depth]`; if `n != sentinel`:
`nodes[n].t_end = ticks()`. Frame end (`render_present` step 7 calls `tl_prof_frame_end(tick)`):
merge every worker's nodes into `ring[head]` in worker order, rebasing `parent` by the worker's
offset; sample counters; `head = (head+1) % 60`; reset worker counts; `TL_ASSERT(workers[0].depth == 0)`.
Every system is scoped by the scheduler (`tl_prof_begin(0, desc.label, name, 0)`), every Alloy
pass by `alloy_step`, every `parallel_for` chunk by the job system (`job = chunk`). Steady state:
zero allocation (fixed buffers), two `ticks()` reads per scope.

**9.3.2 Chrome trace export** (`trace_export.cpp`, `profiler dump <path> [frames]`): for each ring
frame oldest→newest, for each node: one complete event; `ts`/`dur` in microseconds (`f64`),
`ts = (t_begin − ring_t0) · 1e6 / frequency`:
```
{"traceEvents":[
 {"name":"frame","cat":"loop","ph":"X","ts":0.0,"dur":16612.3,"pid":1,"tid":0,"args":{"frame":1234,"tick":4567}},
 {"name":"alloy.pass3","cat":"sim","ph":"X","ts":812.4,"dur":3901.0,"pid":1,"tid":3,"args":{"job":17,"tick":4567}},
 {"name":"draw_calls","cat":"counter","ph":"C","ts":0.0,"pid":1,"args":{"draw_calls":61}},
 {"name":"thread_name","ph":"M","pid":1,"tid":0,"args":{"name":"main"}}   // one per worker: "worker N"
],"displayTimeUnit":"ms"}
```
`cat` = the key's prefix before the first `.`; `tid` = worker. Written with `fmt_buf` into a 1 MB
staging buffer, flushed by `file.append`; loads in Perfetto and speedscope unchanged.

**9.3.3 Probe throttle.** `tl_probe_log(key, name, raw, frac, n)`: `k = lookup-or-insert(key)`
(insert: fatal at 1024); if `!k.enabled` return (a disabled probe is this one branch);
`tick = *tick_ptr`; if `k.count == 0 || tick − k.last_tick >= n` → row + stats update
(`v = frac ? raw·2^-frac : raw`; `min/max/sum/first/last`, `changes += (raw != last_raw)`, `count++`,
`last_tick = tick`). `ON_CHANGE`: row when `|raw − last_raw| > eps` or first. `MARK`: row
every call. `ASSERT`: if `raw < lo || raw > hi` → row + `TL_LOG_ERR`, and `TL_FATAL` when cvar
`probe_assert_fatal` (default 0 in dev, 1 in the driver). Tick-throttled only — never wall-clock.
`--dump-probes <path>` (§10 R-3) sets `tsv_path`; the panel reads the same `ProbeKey` table.

**9.3.4 Inspector walker** (`inspector.cpp`, per frame, single selection; built W3 — this section
was rewritten against the shipped code, not the other way around, once the earlier pseudocode's
names/shapes were found to predate several `ECS.md` reconciliations):
```
for c in 0..w->comp_count: info = w->comps[c].info; if info.flags & COMP_HIDDEN: continue
  if info.flags & COMP_SINGLETON: row = w->comps[c].dense; entity = null
  else: if sel is null: continue; row = column_get(&w->comps[c], sel); if !row: continue; entity = sel
  if !CollapsingHeader(info.name, ImGuiTreeNodeFlags_DefaultOpen): continue
  for fi in 0..info.field_count: f = info.fields[fi]; elems = f.count (never 0); esz = kind_scalar_size(f.kind)
    for k in 0..elems: addr = row + f.offset + k*esz; PushID(fi*256+k); label = elems > 1 ? "name[k]" : name
      editable = (elems == 1) && (kind is an integer K_i8..K_u64, K_bool, or a K_pos..K_scalar palette row)
      switch f.kind:
        K_i8..K_u64: if editable: tmp = load; InputScalar(ImGuiDataType per kind, &tmp); if deactivated-after-edit: inspector_set_scalar_field(w, lockstep, entity, c, fi, &tmp, esz); else (count > 1): draw_int_readonly(addr, kind) — a plain read-only Text at the field's own signed/sized type (B-6, 2026-08-27: the widget itself is gated, not only the write — an earlier revision drew InputScalar unconditionally and gated only inspector_set_scalar_field's call, so editing an array element gave a live, typeable widget whose value silently reverted next frame)
        K_bool: if editable: Checkbox; same deactivated-after-edit call, 1-byte value; else: Text("true"/"false") — same B-6 gating
        K_pos..K_scalar (the nine palette rows): raw = load i32; shown = raw * 2^-FRAC(kind) as f64; Text("%.9g (0x%08x)", shown, raw) (read-only, dev-UI f64, unrelated to the parse-back path below); SameLine InputTextWithHint("new value") — RR-38/RR-39 (2026-08-27): if editable and deactivated-after-edit, parsed = fx::fx_parse_decimal_raw(buf, FRAC(kind)); if parsed.err == ERR_OK: inspector_set_scalar_field(w, lockstep, entity, c, fi, &parsed.value, esz) — a parse failure is a silent no-op (empty/malformed/out-of-range text), matching the console's "a rejected command doesn't mutate state" shape; the error text itself is not surfaced yet (no toast/status-line mechanism in this panel — known post-v0 gap, `TODO.md`)
        K_Entity / the other handle kinds: Text("%s #%u g%u", domain, idx, gen) (or "null"); K_Entity only also draws SmallButton("go") → ed->sel = that handle; DISPLAY ONLY, no edit widget (a handle edit needs its own resolution UI - "type an entity name" or "pick from the interner" - a different, unscoped feature, not a data-representability gap)
        K_StrId: Text(interner_name(id)) when w->interner is set, else "#%u" — read-only
      PopID
after the component loop: no custom-draw hook and no per-system debug_draw registry exist — neither is built; the generic per-field walk above is the whole panel at v0
```
`inspector_set_scalar_field(w, lockstep, e, comp, field_index, bytes, len)` (`inspector.h`) is the
one write path: refuses with `ERR_EDITOR_LOCKSTEP` before recording anything when `lockstep` is
true (hardcoded `false` at the only call site today — no netcode/Hovel session exists yet to ask,
matching `console_panel_draw`'s own note), else calls `world_set_field_cmd` (`core/world.h`) —
`core/commands.h`'s real `CMD_SET_FIELD` payload is `{ u32 field_index; bytes[field.size] }`, no
element index (`ECS.md` §10.5, not §4), so a single array-element write is not representable — the
same gap `editor/dotpath.cpp`'s `dotpath_set_raw` already documents and guards
(`TL_CHECK(f->count == 1u)`); the walker guards the same way, by never drawing an editable widget
for `count > 1` rather than drawing one that would write the wrong bytes. `world_set_field_cmd`
itself carries no lockstep concept — refusing is the caller's job. `COMP_HIDDEN` components are
skipped entirely (not shown, not drawn); `COMP_SINGLETON` components draw every frame regardless
of selection, with `entity` null in the write path (an edit call there is unreached at v0 — no
singleton test component in this tree currently declares an editable field, and nothing routes an
edit to one; `world_singleton_set_cmd` remains the wholesale-swap door for singleton writes,
untouched by this file).

**9.3.5 Console.** Tokenizer: split on ASCII space/tab; a `"`-quoted token keeps spaces and
honours `\"` and `\\`; `#` starts a comment; max 16 tokens (`ERR_CONSOLE_TOO_MANY_ARGS`);
unterminated quote → `ERR_CONSOLE_SYNTAX`. Dispatch (deviation from this section's original text,
recorded 2026-08-27, B-5): a bytewise binary search over `sorted` (the same name-ascending index
completion below already walks) resolves `argv[0]` by NAME - not `key = hash(argv[0])` into a
`SortedMap`, which this section originally specified and nothing ever built; the binary search is
strictly better (no hash, no collision class, no second index to keep in sync with `cmds`), so the
CODE is right and this text is corrected to match it, not the other way around. `ConsoleCmd::key`
still exists (`console.h`, computed from `name` at registration) but neither dispatch nor
completion reads it. `argc` checked against `argc_min/max`; `SIM_AFFECTING` commands in a lockstep session → refused;
otherwise `fn(w, argc−1, argv+1, reply)`; reply and `ERR_NAME(err)` go to the console log.
Completion: the name-sorted `u16` index, `lower_bound` on the typed prefix (bytewise), walk
while prefix matches (≤ 32 shown); for argument `i`, `arg_hints[i]` selects a source:
`"entity"` (names from the `Name` component via the interner), `"cvar"`, `"cmd"`,
`"file:<dir>"` (`file.enumerate`), `"enum:a|b|c"` (literal list), `null` (none). History: a
64-line ring in the dev arena, ↑/↓ walks it. Luau: `console.command(name, fn, usage)` registers
a `ConsoleCmd` with `from_luau = 1`, `lua_ref` the registry ref; the trampoline marshals `argv`
as strings. Cvars: `set <name> <value>` parses per kind (`FX_RAW` accepts `raw:<i32>` or a
decimal literal quantized RNE); `READONLY` refused; `SIM` → a sealed command (`CMD_SET_CVAR`,
tick-stamped, fingerprint recomputed in dev, refused in lockstep); `ARCHIVE` → persisted to
`pref_path/cvars.txt` on shutdown as `name value` lines.

**9.3.6 Dot paths.** `player.Transform.x[0]` → split on `.`; token 0: `#<index>` (entity index, any
generation → current), `@<singleton>` or a name (`Name` component StrId via the interner —
`ERR_PATH_NO_ENTITY` if unknown or ambiguous); token 1: component by name hash; token 2: field
by name hash with optional `[k]`; result `{ Entity e; ComponentId c; u16 field; u16 elem; }`.
Reads go through `world_get` + `FieldInfo`; writes through `world_set_field`. A watch stores the
resolved tuple and re-resolves only when `world_get` returns null (stale handle) — never the
string per frame.

**9.3.7 Capture mask.** After ImGui's `NewFrame`, the shell publishes
`input_set_capture_mask(w, (io.WantCaptureMouse ? 1 : 0) | (io.WantCaptureKeyboard ? 2 : 0))`
(`core/input.h`, expected); the Live producer applies it at the next fold (`INPUT.md` §5) —
one frame of latency, ImGui's own model.

**9.3.8 Desync diff** (`desync_diff(const Snapshot* a, const Snapshot* b, u32 max_n, DiffFn out, void* ctx) → u32`;
`b` may be the live registry wrapped as a snapshot view):
```
if a.fingerprint != b.fingerprint: out(FINGERPRINT_MISMATCH); return 1
for i in registry order:   // the lockstep contract order
  if used_a[i] != used_b[i]: out({arena i, USED, used_a, used_b}); n++ ; continue-after-report
  if memcmp(base_a, base_b, used) == 0: continue
  table = component info (ECS column) | TL_POOL_ROW table (Alloy pool) | null
  if table: rows = dense_count (from the column header at base); for r in 0..rows: if memcmp(row_a, row_b, size) != 0:
      for f in fields (declaration order): if memcmp(field bytes): out({arena, entity_a[r] or row r, comp, f.name, elem, fmt(kind, a), fmt(kind, b)}); if ++n == max_n return n
      then the sparse pages / entity map as STRUCTURE entries (first differing u32)
  else: out({arena, BYTES, first differing offset, 16-byte hex of each}); n++
return n
```
**Signature completed over spec (w3-editor, 2026-08-27, `CONTAINERS.md` §8.6a's own precedent for
this class of gap):** the pseudocode's signature has no way to know which arenas are
`ARENA_SNAPSHOT`-flagged — only flagged arenas occupy blob space (`arena_registry.cpp`'s
`registry_snapshot`/`registry_restore`, both of which take the registry for exactly that reason)
— so the real signature is `desync_diff(const ArenaRegistry* reg, const Snapshot* a, const
Snapshot* b, u32 max_n, DiffFn out, void* ctx) → u32` (`core/desync_diff.h`). `DiffKind` /
`DesyncEntry` / `DiffFn` (also undescribed beyond the pseudocode's loose field names) are
scoped, for now, to the three cases a plain byte-level walk can report without a component/pool
table: `DIFF_FINGERPRINT_MISMATCH`, `DIFF_USED`, `DIFF_BYTES` (the `table` branch's per-field
ECS-column case and the Alloy pool-table case both stay `DIFF_BYTES` — the honest fallback —
until each lands; extending `DesyncEntry` then, not speculatively now). "`b` may be the live
registry wrapped as a snapshot view" is not yet built — every caller today diffs two real
`Snapshot`s.
Order: registry order → row → field, so the first report is the earliest difference in the
contract order. CLI: `tl_driver --diff a.snap b.snap [--max 50]` prints TSV
`arena\trow\tcomponent\tfield\ta\tb`; the Net/World panel calls the same function.

**9.3.9 Crash pipeline.** `core/crash_report.cpp` registers `crash_write(ctx, reason, code, addr, os_ctx)`
with `platform.crash.install` at init; the 16 MB crash arena is committed then, and every
pointer the writer needs (log ring, profiler ring, registry, snapshot ring, input ring, census
counters) is captured into a `CrashCtx` at init — the handler touches no allocator and no lock
it does not own.
- *Writer (both OSes):* reentry guard (`atomic_cas32`); fill `CrashHeader`; sections 1–7 by
  `memcpy` into the arena (snapshot = the newest ring slot's `[base, used)` per arena, inputs =
  frames since that slot's tick); one `os_crash_write_file(path, span)` (raw `CreateFileW`/
  `WriteFile` / `open`/`write`/`fsync` — not the `FileApi`); then the OS tail below.
- *Windows (`os_crash_win.cpp`):* `SetUnhandledExceptionFilter(tl_seh_filter)`; `TL_FATAL` raises
  `0xE0544C46`. Filter: call the writer; then `CreateProcessW(self, "--dump <pid> <tid> <EXCEPTION_POINTERS*> <event> <path.dmp>")`
  with an inherited event, wait ≤ 10 s; `tidelock --dump` (the same exe, `app/dumper.cpp`, no
  new exe — CANON exe list) does `OpenProcess` + `MiniDumpWriteDump(WithIndirectlyReferencedMemory | WithThreadInfo | WithUnloadedModules)`
  and sets the event; the filter appends section 8 and returns `EXCEPTION_EXECUTE_HANDLER` →
  `TerminateProcess(3)`. Out-of-process because in-process `MiniDumpWriteDump` on a corrupt heap
  or exhausted stack is unreliable (MS guidance).
- *Linux (`os_crash_posix.cpp`):* `sigaction` SEGV/BUS/ILL/FPE/ABRT, `SA_SIGINFO | SA_ONSTACK |
  SA_NODEFER` off, 64 KB `sigaltstack` from the crash arena; handler: guard; frame-pointer walk
  (`-fno-omit-frame-pointer` in dev/netcode) ≤ 64 frames → section 7; writer; `signal(sig, SIG_DFL); raise(sig)`
  so the OS core dump (`ulimit -c`) still happens. `backtrace()` and `dladdr` are not
  async-signal-safe and are not called; module bases come from `dl_iterate_phdr` at init.
- `TL_FATAL` → `tl_fatal` → `TL_LOG_ERR` → `crash.raise_fatal(msg)` → the same path with `reason = 1`.

**9.3.10 Replay scrub.** Record (always on in dev): at `LAST`, the recorder appends
`InputFrame[MAX_PEERS]` + world hash to `inputs`; every `interval_ticks` it copies the `LAST`
snapshot into `slots[head]` and pushes a `Keyframe`. Seek to `T`: `kf = max{frames.tick ≤ T}`
(none → the oldest); `registry_restore(kf.slot)` → `post_restore` barrier (`MEMORY.md` §5);
set the `Replay` producer cursor to `kf.input_offset`; run `T − kf.tick` ticks with
`run_phases_sim` + `barrier_end_of_tick` and no render (≤ 59 ticks ≈ 0.5 s at 8 ms/tick); then
the next frame renders at `T`. `TL_CHECK(world_hash(T) == recorded hash)` — bit-exact by
construction, so a mismatch is a determinism bug surfaced by the scrub bar. Play/pause/speed:
speed ∈ {0, ¼, ½, 1, 2, 4} ticks per frame applied as an accumulator override while the panel
owns the producer. "Save replay" writes `RecordedInput` (`INPUT.md` §4) + the keyframe slots
with `write_atomic`.

### 9.4 Panels

| Panel | Data source | Refresh |
|---|---|---|
| Inspector | selection (`Editor.sel`), `World.comps[].info`, `world_get` | every frame (read); edits → commands at the next barrier |
| Console | `ConsoleCmd` table, `CvarTable`, history ring, Luau UI VM | on input; log lines appended as they arrive |
| Log | `LogState.ring` | every frame; filter by level/tick range; follows tail unless scrolled |
| Profiler | `ProfState.ring`, counters | every frame, depth-indented node list of the latest ring frame (**v0: text list, not a rendered flame graph** — built W3, `profiler_panel.cpp`); pause pins the panel's view to a specific ABSOLUTE ring frame, never `ring.head` itself (`Editor.prof_paused`/`prof_view_frame`) — every other reader keeps seeing live frames; a `slots_back` offset is NOT a stable view under an advancing ring, so the pinned frame's current `slots_back` is re-derived every draw (B-1, 2026-08-27), falling back to the oldest live frame once the pin ages out; `dump` → §9.3.2 deferred until `trace_export.cpp` unblocks (§9.6 build order item 3) |
| Probes | `ProbeState.keys` | every frame, summary table (**v0: READ ONLY** — built W3, `probes_panel.cpp`; no enable/disable checkbox and no "profile masks" control — the real toggle is console/cvar-routed and that wiring does not exist yet, the same class of gap already deferred for Console's own cvar UI, and `ProbeKey` carries no "profile mask" field for any control to back) |
| Replay | `KeyframeRing`, the `Replay` producer | scrub events only; per-frame tick readout |
| Scripts | Luau VM file list, ImGuiColorTextEdit buffers, Tier 0/1 debugger state (`LUAU-LAYER.md`) | on edit / breakpoint events |
| World | `World.entities` (slot walk), singleton list, `ArenaRegistry` | every frame — entities/singletons; entity list virtualized (`ImGuiListClipper`, built W3 `world_panel.cpp`). **Arena hashes are ON-CLICK ("rehash arenas"), not every frame** — `registry_hash_all` rehashes every `ARENA_HASHED` entry's full `[base,used)`, a cost that scales with world size, not a fixed per-frame read like every other panel's own data; the panel keeps the previous click's set (`Editor.world_arena_hash_prev`) to flag which arenas changed since the last click — the actual "last hash" workflow, paid for on click. The button itself is hidden until `ArenaRegistry.sealed` (`registry_seal` is the registry owner's call, `app/`'s job, `W4`, not built) — arenas display by hex `NameHash` id, not a name (none is stored anywhere for one) |
| Sim | `sim/views.h`: chunk dirty serials, island list, cavity graph, per-pass `ProfNode` times, float-shadow error column (§8) | every frame; the chunk map draws from dirty serials, not rasters |
| Net | `net/` state (`tl_net` only): peers, RTT, quorum, epoch, log ring, impairment cvars | every frame; desync diff on demand (§9.3.8) |

### 9.5 Tests — `tests/editor/` (in `tl_tests`; panel tests run ImGui headless via a null backend)

| Test | Asserts |
|---|---|
| `log_levels_and_compile_out` | each sink filters by its level; a `TL_LOG_TRACE` call site under `TL_LOG_MIN=2` leaves no symbol and does not evaluate its argument (a counter inside the args stays 0); records carry the sim tick; 4097th record overwrites the oldest; `msg` truncation at 223 bytes + NUL |
| `prof_zero_alloc_and_tree` | 1000 frames of nested scopes: arena-offset delta 0 after the first frame; tree `parent`/`depth` match a known nesting; `t_end ≥ t_begin`; overflow at 8193 scopes counted, not crashed; two workers merge in worker order; `depth == 0` at frame end else fatal (child) |
| `trace_json_golden` | a fixed 3-frame tree exports byte-identical to `tests/golden/trace_3frames.json` (clock stubbed) |
| `probe_tsv_golden` | `LOG` every 3 ticks, `ON_CHANGE`, `MARK`, `ASSERT` over 20 ticks → `TL_GOLDEN_TSV("probe_basic")`; summary line values (count/changes/min/max/mean) exact; disabled key emits nothing; a `pos_t` probe prints `%.9g` of raw·2^-18 |
| `inspector_roundtrip_per_kind` | **amended by RR-39 (2026-08-27): dropped "driven through the headless ImGui test engine"** — Dear ImGui Test Engine is not vendored (`vendor/VERSIONS` pins imgui core-only, `IMGUI_ENABLE_TEST_ENGINE` is commented out in `imconfig.h`, no `vendor/imgui_test_engine` directory exists, and vendoring a new dependency is a ruling of its own, not a drive-by — `VERSIONS`'s own words), so the row was unmeetable as written. The driving method is now the direct call the walker's own write path already establishes (`inspector_set_scalar_field`, matching `console_exec`'s identical precedent) — every other assertion is unchanged and still required: a test component with one field of every `FieldKind`; an edit of each **editable** field → one `CMD_SET_FIELD` per edit → after the barrier the column holds the value; fx edit of `1.5` into `pos_t` yields raw `0x60000` (**satisfied** — RR-38's quantizer landed and is wired into the walker; `tests/editor/inspector.test.cpp`'s `inspector_fx_field_edit_widget_writes_through_parse_and_command` asserts the parse, the barrier apply, and the column value); handle kinds **and `K_StrId`** produce no command. **Residual gap, recorded rather than silently passed over (RR-39):** with no test engine, nothing proves a real widget is wired TO the setter it calls — the setter itself is tested, the widget-to-setter edge is not; a real click-driven regression here would go undetected until a human notices in a live session. Tracked in `TODO.md` as a known post-v0 gap. |
| `console_parse` | tokenizer table (quotes, escapes, comment, 17 tokens → error, unterminated quote → error); dispatch with `argc` bounds; completion returns sorted prefix matches; `SIM` cvar set is refused under a lockstep flag and accepted otherwise (as a command) |
| `dotpath_resolve` | `player.Transform.x`, `#12.Health.hp`, `@PeerSlots.local_slot`, `a.Flags.bits[3]`; unknown entity/component/field → named errors |
| `desync_diff_known_pair` | two worlds, identical until tick 100, then one receives a poked `Health.hp` (test hook): the diff reports exactly `{arena Health, entity e, field hp, 10, 11}` first; `max_n = 1` returns 1; fingerprint mismatch short-circuits |
| `crash_report_layout` | `sizeof(CrashHeader) == 368`; a forced `TL_FATAL` in a child writes a `.tlcrash` whose header parses, sections 1/2/3/6 present, total ≤ 16 MB; the log tail's last record is the fatal message |
| `replay_scrub_exact` | record 300 ticks with keyframes every 60; seek to 175 → restore kf 120 + 55 re-sim ticks; `world_hash(175)` equals the recorded hash; seek to 0 and to 299 likewise |

### 9.6 Build order and done criteria

1. `tl_assert.h`, `tl_log.h` + `log.cpp` (ring + stderr; file sink once `FileApi.append` exists) → `log_levels_and_compile_out`. Needed by every other module's tests — first.
2. `core/cvar`, `core/crash_report.cpp` + `os_crash_*` → `crash_report_layout` (Windows dumper = `--dump` path in `app/`).
3. `tl_prof.h` + `prof.cpp`, `trace_export.cpp` → `prof_zero_alloc_and_tree`, `trace_json_golden`; scheduler auto-scopes.
4. `tl_probe.h` + `probe.cpp` + `--dump-probes` → `probe_tsv_golden`. (Steps 1–4 are what the Alloy harness needs; they precede any sim code.)
5. Panels **Log**, **Console** (+ cvars), **Inspector** → `inspector_roundtrip_per_kind`, `console_parse`, `dotpath_resolve`; **Profiler**, **Probes**, **World**; `editor/shell.cpp` + `PlatformDevApi` wiring + capture mask.
   **RR-40 (2026-08-27): the original single "v0 editor done" bullet is SPLIT into two criteria,
   because four of its own clauses turned out to need a running shell that cannot run yet**
   (`struct PlatformDevApi` is defined nowhere in this tree; `editor_frame` is
   `TL_FATAL("unimplemented")`; `editor/shell.cpp` itself does not exist; `PLATFORM.md` §9.7 step 5
   — `imgui_backend.cpp` + `PlatformDevApi` — is not built and no lane is queued for it). Every
   clause of the original criterion is assigned below; none is left unassigned.

   **Panels v0** — reachable without a running shell; headless-testable; `w3-editor`'s own PR gate:
   - All six panels exist and are registered (`editor_register_panel`), each callable and tested
     directly against a null ImGui backend, with no `editor_frame`/`PlatformDevApi` involved —
     **satisfied** (Log, Console, Inspector, Profiler, Probes, World all shipped).
   - Zero heap allocation per frame outside `pool_vendor`, NARROWED to what a running shell is not
     needed to prove: every existing panel's own `draw_fn`, called directly and repeatedly —
     **satisfied and verified** (`tests/editor/no_stray_alloc.test.cpp`; see `TODO.md`). The
     BROADER original reading — the whole live session, every frame, forever, including
     `editor_frame`'s own `NewFrame`/dockspace/`Render` bracket once it exists — moves to shell v0
     below, since there is no `editor_frame` body yet to test it against.
   - Console's cvar `set <name> <value>` command path (`§9.3.5`: parse per kind, `FX_RAW` accepts
     a decimal literal "quantized RNE", `SIM` → `CMD_SET_CVAR`, `ARCHIVE` → `pref_path/cvars.txt`
     on shutdown) — text-driven, panel-local, no widget, no shell. **Not yet built; blocked only on
     RR-38's quantizer** (filed, `TODO.md`) for the decimal-literal half — once that lands, this is
     in scope for `w3-editor`'s own PR gate. `§9.3.5`/`§9.4` describe no SEPARATE cvar "browser"
     widget beyond this text command — if a later reading disagrees, that is itself a doc gap to
     raise, not an assumption to build past.
   - Inspector's fx-field editing — **DONE (RR-38 landed, wired in the same wave)**: the
     `1.5` → `0x60000` assertion in `inspector_roundtrip_per_kind` (RR-39, amended above) is
     satisfied (`tests/editor/inspector.test.cpp`'s
     `inspector_fx_field_edit_widget_writes_through_parse_and_command`). Handle-field editing
     stays display-only at v0 — not an RR-38 dependency at all, `§9.3.4`'s own updated text notes
     it needs its own resolution UI ("type an entity name"/"pick from the interner"), a different,
     unscoped feature.
   - "An edit in the inspector appears in the replay log" — re-examined against the doc rather than
     assumed shell-blocked: recording (`core/recorder.cpp`'s `recorder_tick`, already built by
     `loop+input`) and re-sim (`engine_tick_once`, `core/loop.cpp`) both run headlessly today, no
     shell required (`tests/core/loop.test.cpp`'s own precedent). **This clause is panels-v0-SHAPED,
     not shell-blocked — but it is UNSATISFIABLE as literally written until a separate, deeper gap
     is resolved:** `core/recorder.h`'s format records only `{frames, world_hash}`, never commands,
     so an inspector edit (a command, `CMD_SET_FIELD`, never an `InputFrame`) cannot be reproduced
     by the seek algorithm's input-only re-sim. Filed as its own ruling request (`TODO.md`,
     "keyframes.cpp's seek algorithm cannot reproduce an inspector edit") — tracked there, not
     re-filed here; this row exists so the clause has a home and isn't silently dropped by the
     shell/no-shell split.

   **Shell v0** — needs a running `editor_frame`/real `PlatformDevApi`; deferred; NOT this PR's
   gate; tracked in `TODO.md` as a follow-up naming its blocker so it cannot be forgotten once this
   PR merges:
   - `editor/shell.cpp` itself (ImGui context creation, dockspace, `imgui.ini` load/save, the menu
     that calls `editor_toggle_panel`).
   - `PlatformDevApi` (`PLATFORM.md` §9.7 step 5: `imgui_backend.cpp` + the `imgui_init`/
     `imgui_new_frame`/`imgui_render` hooks) — `platform/`'s file, out of `editor`'s cone regardless
     of this split.
   - `editor_frame`'s real (non-stub) implementation — the `NewFrame`/panel-draw-loop/`Render`
     sequence over a REAL platform window.
   - The capture-mask publish (`§9.3.7`): `input_set_capture_mask` called from inside a real
     `editor_frame`, once per real frame, over a real `io.WantCaptureMouse`/`WantCaptureKeyboard`
     — the algorithm is simple, but nothing calls it outside the stubbed `editor_frame` body today.
   - `imgui.ini` persisting in `pref_path` — needs `PlatformDevApi`'s real OS pref-path mechanism
     and a real ImGui context's own load/save lifecycle; no headless equivalent.
   - Zero heap allocation per frame, the BROADER reading (above): the whole live session including
     `editor_frame`'s own body, once built.
   - Luau REPL hand-off (`§9.3.5`: `console.command(name, fn, usage)`) and the Console panel's
     "Luau UI VM" data source (`§9.4`) — **NOT shell-blocked; blocked on a DIFFERENT unbuilt lane**
     (`LUAU-LAYER.md`, `script`, not `editor`) — filed here anyway, explicitly, rather than left to
     float on neither side of the split, per Rafael's own instruction. Post-v0 for this PR either
     way; revisit once `script`'s Luau binding layer lands.
6. `core/desync_diff.cpp` + `tl_driver --diff` → `desync_diff_known_pair` (lands with the determinism harness, before netcode).
7. `keyframes.cpp` + **Replay** panel → `replay_scrub_exact`; **Scripts** panel with the Luau layer; **Sim** panel with Milestone 2 views; **Net** panel with Hovel. **Governed by RR-42** (`TODO.md`, `DETERMINISM.md` §6/§9.2): any external-chunk command applied during a recording makes it non-reproducible via input-only re-sim with no way for the harness to detect it — the ruling requires a taint flag on the recording plus forced keyframes at every external-chunk apply; RECORD-ONLY today, binding on whoever builds this item.

---

## 10. Rulings (closed 2026-08-22 — nothing open)

- **R-1 The Inspector is single-select through v0.** Multi-select edit is part of the editor-shell
  seam (`RESERVED-SEAMS.md` §12 — the selection service) and lands with it, not before.
- **R-2 ImGui multi-viewport (drag a panel out to its own OS-level window) is NOT available at
  v0 — corrected 2026-08-27, w3-editor.** The original claim here ("docking branch + SDLRenderer3
  backend support it; zero cost") was wrong: `ImGuiBackendFlags_PlatformHasViewports` +
  `...RendererHasViewports` are a real backend requirement, and SDL_Renderer (§1's v0 shell
  backend) never implements `RendererHasViewports` upstream (SDL's own docs) — not a gap that
  closes later on this backend, a permanent one. Docking *within* the one OS window (the docking
  branch's other half) is unaffected and stays on for v0. Multi-viewport pop-out is deferred to
  whichever backend migration `RENDER2D.md` §6 eventually triggers; Rafael has since expressed a
  preference for SDL_GPU (`imgui_impl_sdlgpu3.cpp`) over raw OpenGL3 for that migration, discussed
  in full under `TODO.md`'s "Post-v0 render backend" note — not yet a ruling on `RENDER2D.md`'s
  own backend-trigger table, which is render2d's file, not editor's; filed as **RR-37**
  (`TODO.md`), steward-verified against the vendored backend sources: `imgui_impl_sdlgpu3.cpp` is
  one of the backends that actually sets `RendererHasViewports`, so the SDL_GPU direction closes
  this gap, not just a preference among equals.
- **R-3 The driver has `--dump-probes`**: the probe TSV sink writes to a CI artifact path; same
  sink as the panel.

*Rev 1 — 2026-08-22.*
