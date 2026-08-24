#pragma once
// ---------------------------------------------------------------------------------------------
// handle.h - the generational handle: Handle<Tag,IDX_BITS,GEN_BITS>, handle_make/index/gen/is_null.
//
// Spec: docs/MEMORY.md §3, §8.5 (pinned shape); docs/CANON.md ("Types").
// Purpose: one generational-index template shared by every domain that reuses slots (entities,
//   resources, Alloy bodies, platform's Tex/Thread/Sem/Mutex/Watch handles).
// Invariants: `bits == 0` is always null; generation 0 is never issued, so zero-init memory is
//   never a valid handle. `Tag` is a distinct empty struct per domain - it exists only so two
//   domains of the same width never implicitly convert into each other.
// Determinism: trivially copyable, one integer field; safe inside hashed/snapshotted state.
// Threading: none - a value type, no state.
// Includes: foundation/tl_types.h, foundation/tl_assert.h (the panic ABI, docs/CPP-SUBSET.md §9 R-3).
//
// Landed from the W1 platform lane (2026-08-24), not the mem lane: PLATFORM.md §9's contract
// header needs the type and MEMORY.md's lane had not started. Transcribed verbatim from the
// pinned shape in MEMORY.md §3/§8.5 - same precedent as tl_assert.h landing from the fx lane
// (LESSONS.md). The mem lane owns this file from the moment it starts; a conflicting future
// definition there wins and this note is stale.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"

template <typename Tag, int IDX_BITS, int GEN_BITS>
struct Handle {
    uint_fit<IDX_BITS + GEN_BITS> bits;
    static constexpr u32 IDX_BITS_V = (u32)IDX_BITS;
    static constexpr u32 GEN_BITS_V = (u32)GEN_BITS;
    static constexpr u32 IDX_MASK = (1u << IDX_BITS) - 1u;
    static constexpr u32 GEN_MAX  = (1u << GEN_BITS) - 1u;
};

// Packs idx into the low IDX_BITS, gen into the bits above it. gen must be in [1, GEN_MAX] -
// generation 0 is reserved for "never issued" so a zeroed Handle reads as null.
template <typename H>
constexpr H handle_make(u32 idx, u32 gen) {
    TL_ASSERT(idx <= H::IDX_MASK);
    TL_ASSERT(gen >= 1u && gen <= H::GEN_MAX);
    using Bits = decltype(H::bits);
    return H{ (Bits)((gen << H::IDX_BITS_V) | idx) };
}

// Returns the low IDX_BITS of h.bits - undefined meaning on a null handle (check first).
template <typename H>
constexpr u32 handle_index(H h) {
    return (u32)h.bits & H::IDX_MASK;
}

// Returns the generation field of h.bits - 0 only for a null handle.
template <typename H>
constexpr u32 handle_gen(H h) {
    return ((u32)h.bits >> H::IDX_BITS_V) & H::GEN_MAX;
}

// bits == 0 is the one null representation; zero-init memory always reads as null.
template <typename H>
constexpr bool handle_is_null(H h) {
    return h.bits == 0;
}

static_assert(__is_trivially_copyable(Handle<struct HandleSelfTestTag, 22, 10>), "");
static_assert(sizeof(Handle<struct HandleSelfTestTag2, 22, 10>) == 4, "22+10 bits fits u32");
static_assert(sizeof(Handle<struct HandleSelfTestTag3, 12, 4>) == 2, "12+4 bits fits u16");
