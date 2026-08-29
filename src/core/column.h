#pragma once
// ---------------------------------------------------------------------------------------------
// column.h - ComponentTable: one component type's paged sparse-set column.
//
// Spec: docs/ECS.md §2 (design), §10.3 (this header). Consumed by world.h (registration, the
//   typed accessors) and commands.cpp (the barrier appliers - the only writers).
// Purpose: packed dense rows + a paged sparse index (entity index -> dense index) per component
//   type. Add/remove O(1), remove is swap-remove, iteration is 0..count packed - the walk order
//   every system sees, deterministic for a given world state (docs/DETERMINISM.md §2 rule 1).
// Invariants: three own VMem ranges per column (docs/MEMORY.md §7 R-1). dense and entities are
//   registered HASHED|SNAPSHOT|GROWS_AT_BARRIER by world_register_component; the sparse pages
//   arena is SNAPSHOT only (derivable from entities[], snapshotted anyway for O(1) restore -
//   docs/ECS.md §10.3). HASHING RULING (RR-48, ruled 2026-08-28): the hashed extent is the arena's
//   [base, used), and FOR A COLUMN `used` IS the live extent - `column_remove` shrinks the dense
//   and entity arenas by one row, so `used == count * stride` invariantly. Vacated rows are still
//   zeroed, now as defence rather than as the mechanism.
//   SCOPE, corrected 2026-08-28 (PR #17 review, D5): this ruling is about the COLUMN and retires
//   nothing in docs/CONTAINERS.md §1. §1's rule - a vmem-backed Array's arena `used` covers its
//   whole committed capacity, so [count, cap) is hashed - is still true and still correct, because
//   a ComponentTable is not an Array. An earlier draft of this block claimed to supersede a
//   "never shrinks form of the Array ruling"; §1 contains no such form, and the misattribution
//   would have told a reader of §1 that columns had changed something there. They have not.
//   WHY the column's earlier form was wrong, since it read as a considered ruling and was: it made
//   the EXTENT "a pure function of the OP HISTORY, never of what was removed". History-dependence
//   was the accepted design, and it is the defect - `used` is a high-water mark, so two worlds
//   holding identical live rows hash a different NUMBER OF BYTES whenever one reached that state
//   by a different add/remove path. Demonstrated by this module's own regression row,
//   world_divergent_histories_hash_the_same_extent: same live rows built two ways, one extent
//   required, and it fails without the shrink.
//   TWO CHANNELS, and RR-48 closes exactly ONE of them (RR-54, ruled 2026-08-28 by Rafael, on the
//   PR #17 ship round's D1). History reaches a column's hash by two independent routes:
//     (1) EXTENT - how many bytes get hashed. `used` as a high-water mark. CLOSED by this ruling.
//     (2) DENSE ORDER - which permutation those bytes are in. column_remove is swap-remove, so the
//         packed order is a function of WHICH row was removed. NOT closed, and deliberately not:
//         packed order is part of the walk contract (docs/ECS.md §2), and every consumer reads a
//         column BY ENTITY, never by dense index. src/core/interp.cpp:29 is the standing statement
//         of the order channel and remains its one home - this block cites it, never restates it.
//   So two worlds with identical live rows AND identical extent can still hash differently, by
//   design. What a consumer may rely on: a RESTORE-based rejoin is order-stable (registry_restore
//   memcpys the bytes, so the permutation survives verbatim); a REPLAY-based rejoin is NOT, since
//   it rebuilds dense order from its own command history. A late-join path that replays must
//   compare live state by entity, not by hashing a column and expecting a peer to match.
//   The negation is pinned by world_divergent_removal_order_hashes_differently_by_design, which
//   asserts the hashes DIFFER while the extents match - LESSONS.md requires a property that is
//   false by design to have its negation tested and said out loud, so nobody "fixes" it later.
//   BOUNDING, and it is load-bearing (PR #17 review, D6; TODO.md records it as such): this is NOT
//   a live desync in the tree today. Lockstep peers replay one command stream, so their histories
//   match; the documented same-world save/reload path is clean; and no netcode consumer of
//   save_read exists yet. It becomes live at the first save/rejoin/late-join consumer - which is
//   the horizon RR-48 was scheduled against, not a present fire. An earlier draft of this block
//   stated the divergence unqualified AND cited a save-restored-world scenario that the review
//   could not reconstruct in this tree (save_read re-adds against saved entity handles that must
//   already be live, and the entity slotmap has no save encoder), so the scenario is replaced here
//   by the two-worlds demonstration that the regression row actually rests on.
//   Structure changes only inside the barrier window (GROWS_AT_BARRIER; commands.cpp is the
//   writer). A shrink there is SANCTIONED, not merely tolerated - docs/MEMORY.md §2, amended
//   2026-08-28; the guard's `used != used_at_start` comparison is the enforcement, not the
//   argument (PR #17 review, D2).
// Determinism: sparse pages are committed on demand and filled with ECS_SPARSE_NONE; the
//   page-pointer array is fixed at init (the full Entity domain's 1024 page slots), so pointer
//   values are a pure function of the arena base and layout, restore-stable. The generation in
//   Entity's bits makes a stale handle read as absent in column_get (entities[d] compare).
// Threading: one writer (the barrier applier / init), readers inside the tick; no locking.
// Includes: core/reflect.h, foundation/vmem_arena.h.
// ---------------------------------------------------------------------------------------------
#include "core/reflect.h"
#include "foundation/vmem_arena.h"

// The sparse-set page geometry (docs/CANON.md "Sizes and caps": 4096-entry pages of u32 dense
// indices; docs/ECS.md §10.3 spells these PAGE_SHIFT/PAGE_SIZE/NONE - prefixed here because
// they are global names and `NONE` is claimed by half the platform headers in existence).
enum : u32 {
    ECS_PAGE_SHIFT  = 12,
    ECS_PAGE_SIZE   = 4096,
    ECS_SPARSE_NONE = 0xFFFFFFFFu,
};

// One component type's storage (docs/ECS.md §10.3, field for field; the arenas live inline so
// a column is self-contained and World's comps[] array is the only home).
struct ComponentTable {
    const ComponentInfo* info;
    VMemArena dense_arena;    // packed rows, stride = info->size; registered HASHED|SNAPSHOT|GROWS_AT_BARRIER
    VMemArena entity_arena;   // dense -> Entity (4 B rows), registered like dense
    VMemArena page_arena;     // page-pointer array + committed pages; registered SNAPSHOT only
    u8*      dense;
    u32      count;
    u32      stride;          // == info->size (asserted a multiple of info->align at init)
    Entity*  entities;
    u32**    pages;           // entity index >> ECS_PAGE_SHIFT -> page of 4096 u32, null until committed
    u32      page_count;      // fixed at init: the full Entity domain / ECS_PAGE_SIZE
};

// Reserves the three arenas (dense/entities to the domain's worst case, pages to the fixed
// pointer array + all pages), pushes the zeroed page-pointer array, and wires the members.
// The three NameHash ids name the arenas in the registry (world.cpp registers them; a test may
// register none). Returns the first failing vmem init's code; ERR_OK otherwise.
ErrCode column_init(ComponentTable* t, const ComponentInfo* info,
                    NameHash id_dense, NameHash id_entity, NameHash id_pages, const VMemApi* os);

// The dense row for e, or null when e has no row here OR e is stale (the entities[d] bits
// compare is the generation check - docs/ECS.md §10.3). Pointer is tick-scoped: the next
// barrier's swap-removes may move the row. Pure, all tiers, no assert.
void* column_get(ComponentTable* t, Entity e);

// Adds e's row (a copy of value, info->size bytes). Barrier-window only (arena growth).
// TL_CHECK: e must not already have a row here. value must not alias the column.
void column_add(ComponentTable* t, Entity e, const void* value);

// Swap-removes e's row; the vacated tail row and its entity slot are zeroed (the hashing
// ruling above). Barrier-window only. TL_CHECK: e has a row; TL_ASSERT: it is e's own (a stale
// remove reaching a reused slot is a caller bug - the appliers check liveness first).
void column_remove(ComponentTable* t, Entity e);
