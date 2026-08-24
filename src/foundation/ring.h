#pragma once
// ---------------------------------------------------------------------------------------------
// ring.h - RingBuffer<T>: fixed-capacity ring, optional overwrite-oldest.
//
// Spec: docs/CONTAINERS.md §8.5 (pinned shape); docs/CANON.md ("Sanctioned templates").
// Purpose: the one ring shape shared by event queues' persistent mode, the netcode redundancy
//   window, the log ring, Alloy's event stream and - the reason it landed here - the platform
//   event pump's RawEvent ring (docs/PLATFORM.md §2).
// Invariants: `cap` is a power of two, set once by the owner and never by this header (no
//   allocation lives here); `head` is the write cursor, `tail` the read cursor, both monotonic
//   u32 counters wrapped only at index time via `& (cap - 1)`, so `head - tail` is always the
//   live count without a modulo. `data` is caller-owned (arena-backed), sized `cap` elements.
// Determinism: `T` must be trivially copyable (POD); no ordering ambiguity - push/pop/peek are
//   pure functions of the counters.
// Threading: none here - single-writer/single-reader disciplines belong to the caller (the event
//   pump is main-thread-only, docs/PLATFORM.md §9.3).
// Includes: foundation/tl_types.h, foundation/tl_assert.h (the panic ABI, docs/CPP-SUBSET.md §9 R-3).
//
// Landed from the W1 platform lane (2026-08-24), not the containers lane: PLATFORM.md §9's
// contract header (EventApi::pump) needs the type and the containers lane had not started.
// Transcribed verbatim from the pinned shape in CONTAINERS.md §8.5 - same precedent as
// tl_assert.h landing from the fx lane (LESSONS.md). The containers lane owns this file, and its
// Bitset/sort neighbours, from the moment it starts.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"

template <typename T>
struct RingBuffer {
    T* data;
    u32 cap;              // power of two
    u32 head, tail;       // head = next write slot, tail = oldest unread slot; both monotonic
    u8 overwrite_oldest;
};

// Number of live elements - always `head - tail`, valid even across the u32 wrap of the counters
// themselves (unsigned subtraction), never across `cap` (that would be a logic bug upstream).
template <typename T>
constexpr u32 ring_count(const RingBuffer<T>* r) {
    return r->head - r->tail;
}

// Appends v. When full: overwrite_oldest drops the oldest element (tail advances) and the push
// succeeds; otherwise the push is refused and the buffer is unchanged.
template <typename T>
constexpr bool ring_push(RingBuffer<T>* r, T v) {
    if (ring_count(r) == r->cap) {
        if (!r->overwrite_oldest) return false;
        ++r->tail;
    }
    r->data[r->head & (r->cap - 1u)] = v;
    ++r->head;
    return true;
}

// Reads the i-th live element (0 = oldest) without removing it. i must be < ring_count(r).
template <typename T>
constexpr T ring_peek(const RingBuffer<T>* r, u32 i) {
    TL_ASSERT(i < ring_count(r));
    return r->data[(r->tail + i) & (r->cap - 1u)];
}

// Removes and returns the oldest element. The buffer must be non-empty.
template <typename T>
constexpr T ring_pop(RingBuffer<T>* r) {
    TL_ASSERT(ring_count(r) > 0u);
    T v = r->data[r->tail & (r->cap - 1u)];
    ++r->tail;
    return v;
}

// True iff no live elements remain - equivalent to `ring_count(r) == 0`.
template <typename T>
constexpr bool ring_empty(const RingBuffer<T>* r) {
    return r->head == r->tail;
}
