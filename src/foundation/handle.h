#pragma once
// ---------------------------------------------------------------------------------------------
// handle.h - Handle<Tag, IDX_BITS, GEN_BITS>: the generational id, the only cross-pool
//   reference type.
//
// Spec: docs/MEMORY.md §3 (design + the domain table), §8.5 (this header);
//   docs/CANON.md "Types" (Entity = Handle<EntityTag, 22, 10>, resources Handle<_, 12, 4>).
// Purpose: no pointers in authoritative state - handles/indices only (docs/MEMORY.md section 0
//   rule 2). SlotMap is the only place a slot is freed and reused; it bumps the generation so a
//   stale handle fails loudly (debug) or reads as absent (release).
// Invariants: bits == 0 is the null handle; generation 0 is NEVER issued, so zero-initialised
//   memory is never a valid handle (docs/CPP-SUBSET.md section 3). Layout: gen in the high
//   GEN_BITS, index in the low IDX_BITS. Generation-wrap policy is per domain (Entity:
//   quarantine on wrap - the SlotMap's job, docs/MEMORY.md section 3).
// Determinism: handle bits are state - they survive snapshot/restore and are hashed wherever
//   their pool is. Pure constexpr functions, no state here.
// Threading: none - values.
// Includes: foundation/tl_types.h, foundation/tl_assert.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"

// A closed value template (docs/CPP-SUBSET.md section 2). Instantiated per domain with a tag
// struct (`struct EntityTag;`) declared by the domain's owner.
template <typename Tag, int IDX_BITS, int GEN_BITS>
struct Handle {
    static_assert(IDX_BITS >= 1 && GEN_BITS >= 1, "a handle needs both an index and a generation");
    static_assert(IDX_BITS <= 31 && GEN_BITS <= 31, "masks are u32 (docs/MEMORY.md section 8.5)");
    static_assert(IDX_BITS + GEN_BITS <= 64, "bits must fit one integer");

    using rep = uint_fit<IDX_BITS + GEN_BITS>;
    rep bits;   // 0 = null; otherwise gen << IDX_BITS | idx with gen >= 1

    static constexpr u32 IDX_MASK = (1u << IDX_BITS) - 1;
    static constexpr u32 GEN_MAX  = (1u << GEN_BITS) - 1;
    static constexpr int IDX_BITS_N = IDX_BITS;
};

// Builds a handle from a slot index and a generation. Preconditions (TL_ASSERT): idx <=
// IDX_MASK, gen in [1, GEN_MAX] - generation 0 is never issued.
template <typename H> constexpr H handle_make(u32 idx, u32 gen) {
    TL_ASSERT(idx <= H::IDX_MASK);
    TL_ASSERT(gen >= 1u && gen <= H::GEN_MAX);
    return H{ (typename H::rep)(((u64)gen << H::IDX_BITS_N) | (u64)idx) };
}

// The slot index; meaningful only for non-null handles.
template <typename H> constexpr u32 handle_index(H h) { return (u32)(h.bits & H::IDX_MASK); }

// The generation; 0 only for the null handle.
template <typename H> constexpr u32 handle_gen(H h) { return (u32)(((u64)h.bits >> H::IDX_BITS_N) & H::GEN_MAX); }

// True for the null handle (bits == 0 - zero-init memory is never valid).
template <typename H> constexpr bool handle_is_null(H h) { return h.bits == 0; }

// The shapes docs/CANON.md pins. (EntityTag itself is the ECS lane's; the asserts use local tags.)
namespace mem {
struct HandleShapeCheck32Tag;  struct HandleShapeCheck16Tag;
static_assert(sizeof(Handle<HandleShapeCheck32Tag, 22, 10>) == 4, "Entity shape is u32 (docs/CANON.md)");
static_assert(sizeof(Handle<HandleShapeCheck16Tag, 12, 4>) == 2, "resource shape is u16 (docs/CANON.md)");
static_assert(__is_trivially_copyable(Handle<HandleShapeCheck32Tag, 22, 10>), "");
}  // namespace mem
