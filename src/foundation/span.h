#pragma once
// ---------------------------------------------------------------------------------------------
// span.h - Span<T>: the non-owning, non-resizable view every buffer-returning API speaks.
//
// Spec: docs/CONTAINERS.md §1, §8.1 (pinned shape); docs/CPP-SUBSET.md §2 (sanctioned template).
// Purpose: `{ data, count }` over caller- or arena-owned storage; no allocation, no ownership,
//   no growth. `Array<T>` (the owning, growable counterpart) is a heavier deliverable - its own
//   backing choice, `VMemArena` growth policy - and stays out of this header entirely.
// Invariants: `data` may be null only when `count == 0`. `T` must be trivially copyable
//   (docs/CPP-SUBSET.md §2); nothing here enforces that with a static_assert because a span over
//   an incomplete `T` (forward-declared at the point of use) cannot be sized yet - callers that
//   instantiate a span over a concrete T get the class template's own layout guarantees.
// Determinism: a plain two-field view; safe inside hashed/snapshotted state when T is.
// Threading: none - a value type, no state.
// Includes: foundation/tl_types.h only.
//
// Landed from the W1 platform lane (2026-08-24), not the containers lane: PLATFORM.md §9's
// contract header (`FileApi::read_all` returns `Result<Span<u8>>`) needs the type and the
// containers lane had not started. Transcribed verbatim from the pinned shape in CONTAINERS.md
// §1/§8.1 - same precedent as tl_assert.h landing from the fx lane (LESSONS.md). The containers
// lane owns this file, and `Array<T>`/`array_*`, from the moment it starts.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

template <typename T>
struct Span {
    T* data;
    u32 count;
};

static_assert(__is_trivially_copyable(Span<u32>), "");
static_assert(sizeof(Span<u32>) == 16, "T* (8) + u32 (4) + 4 pad on a 64-bit target");
