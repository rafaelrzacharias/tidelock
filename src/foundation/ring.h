#pragma once
// ---------------------------------------------------------------------------------------------
// ring.h - RingBuffer<T>: fixed capacity (power of two), optional overwrite-oldest.
//
// Spec: docs/CONTAINERS.md §4 (design), §8.5 (this header).
// Purpose: event queues' persistent mode, the netcode redundancy window, the log ring, Alloy's
//   event stream.
// Invariants: cap is a power of two (head/tail wrap via mask, no modulo-by-non-pow2 divergence
//   risk); overwrite_oldest is a per-instance flag, not a compile-time switch (docs/CONTAINERS.md
//   §4). `ring_init` is a signature added over the rev-1 spec (folded into CONTAINERS.md this
//   commit) - one arena_push of cap*sizeof(T).
// Determinism: push/pop/peek are pure functions of (state, call); no floats, no padding beyond
//   T's own.
// Threading: one ring, one writer; no locking.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/vmem_arena.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/vmem_arena.h"

template <typename T>
struct RingBuffer {
    T* data;
    u32 cap;             // power of two
    u32 head, tail;       // head = next pop/peek(0) slot; tail = next push slot; both mod cap via mask
    u8 overwrite_oldest;
    u8 _pad0[3];
};

// True iff n is a nonzero power of two. Pure, never fails.
inline bool ring_is_pow2(u32 n) { return n != 0u && (n & (n - 1u)) == 0u; }

// One arena_push(cap * sizeof(T)). TL_ASSERT(ring_is_pow2(cap)).
template <typename T>
void ring_init(RingBuffer<T>* r, VMemArena* arena, u32 cap, bool overwrite_oldest) {
    static_assert(__is_trivially_copyable(T), "RingBuffer<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    TL_ASSERT(ring_is_pow2(cap));
    r->data = (T*)arena_push(arena, (u64)cap * sizeof(T), alignof(T));
    r->cap = cap;
    r->head = 0;
    r->tail = 0;
    r->overwrite_oldest = overwrite_oldest ? 1u : 0u;
    r->_pad0[0] = 0; r->_pad0[1] = 0; r->_pad0[2] = 0;
}

// The live element count (tail - head, a wrapping subtract over u32, exact under cap <= 2^32).
// Pure, never fails.
template <typename T>
u32 ring_count(const RingBuffer<T>* r) { return r->tail - r->head; }

// True iff count == cap. Pure, never fails.
template <typename T>
bool ring_full(const RingBuffer<T>* r) { return ring_count(r) == r->cap; }

// True iff count == 0. Pure, never fails.
template <typename T>
bool ring_empty(const RingBuffer<T>* r) { return r->head == r->tail; }

// Pushes v at tail. If full: overwrite_oldest advances head (dropping the oldest element) and
// returns true; otherwise returns false and the ring is unchanged (docs/CONTAINERS.md §8.5).
template <typename T>
bool ring_push(RingBuffer<T>* r, T v) {
    static_assert(__is_trivially_copyable(T), "RingBuffer<T> requires trivially-copyable T (docs/CONTAINERS.md section 0)");
    if (ring_full(r)) {
        if (!r->overwrite_oldest) { return false; }
        r->head += 1u;
    }
    r->data[r->tail & (r->cap - 1u)] = v;
    r->tail += 1u;
    return true;
}

// Peeks the element `i` slots from the oldest live element (0 = the next pop). TL_CHECK(i <
// count) in all tiers.
template <typename T>
T ring_peek(const RingBuffer<T>* r, u32 i) {
    TL_CHECK(i < ring_count(r));
    return r->data[(r->head + i) & (r->cap - 1u)];
}

// Pops and returns the oldest live element. TL_ASSERT(!ring_empty(r)).
template <typename T>
T ring_pop(RingBuffer<T>* r) {
    TL_ASSERT(!ring_empty(r));
    T v = r->data[r->head & (r->cap - 1u)];
    r->head += 1u;
    return v;
}
