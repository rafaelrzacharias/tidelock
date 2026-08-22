# Tooling — editor shell, inspector, console, profiler, probes, logging, crash (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §9. Harvests Layr's born-instrumented
> discipline (`FOUNDRY-EXTRACTION.md`, `LAYR-ANALYSIS.md` §4) and ENGINE-DESIGN F1's dev-tooling
> half, on Dear ImGui. Built **right after v0** (dev surface early, not at "layer 10").
> **Owns:** `src/editor/` (dev tiers only; compiled out of `netcode`/`ship` by absence).

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
(`INPUT.md` §5).

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
- **Cvars:** a C++-registered reflected table (`TL_CVAR(f32, render_zoom, 1.0f, "…")`) with flags
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

## 9. Open

- **O-1** Whether the Inspector supports multi-select edits (lean: later; single-select v0).
- **O-2** ImGui multi-viewport (tear-off panels) at v0 — the SDLRenderer3 backend supports it;
  cost is nothing. Lean: on.
- **O-3** A headless `--dump-probes` mode for the test driver so probe TSVs are CI artifacts
  (yes, trivially — the sink is the same).

*Rev 1 — 2026-08-22.*
