#pragma once
// ---------------------------------------------------------------------------------------------
// array.h - Array<T> and Span<T>: the two backings every other container in this file builds on.
//
// Spec: docs/CONTAINERS.md §1 (design), §8.1 (this header).
// Purpose: no relocating growth anywhere in the runtime (a relocating grow invalidates every
//   transient pointer in the pass - docs/CONTAINERS.md §6). Two backings: (a) vmem-owned - a
//   dedicated VMemArena per array, stable base forever, grows by committing more of its own
//   reserve (docs/MEMORY.md §1.2 R-1: one VMem range per column); (b) fixed - one arena_push from
//   a shared/scratch arena, TL_FATAL on overflow.
// Invariants: static_assert(__is_trivially_copyable(T)) in every function template (memcpy-safe
//   is what makes snapshot/hash legal, docs/CONTAINERS.md §0). Walk order is 0..count, packed;
//   swap_remove reorders (a pure function of the call sequence) - callers needing stable order
//   use SlotMap or tombstones. Every operation that vacates a slot (pop, swap_remove, clear)
//   ZEROES it: a vmem-backed array's arena `used` covers [0, cap), so [count, cap) is inside the
//   hashed extent and stale bytes would make a hashed array's hash a function of removal history
//   (docs/CPP-SUBSET.md §5, and the rule SlotMap's dead-slot zeroing already follows). clear does
//   NOT release capacity.
// Determinism: no floats, no padding beyond T's own (T is the caller's contract). Vmem-backed
//   growth commits whole pages via arena_push, which is itself deterministic (docs/MEMORY.md
//   §8.2); the array never reads arena->used as an element count - cap tracks committed capacity,
//   count tracks logical length, independently.
// Threading: one array, one writer; no locking.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/vmem_arena.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/vmem_arena.h"
#include <string.h>   // memset - vacated elements are zeroed (docs/CPP-SUBSET.md section 5)

template <typename T>
struct Span {
    T* data;
    u32 count;
};

template <typename T>
struct Array {
    T* data;
    u32 count;
    u32 cap;
    VMemArena* grow_arena;   // vmem-backed: the array's own arena, growth commits more of it. null = fixed.
};

// Vmem-backed init: `own_range` is a VMemArena reserved for this array's exclusive use (docs/
// MEMORY.md §1.2 R-1 - one VMem range per column). data = own_range->base; cap starts at 0 and
// grows by committing whole pages on the first push past capacity.
template <typename T>
void array_init_vmem(Array<T>* a, VMemArena* own_range) {
    static_assert(__is_trivially_copyable(T), "Array<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    a->data = (T*)own_range->base;
    a->count = 0;
    a->cap = 0;
    a->grow_arena = own_range;
}

// Fixed-capacity init: one arena_push(cap * sizeof(T)) from a shared arena (scratch, command
// buffers). grow_arena is left null so array_push TL_FATALs on overflow instead of growing.
template <typename T>
void array_init_fixed(Array<T>* a, VMemArena* arena, u32 cap) {
    static_assert(__is_trivially_copyable(T), "Array<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    a->data = cap == 0 ? nullptr : (T*)arena_push(arena, (u64)cap * sizeof(T), alignof(T));
    a->count = 0;
    a->cap = cap;
    a->grow_arena = nullptr;
}

// Appends `v`. Vmem-backed: grows cap by one page's worth of elements (at least one) via
// arena_push when count == cap. Fixed: TL_FATAL on overflow (docs/CONTAINERS.md §8.1). Returns a
// pointer to the newly-pushed element (tick-scoped; stable across further vmem growth since the
// base never moves - not stable across a fixed array's lifetime, which never grows).
template <typename T>
T* array_push(Array<T>* a, T v) {
    static_assert(__is_trivially_copyable(T), "Array<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    if (a->count == a->cap) {
        if (a->grow_arena == nullptr) {
            TL_FATAL("array_push: fixed array overflow");
        }
        u32 page = a->grow_arena->page;
        u32 grow_elems = (u32)(page / sizeof(T));
        if (grow_elems == 0) { grow_elems = 1; }
        void* p = arena_push(a->grow_arena, (u64)grow_elems * sizeof(T), alignof(T));
        TL_ASSERT(p == (void*)(a->data + a->cap));   // the array owns the whole arena range (R-1) - no interloper pushed here
        a->cap += grow_elems;
    }
    a->data[a->count] = v;
    return &a->data[a->count++];
}

// Removes and returns the last element, ZEROING the vacated slot. TL_ASSERT(count > 0).
// The zero is not bookkeeping: an Array's own vmem range is committed to `cap`, so the arena's
// `used` - and therefore the hashed extent [base, used) - covers [count, cap). Leaving the popped
// element's bytes there makes a HASHED array's hash a function of removal history, which
// docs/CPP-SUBSET.md section 5 ("hashed state ... zero-filled arenas") and vmem_arena.h's own
// determinism block forbid, and which SlotMap already honours by zeroing dead slots.
template <typename T>
T array_pop(Array<T>* a) {
    static_assert(__is_trivially_copyable(T), "Array<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    TL_ASSERT(a->count > 0);
    a->count -= 1;
    T v = a->data[a->count];
    memset(&a->data[a->count], 0, sizeof(T));
    return v;
}

// Removes index i by moving the last element into its place (order is NOT preserved - a pure
// function of the call sequence, docs/CONTAINERS.md §1). TL_CHECK(i < count) in all tiers.
template <typename T>
void array_swap_remove(Array<T>* a, u32 i) {
    static_assert(__is_trivially_copyable(T), "Array<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    TL_CHECK(i < a->count);
    a->count -= 1;
    a->data[i] = a->data[a->count];       // self-copy when i was the last element
    memset(&a->data[a->count], 0, sizeof(T));   // the vacated tail slot - see array_pop
}

// Bounds-checked element access, TL_CHECK(i < count) in all tiers (docs/CONTAINERS.md §8.1).
template <typename T>
T& array_at(Array<T>* a, u32 i) {
    static_assert(__is_trivially_copyable(T), "Array<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    TL_CHECK(i < a->count);
    return a->data[i];
}

// A view over the whole array [0, count). Never fails; not tick-restricted.
template <typename T>
Span<T> array_span(Array<T>* a) {
    static_assert(__is_trivially_copyable(T), "Array<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    return Span<T>{ a->data, a->count };
}

// [lo, hi) over the array. TL_CHECK(lo <= hi && hi <= count) in all tiers.
template <typename T>
Span<T> array_slice(Array<T>* a, u32 lo, u32 hi) {
    static_assert(__is_trivially_copyable(T), "Array<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    TL_CHECK(lo <= hi && hi <= a->count);
    return Span<T>{ a->data + lo, hi - lo };
}

// count = 0, ZEROING the cleared elements. Does NOT release capacity (docs/CONTAINERS.md §8.1).
// Rev 1 said the tail was "re-zeroed by the owning arena's ARENA_ZERO_ON_PUSH policy on reuse";
// that mechanism does not exist for an in-place refill - ARENA_ZERO_ON_PUSH only re-zeroes bytes
// an arena_push walks over, and a cleared array pushes nothing until count climbs back past cap.
// The stale range sits below `used` and is hashed. Corrected in CONTAINERS.md §8.1 by the W1
// containers review; O(count * sizeof T), paid only by callers that actually clear.
template <typename T>
void array_clear(Array<T>* a) {
    static_assert(__is_trivially_copyable(T), "Array<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    if (a->count != 0u) { memset(a->data, 0, (usize)a->count * sizeof(T)); }
    a->count = 0;
}
