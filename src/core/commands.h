#pragma once
// ---------------------------------------------------------------------------------------------
// commands.h - the deferred command buffer: the ONE channel through which anything changes
//   world structure (systems, editor, Luau - docs/ECS.md §4).
//
// Spec: docs/ECS.md §4 (design), §10.5 (this header). apply_commands' semantics per record kind
//   are the barrier contract every phase boundary runs (docs/CANON.md "Phases and the barrier").
// Purpose: all structural changes (spawn/destroy/add/remove, field pokes, singleton swaps) are
//   RECORDED, never performed in place - iteration always runs on a stable snapshot, and
//   mid-iteration mutation is structurally impossible because the API defers.
// Invariants: one chunk per system, chunk_id = the system's SCHEDULE position (docs/ECS.md
//   §10.5 "system index in schedule order"), plus one external chunk (id = system count) for
//   recorders outside any system (editor, app); applied at every phase barrier in ascending
//   chunk_id, record order within a chunk. Records/payload live on a dedicated per-world
//   transient arena, NOT the general scratch (v0 reconciliation, docs/ECS.md §10.5): a system's
//   own TL_SCRATCH_SCOPE pair would free a chunk first recorded inside it - the dedicated arena
//   has exactly the required lifetime (reset after every apply) with no scope interleaving.
//   Fixed caps per chunk (WorldDesc; TL_FATAL on overflow - a blown budget is a bug).
// Determinism: apply order is a pure function of the schedule and the per-system record order;
//   the apply window brackets guard_barrier_begin/end (the GROWS_AT_BARRIER window,
//   docs/MEMORY.md §8.4) when the world carries a guard.
// Threading: v0 single-threaded; JOBS.md replaces the chunk table with per-worker chunks keyed
//   by chunk id, same apply order.
// Includes: core/reflect.h, foundation/array.h.
// ---------------------------------------------------------------------------------------------
#include "core/reflect.h"
#include "foundation/array.h"

struct World;

// The record kinds (docs/ECS.md §10.5). CMD_ALLOY / CMD_SCRIPT_RELOAD / CMD_DATA_RELOAD /
// CMD_ASSET_READY have no recorder yet - their producing lanes (alloy-substrate, luau-bindings,
// assets) land them; an applier meeting one today is TL_FATAL("unwired"), never a silent skip.
enum CmdKind : u8 {
    CMD_SPAWN_REALIZE = 1,
    CMD_DESTROY,
    CMD_ADD,
    CMD_REMOVE,
    CMD_SET_FIELD,
    CMD_SINGLETON_SET,
    CMD_ALLOY,
    CMD_SCRIPT_RELOAD,
    CMD_DATA_RELOAD,
    CMD_ASSET_READY,
};

// One record, 16 B (docs/ECS.md §10.5). Payload bytes live in the chunk's payload array;
// CMD_SET_FIELD's payload is a u32 field index followed by the field's bytes.
struct CmdRecord {
    CmdKind kind;
    u8  _pad0;
    u16 comp;          // ComponentId for ADD/REMOVE/SET_FIELD/SINGLETON_SET; 0 otherwise
    Entity e;
    u32 payload_off;
    u32 payload_len;
};
static_assert(sizeof(CmdRecord) == 16, "docs/ECS.md section 10.5: records are 16 B");

// One recorder's buffer for the current phase window; arrays are carved from the world's
// command arena on first record and dropped at apply.
struct CmdChunk {
    Array<CmdRecord> recs;
    Array<u8> payload;
    u32 chunk_id;
    u8  active;
    u8  _pad0[3];
};

// All chunks (one per schedule position + the external chunk). Sized at world_build_schedule.
struct CommandBuffers {
    CmdChunk* chunks;
    u32 chunk_count;    // schedule system count + 1
    u32 records_cap;    // per chunk (WorldDesc)
    u32 payload_cap;    // per chunk, bytes (WorldDesc)
};

// Applies every active chunk in ascending chunk_id, record order within a chunk, inside the
// guard's barrier window; then resets the command arena. Runs at every phase barrier
// (run_phase calls it) and from world_flush. Record semantics: contract block + docs/ECS.md
// §10.5. TL_FATAL on an unwired kind; TL_CHECKs on targets that must be live/present.
void apply_commands(World* w);

// Drops every recorded-but-unapplied command and un-reserves pending fresh spawns (the
// rollback path clears buffers before restoring - docs/FRAME-LOOP.md §8.3).
void commands_discard(World* w);
