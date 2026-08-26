#pragma once
// ---------------------------------------------------------------------------------------------
// arena_registry.h - the registered arena set: one registry, three consumers (hash, snapshot,
//   rollback ring), plus the arena-offset guard.
//
// Spec: docs/MEMORY.md §1.2 (design), §2 (guards), §8.3/§8.4 (this header);
//   docs/DETERMINISM.md §4 (what the hash covers).
// Purpose: every long-lived authoritative pool registers here at init. The registered set is
//   simultaneously the per-tick hash unit, the snapshot unit, and the rollback ring's payload
//   (docs/MEMORY.md section 0 rule 4).
// Invariants: REGISTRATION ORDER IS PART OF THE LOCKSTEP CONTRACT - the world hash folds the
//   per-arena hashes in registry order and the snapshot blob is laid out in registry order; the
//   order is frozen by registry_seal and its ids fold into session_fingerprint (docs/BUILD.md
//   section 5). Adding after seal is TL_FATAL. Hashes cover [base, used) - never capacity
//   (docs/DETERMINISM.md section 4); ArenaEntry carries explicit padding.
// Determinism: this TU is in the audited det half. registry_hash_all calls tl_hash64
//   (foundation/hash.h, rapidhash, seed TL_HASH_SEED) - a det-half symbol.
// Threading: registry mutation is init-only; hash/snapshot/restore run single-threaded at the
//   LAST phase / barrier (docs/CANON.md "Phases and the barrier").
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/vmem_arena.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/vmem_arena.h"

struct Snapshot;  // foundation/snapshot.h

enum : u32 { MAX_ARENAS = 4096 };  // docs/CANON.md "Sizes and caps" (E-2 ruling 2026-08-26)

// Registry membership flags (docs/MEMORY.md section 1.2): HASHED = folded into the world hash;
// SNAPSHOT = copied by registry_snapshot; GROWS_AT_BARRIER = may grow inside the barrier-apply
// window (ECS columns, Alloy pools during pass 5) without tripping the guard.
// HASHED IMPLIES SNAPSHOT: registry_add TL_FATALs on the combination without it (ruled
// 2026-08-24, docs/MEMORY.md section 1.2). SNAPSHOT without HASHED stays legal - state that is
// restored but deliberately outside the hash.
enum : u32 {
    ARENA_HASHED           = 1u << 0,
    ARENA_SNAPSHOT         = 1u << 1,
    ARENA_GROWS_AT_BARRIER = 1u << 2,
};

struct ArenaEntry { NameHash id; VMemArena* arena; u32 flags; u32 _pad0; };
static_assert(sizeof(ArenaEntry) == 24, "docs/MEMORY.md section 8.3; explicit padding");

// `session_fingerprint` is stamped after seal via registry_set_fingerprint (the app computes it
// over the sealed ids per docs/BUILD.md section 5). Until then it holds registry_seal's own fold
// of the sealed ids (docs/MEMORY.md section 8.3), so restore's id/order gate is real even before
// the app stamps the full fingerprint (W1 mem review 2). It is copied into every Snapshot and
// checked on restore. Field added over the section 8.3 struct - recorded in TODO.md (W1 mem notes).
struct ArenaRegistry {
    ArenaEntry e[MAX_ARENAS];
    u32 count;
    u8  sealed;
    u8  _pad[3];
    u8  session_fingerprint[32];
};
static_assert(__is_trivially_copyable(ArenaRegistry), "");

// Registers `a` under `id` with ARENA_* registry flags. Init only: TL_FATAL if sealed, if
// count == MAX_ARENAS, if `id` is already registered (ids must be unique - they key restore), or
// if `flags` sets ARENA_HASHED without ARENA_SNAPSHOT (docs/MEMORY.md section 1.2: hashed state
// that cannot be rolled back breaks the section 8.8 hash-trace contract).
void registry_add(ArenaRegistry* r, NameHash id, VMemArena* a, u32 flags);

// Freezes count and order; further registry_add is TL_FATAL. Called once at end of init.
// Folds the sealed ids into session_fingerprint (docs/MEMORY.md section 8.3) so restore refuses
// a snapshot whose registry had different ids or a different order even before the app calls
// registry_set_fingerprint (which overwrites the fold with the full BLAKE2b value).
void registry_seal(ArenaRegistry* r);

// Stores the 32-byte session fingerprint (docs/BUILD.md section 5) stamped into snapshots and
// required to match on restore. Call after registry_seal; TL_FATAL before seal.
void registry_set_fingerprint(ArenaRegistry* r, const u8 fingerprint[32]);

// Per-arena rapidhash over [base, used) for every HASHED entry (0 for others) into
// out_per_arena[0..count); returns the world hash = tl_hash64 over those u64s in registry
// order, seed TL_HASH_SEED (docs/DETERMINISM.md section 4). Runs at LAST, before the barrier.
u64 registry_hash_all(const ArenaRegistry* r, u64 out_per_arena[MAX_ARENAS]);

// Copies every SNAPSHOT entry's [base, used) into s->blob (registry order, each segment
// 64-byte aligned), records the used table, tick, and fingerprint. On blob_cap overflow:
// ERR_MEM_RING_OVERFLOW in dev tiers, TL_FATAL in netcode/ship (docs/MEMORY.md section 7 R-2 -
// the caller warns and grows at the next barrier; rationale in TODO.md W1 mem notes).
ErrCode registry_snapshot(const ArenaRegistry* r, Snapshot* s, u64 tick);

// Restores every SNAPSHOT entry from `s`: fingerprint, count and ids must match, else
// ERR_SNAPSHOT_MISMATCH with no state touched. Commits pages as needed, memcpys back, sets
// used, high_water = max(high_water, used). Followed by the post_restore barrier
// (docs/MEMORY.md section 5) - nothing else may observe a restore.
ErrCode registry_restore(ArenaRegistry* r, const Snapshot* s);

// --- the arena-offset guard (docs/MEMORY.md section 2, section 8.4) --------------------------
// Dev/debug only; every function compiles to a no-op elsewhere. Defined in arena_guard.cpp
// (non-det half: guard_tick_end reads the CRT-malloc counter from alloc_shim, an upward symbol
// the det audit forbids - docs/CPP-SUBSET.md section 4). Engine-side only, never sim-called.

struct ArenaGuard {
    u64 used_at_start[MAX_ARENAS];
    u8  in_barrier;
    u8  _pad[7];
};
static_assert(__is_trivially_copyable(ArenaGuard), "");

#if TL_DEV
// Records every registered arena's `used` and the CRT-alloc counter at tick start.
void guard_tick_begin(ArenaGuard* g, const ArenaRegistry* r);
// Opens the barrier-apply window. TL_FATAL if a GROWS_AT_BARRIER arena already grew this tick
// (growth is legal only INSIDE the window). Takes the registry so it can look - section 8.4's
// one-arg spelling cannot enforce its own section 2 semantics (TODO.md, W1 mem notes).
void guard_barrier_begin(ArenaGuard* g, const ArenaRegistry* r);
// Closes the barrier-apply window; GROWS_AT_BARRIER arenas are re-baselined at their new `used`.
void guard_barrier_end(ArenaGuard* g, const ArenaRegistry* r);
// TL_FATAL if any arena grew outside its sanctioned window, or if the CRT-malloc counter moved
// during the tick (vendor libs allocate only through pools - a mid-tick CRT malloc is a leak
// of discipline somewhere, docs/MEMORY.md section 8.4).
void guard_tick_end(ArenaGuard* g, const ArenaRegistry* r);
#else
// Non-dev tiers: the guard compiles out to no-ops (docs/MEMORY.md §8.4 "dev/debug only").
inline void guard_tick_begin(ArenaGuard*, const ArenaRegistry*) {}
// No-op outside dev/debug (docs/MEMORY.md §8.4).
inline void guard_barrier_begin(ArenaGuard*, const ArenaRegistry*) {}
// No-op outside dev/debug (docs/MEMORY.md §8.4).
inline void guard_barrier_end(ArenaGuard*, const ArenaRegistry*) {}
// No-op outside dev/debug (docs/MEMORY.md §8.4).
inline void guard_tick_end(ArenaGuard*, const ArenaRegistry*) {}
#endif
