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
//   use SlotMap or tombstones. array_clear does NOT release or zero - a hashed array's re-used
//   tail is re-zeroed by the owning arena's ARENA_ZERO_ON_PUSH policy on the next growth past the
//   old high-water mark, not by this file (docs/MEMORY.md §1.1's asymmetry).
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

// Removes and returns the last element. TL_ASSERT(count > 0).
template <typename T>
T array_pop(Array<T>* a) {
    static_assert(__is_trivially_copyable(T), "Array<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    TL_ASSERT(a->count > 0);
    a->count -= 1;
    return a->data[a->count];
}

// Removes index i by moving the last element into its place (order is NOT preserved - a pure
// function of the call sequence, docs/CONTAINERS.md §1). TL_CHECK(i < count) in all tiers.
template <typename T>
void array_swap_remove(Array<T>* a, u32 i) {
    static_assert(__is_trivially_copyable(T), "Array<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    TL_CHECK(i < a->count);
    a->count -= 1;
    a->data[i] = a->data[a->count];
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

// count = 0. Does NOT release capacity and does NOT zero the tail (docs/CONTAINERS.md §8.1) - a
// hashed array's reused bytes are re-zeroed by the owning arena's ARENA_ZERO_ON_PUSH policy.
template <typename T>
void array_clear(Array<T>* a) {
    static_assert(__is_trivially_copyable(T), "Array<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    a->count = 0;
}
