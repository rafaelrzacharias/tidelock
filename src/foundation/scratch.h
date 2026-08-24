#pragma once
// ---------------------------------------------------------------------------------------------
// scratch.h - per-worker frame/scratch arenas: a VMemArena plus an explicit marker stack.
//
// Spec: docs/MEMORY.md §1.3 (design), §8.1 (this header); docs/JOBS.md §1 (per-worker scratch
//   is for ALLOCATION LOCALITY - nothing keyed by worker identity may be read back into
//   results); docs/CPP-SUBSET.md §7b (the TL_SCRATCH_SCOPE row).
// Purpose: the everyday transient allocator. Command buffers, broadphase transients, neighbor
//   lists, the render packet live here; event queues do NOT (their read half outlives the
//   frame - docs/ECS.md section 10.4).
// Invariants: reset at frame end (main thread's) or at the barrier (workers'); depth must be 0
//   at reset (unbalanced scopes are a bug). Scratch is never registered, never hashed, never
//   snapshotted. Reused memory is NOT zero - debug poisons 0xDD on reset (ARENA_POISON) so a
//   stale read shows as garbage; a pool that needs zeroed scratch memsets explicitly
//   (docs/MEMORY.md section 1.1).
// Determinism: contents are transient by contract; the hash-region integrity test proves
//   mutating scratch never moves the world hash (docs/DETERMINISM.md section 4).
// Threading: one Scratch per worker, single-owner; no locking.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/vmem_arena.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/vmem_arena.h"

// Scope-stack depth cap; a deeper nest is a design smell, not a tunable. Constant added over
// docs/MEMORY.md section 8.1 - recorded in TODO.md (W1 mem notes).
enum : u32 { SCRATCH_MAX_SCOPES = 16 };

struct Scratch {
    VMemArena a;
    u64 mark[SCRATCH_MAX_SCOPES];
    u32 depth;
    u32 _pad0;
};
static_assert(__is_trivially_copyable(Scratch), "");

// Reserves the worker's scratch space (ARENA_POISON is added in TL_DEV builds). Same errors as
// vmem_arena_init.
inline ErrCode scratch_init(Scratch* s, NameHash id, u64 reserve_bytes, const VMemApi* os) {
    s->depth = 0; s->_pad0 = 0;
    for (u32 i = 0; i < SCRATCH_MAX_SCOPES; ++i) { s->mark[i] = 0; }
#if TL_DEV
    return vmem_arena_init(&s->a, id, reserve_bytes, ARENA_POISON, os);
#else
    return vmem_arena_init(&s->a, id, reserve_bytes, 0, os);
#endif
}

// Bump-allocates from the scratch arena; contents are garbage (poisoned in debug), zero only on
// first-touch pages. Legal inside a tick (scratch is the exception to "ticks allocate nothing").
inline void* scratch_push(Scratch* s, u64 bytes, u32 align) { return arena_push(&s->a, bytes, align); }

// Opens a scope: records the current mark. TL_ASSERT on overflow (SCRATCH_MAX_SCOPES).
inline void scratch_scope_begin(Scratch* s) {
    TL_ASSERT(s->depth < SCRATCH_MAX_SCOPES);
    s->mark[s->depth] = arena_mark(&s->a);
    s->depth += 1;
}

// Closes the innermost scope: rolls the arena back to its mark (poison in debug). TL_ASSERT on
// underflow.
inline void scratch_scope_end(Scratch* s) {
    TL_ASSERT(s->depth > 0);
    s->depth -= 1;
    arena_reset_to(&s->a, s->mark[s->depth]);
}

// Frame-end / barrier reset: TL_ASSERT depth == 0 (unbalanced scopes), then rolls to 0.
inline void scratch_reset(Scratch* s) {
    TL_ASSERT(s->depth == 0);
    arena_reset_to(&s->a, 0);
}

// docs/CPP-SUBSET.md section 7b: the explicit begin/end pair, no RAII. (The catalogue row
// spells the stem TL_SCRATCH_SCOPE; the pair spelling is recorded in TODO.md, W1 mem notes.)
#define TL_SCRATCH_SCOPE_BEGIN(s) scratch_scope_begin((s))
#define TL_SCRATCH_SCOPE_END(s)   scratch_scope_end((s))
