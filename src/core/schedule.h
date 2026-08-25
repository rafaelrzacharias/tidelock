#pragma once
// ---------------------------------------------------------------------------------------------
// schedule.h - SystemDesc storage, the topo-sorted total order, phase slices, run_phase.
//
// Spec: docs/ECS.md §3 (design), §10.6 (this header); docs/FRAME-LOOP.md §2 (phase semantics).
// Purpose: systems are stateless free functions ordered by REGISTRATION ORDER, refined by
//   before/after edges on stable labels, topo-sorted with registration order as the tie-break -
//   a total, reproducible order built once at startup (rebuilt only on script-reload
//   re-registration). The per-tick loop just walks the slices; zero scheduling cost.
// Invariants: reads/writes are for PARALLELISM, never ordering (docs/ECS.md §3) - v0 stores
//   them untouched for the jobs lane. before/after labels resolve WITHIN the system's phase:
//   phases already impose order across phases, so a cross-phase (or unknown) label is the same
//   registration bug and is TL_FATAL by name (docs/ECS.md §10.6 "unknown label -> TL_FATAL").
//   Duplicate labels are TL_FATAL at registration (labels are the edge keys; two systems under
//   one label would make before/after silently ambiguous). A cycle is TL_FATAL at build, naming
//   a member. Registration is init-only; MAX_SYSTEMS = 512 bounds the storage (hundreds by
//   design - docs/ECS.md §10.6; Luau alone caps at 256, docs/LUAU-LAYER.md §10.6).
// Determinism: the order is a pure function of (registration order, before/after edges) - Kahn's
//   algorithm always takes the LOWEST reg_index from the ready set (O(n^2) at startup only).
//   run_phase publishes sched.running = { index, label } before every call (docs/CANON.md; the
//   Luau trampoline and the profiler auto-scope read it) and applies the command barrier at the
//   phase's end (docs/ECS.md §4).
// Threading: build and registration are init-only, single-threaded; run_phase is the tick
//   thread. With JOBS.md the phase slice is partitioned by reads/writes intersection - later.
// Includes: core/reflect.h, core/phase.h, foundation/array.h.
// ---------------------------------------------------------------------------------------------
#include "core/reflect.h"
#include "core/phase.h"
#include "foundation/array.h"
#include "foundation/scratch.h"

struct World;

// A system: a stateless free function receiving the world and nothing else (docs/ECS.md §3).
typedef void (*SystemFn)(World* w);

// SystemDesc.flags. The high 16 bits carry the Luau ordinal for trampoline systems
// (docs/LUAU-LAYER.md §10.6 "the desc carries the ordinal in flags >> 16").
enum : u32 { SYS_LUAU = 1u << 0 };

// One system's registration record (docs/ECS.md §3). The spans are caller storage at the
// world_register_system call; registration copies their contents into schedule-owned memory.
struct SystemDesc {
    SystemFn fn;
    NameHash label;
    Phase    phase;
    Span<const ComponentId> reads;
    Span<const ComponentId> writes;
    Span<const NameHash>    before;
    Span<const NameHash>    after;
    u32 flags;
};

// Storage bound (docs/ECS.md §10.6: "n = hundreds, startup only").
enum : u32 { MAX_SYSTEMS = 512 };

// A registered system + its two order coordinates (docs/ECS.md §10.6).
struct SystemRec {
    SystemDesc d;
    u32 reg_index;   // registration ordinal - the tie-break and the chunk id (docs/ECS.md §10.5)
    u32 phase_pos;   // index into `order` after build
};

// The built schedule (docs/ECS.md §10.6): phase p's slice is order[phase_begin[p],
// phase_begin[p+1]). `running` is published by run_phase around every system call.
struct Schedule {
    Array<SystemRec> systems;
    u32 phase_begin[PHASE_COUNT + 1];
    Array<u32> order;
    struct { u32 index; NameHash label; } running;   // index == RUNNING_NONE outside a call
    u8 built;
    u8 _pad0[3];
};

// The `running.index` value outside any system call.
enum : u32 { RUNNING_NONE = 0xFFFFFFFFu };

// Wires the schedule's fixed storage (MAX_SYSTEMS records) onto the meta arena; no build yet.
void schedule_init(Schedule* s, VMemArena* meta);

// Registers one system (init only; order of calls = registration order = the tie-break).
// Copies desc and its four span contents onto `meta`. TL_FATAL: after build, at MAX_SYSTEMS,
// on a duplicate label, or on a render-phase/sim-phase value outside the enum.
void schedule_register(Schedule* s, VMemArena* meta, const SystemDesc* desc);

// Builds the total order: per phase, Kahn's algorithm over the before/after edges, ready set
// scanned for the lowest reg_index (docs/ECS.md §10.6). TL_FATAL on an unknown or cross-phase
// label (named) and on a cycle (a member named). Scratch is used for the edge lists and reset
// to its entry mark. Idempotent per registration set: call once after the last registration
// (script reload re-registers and rebuilds - the world owns that sequencing).
void schedule_build(Schedule* s, Scratch* scratch);

// Runs phase p's slice against w: publishes running, calls each system, clears running, then
// applies the command barrier (apply_commands - docs/ECS.md §4/§10.6). Sim code never calls
// this; the loop (and the tests' tick driver) does.
void run_phase(World* w, Phase p);
