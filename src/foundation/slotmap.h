#pragma once
// ---------------------------------------------------------------------------------------------
// slotmap.h - SlotMap<T,H>: the only place a slot is freed and reused (docs/MEMORY.md §1.4).
//   LIFO free-list reuse, generation-wrapped Handle<H> identity, dead slots zeroed and hashed.
//
// Spec: docs/CONTAINERS.md §2 (design), §8.2 (this header); docs/MEMORY.md §3 (Handle, the
//   per-domain cap table); docs/CANON.md "Types" (Entity/resource domain widths).
// Purpose: cross-tick identity for ECS entities, Alloy bodies/constraints/plants/cavities/basins -
//   anything a Handle names. Iteration is 0..slot_cap(), skipping dead slots - NEVER 0..live_count
//   (docs/CONTAINERS.md §2): the dead-slot range is part of the deterministic walk.
// Invariants: free_list is LIFO - insertion is a pure function of the call sequence, itself
//   deterministic (system order). gen starts at 1, 0 is never issued (docs/MEMORY.md §3). Gen-wrap
//   (gen == GEN_MAX on remove) quarantines the slot - it is never pushed back to free_list and
//   never reissued. Removed slots are memset(0) before the live bit clears, so the slots array
//   hashed over [0, slot_cap) - including dead slots - is a pure function of state, not history
//   (docs/CONTAINERS.md §2).
// Determinism: each of the four columns (slots, gen, free_list, live) owns its own VMemArena,
//   reserved to the domain's full capacity from H's IDX_BITS at slotmap_init (docs/MEMORY.md
//   §1.2 R-1 "one VMem range per column"; virtual-address reservation is free, so reserving the
//   handle's whole domain up front needs no separate growth policy for `live`, which is fixed-size
//   at init by contract, docs/CONTAINERS.md §4). This owning-arenas shape and `slotmap_init`'s
//   signature are additions over the rev-1 struct (which lists only the four columns) - folded
//   into CONTAINERS.md in this commit per docs/ROADMAP.md's "signatures added over spec" pattern
//   (the mem lane set this precedent for `ring_init`/`registry_set_fingerprint`).
// Threading: one SlotMap, one writer; no locking.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/vmem_arena.h,
//   foundation/array.h, foundation/bitset.h, foundation/handle.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/vmem_arena.h"
#include "foundation/array.h"
#include "foundation/bitset.h"
#include "foundation/handle.h"
#include <string.h>   // memset (zeroing a removed/fresh slot) and memcpy (inserting *v) - docs/CPP-SUBSET.md §1

template <typename T, typename H>
struct SlotMap {
    Array<T> slots;         // vmem range; dead slots memset(0) on remove
    Array<u16> gen;         // parallel to slots; gen[i] starts at 1, never 0
    Array<u32> free_list;   // LIFO; vmem range
    Bitset live;            // one bit per slot, fixed to the domain's full capacity at init
    u32 live_count;
    u32 quarantined;        // slots retired on generation wrap - never reissued

    VMemArena _slots_arena;
    VMemArena _gen_arena;
    VMemArena _free_arena;
    VMemArena _live_arena;
};

// Reserves all four columns to H's full domain capacity (IDX_MASK + 1 slots - docs/MEMORY.md §3's
// per-domain cap table) and wires the four Arrays/Bitset onto them. Four distinct NameHash ids are
// required (not derived) so the app's ArenaRegistry can register each column under its own name -
// all four are part of the pool's authoritative state and must each be hashed/snapshotted
// independently (docs/MEMORY.md §1.2).
template <typename T, typename H>
ErrCode slotmap_init(SlotMap<T, H>* sm, NameHash id_slots, NameHash id_gen, NameHash id_free,
                      NameHash id_live, const VMemApi* os) {
    static_assert(__is_trivially_copyable(T), "SlotMap<T,H> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    u64 cap = (u64)H::IDX_MASK + 1u;
    // No reserve floor here: vmem_arena_init rounds every reserve UP to COMMIT_GRANULE (ruled
    // 2026-08-24, docs/MEMORY.md section 8.2), so a small-cap domain's columns are already
    // granule-sized and every reserved byte is pushable. The floor this lane shipped as a
    // forward-compatible workaround is dead code now that the ruling has landed (TODO.md, W1
    // containers review 1).
    ErrCode e;
    u64 slots_bytes = cap * sizeof(T);
    u64 gen_bytes   = cap * sizeof(u16);
    u64 free_bytes  = cap * sizeof(u32);
    e = vmem_arena_init(&sm->_slots_arena, id_slots, slots_bytes, ARENA_ZERO_ON_PUSH, os);
    if (e != ERR_OK) { return e; }
    e = vmem_arena_init(&sm->_gen_arena, id_gen, gen_bytes, ARENA_ZERO_ON_PUSH, os);
    if (e != ERR_OK) { return e; }
    e = vmem_arena_init(&sm->_free_arena, id_free, free_bytes, ARENA_ZERO_ON_PUSH, os);
    if (e != ERR_OK) { return e; }
    u64 live_bytes = ((cap + 63u) / 64u) * 8u;
    e = vmem_arena_init(&sm->_live_arena, id_live, live_bytes, ARENA_ZERO_ON_PUSH, os);
    if (e != ERR_OK) { return e; }

    array_init_vmem(&sm->slots, &sm->_slots_arena);
    array_init_vmem(&sm->gen, &sm->_gen_arena);
    array_init_vmem(&sm->free_list, &sm->_free_arena);
    bitset_init(&sm->live, &sm->_live_arena, (u32)cap);
    sm->live_count = 0;
    sm->quarantined = 0;
    return ERR_OK;
}

// The high-water slot count - the correct iteration bound (docs/CONTAINERS.md §2: "iterate
// 0..slot_cap(), skipping dead slots - never 0..live").
template <typename T, typename H>
u32 slotmap_slot_cap(const SlotMap<T, H>* sm) { return sm->slots.count; }

// Inserts a copy of *v. Reuses the most recently freed slot (LIFO) if any; otherwise appends a
// brand-new slot (gen starts at 1). Returns the handle naming the new element.
template <typename T, typename H>
H slotmap_insert(SlotMap<T, H>* sm, const T* v) {
    static_assert(__is_trivially_copyable(T), "SlotMap<T,H> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    u32 idx;
    if (sm->free_list.count > 0u) {
        idx = array_pop(&sm->free_list);   // LIFO reuse; gen[idx] already holds its post-remove value ("gen stays")
    } else {
        idx = sm->slots.count;
        T zero_t; memset(&zero_t, 0, sizeof(T));
        array_push(&sm->slots, zero_t);
        array_push(&sm->gen, (u16)1);       // a brand-new slot's generation starts at 1
    }
    memcpy(&sm->slots.data[idx], v, sizeof(T));
    bitset_set(&sm->live, idx);
    sm->live_count += 1u;
    return handle_make<H>(idx, sm->gen.data[idx]);
}

// Null if h is the null handle, out of range, dead, or a stale generation (TL_ASSERT fires in
// debug on the stale-but-in-range case - a genuine bug signal; a null handle is documented
// absence and never asserts, docs/CPP-SUBSET.md §3).
template <typename T, typename H>
T* slotmap_get(SlotMap<T, H>* sm, H h) {
    if (handle_is_null(h)) { return nullptr; }
    u32 idx = handle_index(h);
    bool ok = idx < sm->slots.count && bitset_test(&sm->live, idx) && sm->gen.data[idx] == (u16)handle_gen(h);
    if (!ok) {
        TL_ASSERT(false);   // stale handle - a documented bug signal, not a normal-flow path
        return nullptr;
    }
    return &sm->slots.data[idx];
}

// Removes h if it is live: zeroes the slot (hash is a function of state, not history), clears the
// live bit, and either bumps gen and pushes idx to the free list (LIFO reuse) or, on generation
// wrap, quarantines the slot (never pushed - never reissued). Returns false if h was not live.
template <typename T, typename H>
bool slotmap_remove(SlotMap<T, H>* sm, H h) {
    if (handle_is_null(h)) { return false; }
    u32 idx = handle_index(h);
    if (idx >= sm->slots.count || !bitset_test(&sm->live, idx) || sm->gen.data[idx] != (u16)handle_gen(h)) {
        return false;
    }
    memset(&sm->slots.data[idx], 0, sizeof(T));
    bitset_clear(&sm->live, idx);
    sm->live_count -= 1u;
    if (sm->gen.data[idx] == (u16)H::GEN_MAX) {
        sm->quarantined += 1u;   // never pushed to free_list - the slot retires until world reset
    } else {
        sm->gen.data[idx] += 1u;
        array_push(&sm->free_list, idx);
    }
    return true;
}
