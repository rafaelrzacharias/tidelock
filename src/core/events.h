#pragma once
// ---------------------------------------------------------------------------------------------
// events.h - EventTable / EventTables: the two-half event arena behind EventQueue<T>.
//
// Spec: docs/ECS.md §5 (design - D15 as written), §10.4 (this header). The typed wrappers
//   (eq_emit<T>/eq_read<T>) live in world.h - they resolve T through tl_info_of and the world's
//   event index; this header is the type-erased machinery.
// Purpose: double-buffered, one-tick-latency events. Every reader sees ALL of last tick's
//   events regardless of system order, so producer and consumer are fully decoupled - no
//   before/after coupling, no fire-and-forget dispatch (the Layr Signal<T> mistake).
// Invariants: each type gets a FIXED capacity (declared at registration, default
//   EVENT_DEFAULT_CAP) in BOTH halves, pushed at registration; eq overflow is a bug, not a drop
//   (TL_CHECK, all tiers). Swap+clear happens ONCE per tick at the LAST -> FIRST barrier
//   (docs/CANON.md barrier step 2), never at phase barriers. "Clear" is write_count = 0: the
//   stale bytes above the count are never readable through the API and the halves are never
//   hashed or snapshotted (docs/DETERMINISM.md §4 - events are transient; their EFFECTS on
//   columns are what the hash sees). After a rollback both halves are cleared
//   (events_clear_all). Drain order within a type = emission order = the total system order.
// Determinism: emission order is the system order (deterministic); the read half is immutable
//   for the whole tick. The halves live in their own two arenas - NOT scratch (the read half
//   must survive past the frame's scratch reset) and NOT the registered set.
// Threading: one writer at a time per table (v0 serial; JOBS.md will chunk-key emission).
// Includes: core/reflect.h, foundation/vmem_arena.h.
// ---------------------------------------------------------------------------------------------
#include "core/reflect.h"
#include "foundation/vmem_arena.h"

enum : u32 {
    MAX_EVENT_TYPES   = 256,    // docs/ECS.md §10.4
    EVENT_DEFAULT_CAP = 4096,   // events per type per tick, per half (docs/ECS.md §10.4)
};

// One event type's two blocks. `write`/`read` are the live views (docs/ECS.md §10.4);
// `block[2]` pins each half's fixed home so the swap can re-derive them (the spec's "other
// half's block", made a stored fact instead of an address recomputation).
struct EventTable {
    const ComponentInfo* info;
    u32 stride;        // == info->size (asserted an align multiple at registration)
    u32 write_count;
    u32 read_count;
    u8* write;
    u8* read;
    u32 cap;
    u32 _pad0;
    u8* block[2];      // block[h] lives in EventTables::half[h]
};

// All event types + the two halves (docs/ECS.md §10.4). write_half names the half `write`
// pointers currently live in; it toggles at every swap.
struct EventTables {
    VMemArena half[2];
    u32 write_half;
    u32 count;
    EventTable t[MAX_EVENT_TYPES];
};

// A type-erased read view (the typed Span<T> lives in world.h's eq_read<T>).
struct EventSlice {
    const u8* data;
    u32 count;
    u32 stride;
};

// Reserves the two halves (reserve_bytes each - a budget, TL_FATAL when registrations exceed
// it); no types yet. The two ids name the arenas for diagnostics only (never registered).
ErrCode events_init(EventTables* ev, NameHash id_half0, NameHash id_half1, u64 reserve_bytes,
                    const VMemApi* os);

// Registers one event type (init only): pushes cap events (0 = EVENT_DEFAULT_CAP) in BOTH
// halves, appends the table, returns its index. TL_FATAL at MAX_EVENT_TYPES or on a duplicate
// name_hash (the EventTypeId - docs/ECS.md §5).
u32 events_register(EventTables* ev, const ComponentInfo* info, u32 cap);

// The table index for an EventTypeId, or MAX_EVENT_TYPES when unknown. Pure. Linear scan -
// registration-time and cold-path callers only; the world holds the hot map.
u32 events_find(const EventTables* ev, EventTypeId id);

// Appends one event (info->size bytes) to table `index`'s write half, O(1). An overflow is a
// bug, not a drop: TL_CHECK(write_count < cap) in every tier (docs/ECS.md §10.4).
void events_emit(EventTables* ev, u32 index, const void* e);

// Last tick's buffer for table `index`: flat, immutable for the whole tick (docs/ECS.md §5).
EventSlice events_read(const EventTables* ev, u32 index);

// The LAST -> FIRST barrier's step 2 (docs/CANON.md): per table, read <- write (pointer and
// count), write <- the other half's block, write_count <- 0; then the half roles toggle.
void events_swap(EventTables* ev);

// Clears BOTH halves of every table (write_count = read_count = 0). The rollback path
// (docs/ECS.md §10.4: "after a rollback both halves are cleared").
void events_clear_all(EventTables* ev);
