#pragma once
// ---------------------------------------------------------------------------------------------
// snapshot.h - Snapshot and the rollback SnapshotRing (T-F-04).
//
// Spec: docs/MEMORY.md §1.2 (design), §5 (what restore covers), §8.3 (these structs);
//   ring depth = CONFIRMATION_HORIZON_TICKS (docs/CANON.md, owner docs/NETCODE.md App. B).
// Purpose: a Snapshot is a memcpy of every SNAPSHOT-flagged arena's [base, used), stamped with
//   tick + session fingerprint; the ring holds the N most recent confirmed ones, allocated ONCE
//   at init - rollback, late-join/resync, replay keyframes and the desync package all ride it
//   (docs/DETERMINISM.md section 5).
// Invariants: blob layout is arena segments in REGISTRY ORDER, each segment start 64-byte
//   aligned; `used[]` records per-arena extents so restore copies only [base, used). Slot blobs
//   are sized once at the budget (docs/MEMORY.md section 6) - Sum(used) > blob_cap is a budget
//   violation (section 7 R-2): named error in dev tiers, TL_FATAL in netcode/ship.
// Determinism: snapshots are taken at the barrier from sealed state; the blob itself is never
//   hashed - the per-arena hashes are (docs/DETERMINISM.md section 4).
// Threading: single-threaded at the barrier; the ring has one owner (the loop / netcode).
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/arena_registry.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/arena_registry.h"

// docs/CANON.md "Netcode constants" (owner docs/NETCODE.md App. B): the rollback horizon in
// ticks, and therefore the ring depth. The netcode lane's header must agree with this value.
enum : u32 { CONFIRMATION_HORIZON_TICKS = 6 };

// docs/MEMORY.md section 8.3. `used[i]` is arena i's extent at capture (registry order);
// `blob` is the payload; `blob_cap` its budget. session_fingerprint gates restore.
struct Snapshot {
    u8  session_fingerprint[32];
    u64 tick;
    u32 count;
    u32 _pad0;
    u64 used[MAX_ARENAS];
    u8* blob;
    u64 blob_cap;
};
static_assert(__is_trivially_copyable(Snapshot), "");

struct SnapshotRing { Snapshot slot[CONFIRMATION_HORIZON_TICKS]; u32 head; u32 count; };

// Allocates every slot's blob ONCE from `backing`: slot_cap_bytes each, 64-byte aligned
// (docs/MEMORY.md section 1.2 "allocated once"; the cap is the budgeted Sum(used) of
// snapshotted arenas, section 6 - T-A-03 replaces the guess). Added over the section 8.3
// signature list - recorded in TODO.md (W1 mem notes). ERR_MEM_BAD_ARG on zero cap/null.
ErrCode ring_init(SnapshotRing* g, u64 slot_cap_bytes, VMemArena* backing);

// Claims the next slot (evicting the oldest once full), stamps `tick`, and returns it for
// registry_snapshot to fill. The claimed slot is invalidated (count = 0) until that fill
// succeeds, so ring_find can never surface the evicted snapshot's payload under the new tick.
// Never null after a successful ring_init.
Snapshot* ring_push(SnapshotRing* g, u64 tick);

// The slot holding `tick`, or null if it has been evicted / never pushed.
const Snapshot* ring_find(const SnapshotRing* g, u64 tick);
