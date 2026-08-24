#pragma once
// ---------------------------------------------------------------------------------------------
// bitset.h - Bitset: fixed bit count at init, u64 words, walk order = bit order.
//
// Spec: docs/CONTAINERS.md §4 (design), §8.5 (this header).
// Purpose: sleep flags, dirty chunks, action-map masks, and SlotMap's live-slot bitmap.
// Invariants: bit_count is fixed at init (docs/CONTAINERS.md §4 - "fixed bit count at init");
//   words come from a caller-supplied arena, one arena_push, zeroed by fresh OS pages. Walk order
//   is bit order (0..bit_count), a pure function of which bits are set - never of set/clear
//   history. `bitset_init` is a signature added over the rev-1 spec (folded into CONTAINERS.md in
//   this commit, docs/ROADMAP.md's "signatures added over spec, same commit" pattern): the doc
//   states the struct and the operations, not how it is constructed.
// Determinism: word count = ceil(bit_count / 64); the tail bits of the last word above bit_count
//   are never read by any operation here (find_first/popcount stop at bit_count), so their value
//   (zero, from fresh pages) never enters a hash the caller takes over [base, used).
// Threading: one Bitset, one writer; no locking.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/vmem_arena.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/vmem_arena.h"

// _pad0 is explicit and zeroed at init because a Bitset is EMBEDDED in SlotMap (the `live`
// column), which is authoritative pool state: docs/CPP-SUBSET.md section 5 requires hashed
// state to use explicitly-padded structs with every pad named _pad0 and zeroed at construction.
// A bare { u64*; u32; } has four bytes of tail padding the compiler never writes, which is
// exactly the class of stale byte that rule exists to close. Added over docs/CONTAINERS.md
// section 8.5's shorthand struct by the W1 containers review, folded into the doc in the same
// commit.
struct Bitset {
    u64* words;
    u32 bit_count;
    u32 _pad0;
};
static_assert(sizeof(Bitset) == 16, "explicit padding, no implicit tail (docs/CPP-SUBSET.md section 5)");
static_assert(__is_trivially_copyable(Bitset), "");

// Words needed to hold bit_count bits. Pure, never fails.
inline u32 bitset_word_count(u32 bit_count) { return (bit_count + 63u) / 64u; }

// One arena_push of ceil(bit_count/64)*8 bytes (zeroed - fresh pages are OS-zero). bit_count may
// be 0 (words = null, every operation is a no-op / false).
inline void bitset_init(Bitset* b, VMemArena* arena, u32 bit_count) {
    b->bit_count = bit_count;
    b->_pad0 = 0;
    b->words = bit_count == 0 ? nullptr : (u64*)arena_push(arena, (u64)bitset_word_count(bit_count) * 8u, 8u);
}

// Sets bit i. TL_CHECK(i < bit_count) in all tiers. Legal inside a tick (a value write, not a
// growth).
inline void bitset_set(Bitset* b, u32 i) {
    TL_CHECK(i < b->bit_count);
    b->words[i / 64u] |= (u64(1) << (i % 64u));
}

// Clears bit i. TL_CHECK(i < bit_count) in all tiers. Legal inside a tick.
inline void bitset_clear(Bitset* b, u32 i) {
    TL_CHECK(i < b->bit_count);
    b->words[i / 64u] &= ~(u64(1) << (i % 64u));
}

// Reads bit i. TL_CHECK(i < bit_count) in all tiers. Legal inside a tick.
inline bool bitset_test(const Bitset* b, u32 i) {
    TL_CHECK(i < b->bit_count);
    return (b->words[i / 64u] & (u64(1) << (i % 64u))) != 0u;
}

// Clears every bit. Never fails; legal inside a tick.
inline void bitset_clear_all(Bitset* b) {
    for (u32 w = 0; w < bitset_word_count(b->bit_count); ++w) { b->words[w] = 0; }
}

// The smallest set bit index >= `from`, or bit_count if none. Bit order (0..bit_count) - a pure
// function of which bits are set.
inline u32 bitset_find_first(const Bitset* b, u32 from) {
    for (u32 i = from; i < b->bit_count; ++i) {
        if ((b->words[i / 64u] & (u64(1) << (i % 64u))) != 0u) { return i; }
    }
    return b->bit_count;
}

// SWAR popcount over one word - no compiler intrinsic (docs/CPP-SUBSET.md §4: an unresolved
// __builtin_popcountll on a target without native POPCNT calls a compiler-rt symbol outside the
// audit allowlist; this is portable and target-invariant by construction).
inline u32 bitset_popcount_word(u64 w) {
    w = w - ((w >> 1) & 0x5555555555555555ull);
    w = (w & 0x3333333333333333ull) + ((w >> 2) & 0x3333333333333333ull);
    w = (w + (w >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return (u32)((w * 0x0101010101010101ull) >> 56);
}

// Population count over [0, bit_count) only - the tail bits of the last word (if bit_count is not
// a multiple of 64) are never counted even if the underlying memory were nonzero.
inline u32 bitset_popcount(const Bitset* b) {
    u32 n = 0;
    u32 full_words = b->bit_count / 64u;
    for (u32 w = 0; w < full_words; ++w) {
        n += bitset_popcount_word(b->words[w]);
    }
    u32 rem = b->bit_count % 64u;
    if (rem != 0u) {
        u64 mask = (u64(1) << rem) - 1u;
        n += bitset_popcount_word(b->words[full_words] & mask);
    }
    return n;
}
