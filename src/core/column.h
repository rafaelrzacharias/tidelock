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
//   docs/ECS.md §10.3). HASHING RULING (RR-48, ruled 2026-08-28, superseding the "never shrinks"
//   form of the Array<T> ruling, docs/CONTAINERS.md §1): the hashed extent is the arena's
//   [base, used), and for a column `used` IS the live extent - `column_remove` shrinks the dense
//   and entity arenas by one row, so `used == count * stride` invariantly. Vacated rows are still
//   zeroed, now as defence rather than as the mechanism.
//   WHY the earlier form was wrong, since it read as a considered ruling and was: it made the
//   extent "a pure function of the OP HISTORY, never of what was removed". History-dependence was
//   the accepted design, and it is the defect - `used` is a high-water mark, so two peers holding
//   identical logical state hash differently whenever one of them reached it by a different
//   add/remove path. Measured by the W3 wave sweep (area B, B-1): a world restored from a save
//   carried two live rows in a 24-byte extent against three rows' worth on the peer that had
//   removed one, and the world hashes diverged with every live byte equal.
//   Structure changes only inside the barrier window (GROWS_AT_BARRIER; commands.cpp is the
//   writer) - and the guard compares `used != used_at_start`, so a shrink inside the window is
//   exactly as legal as a growth and one outside is equally fatal.
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
