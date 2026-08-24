#pragma once
// ---------------------------------------------------------------------------------------------
// hash.h - the one pinned state hash and the one compile-time name hash.
//
// Spec: docs/DETERMINISM.md §4 (hashing rules), §9.1/§9.5 (this file); docs/CANON.md "Ticks,
//   hashes, fingerprints, RNG" and "Sizes and caps" (TL_HASH_SEED).
// Purpose: `tl_hash64` is the ONE hash used for state (per-arena hash, desync CRC, `Map`
//   buckets) - a thin wrap over vendored rapidhash so every caller in the tree agrees on the
//   algorithm and the seed. `NameHash`/`operator""_id` is the compile-time FNV-1a used for
//   component/action/event/arena names (`"player"_id`); it is a completely separate hash family
//   from tl_hash64 and is never used for state.
// Invariants: `TL_HASH_SEED` and the rapidhash configuration (RAPIDHASH_COMPACT, RAPIDHASH_FAST
//   - pinned in hash.cpp) are part of the lockstep contract: changing either bumps build_id
//   (docs/DETERMINISM.md §4). NameHash collisions are a registration-time fatal, but the
//   registration + the debug side-table (hash -> literal, for inspector display) are NOT built
//   here: they are the interner's job in dev tiers (docs/CONTAINERS.md §8.6) - foundation has no
//   runtime table to register into without static mutable state, which docs/CPP-SUBSET.md §1
//   bans engine-wide.
// Determinism: `tl_hash64` is deterministic and cross-ISA by construction (verified: clang
//   defines __SIZEOF_INT128__ on every target this project builds for, including clang-cl, so
//   rapidhash's 128-bit multiply takes the identical branch on PC/Deck/Pi - never the MSVC
//   intrinsic path, since we never build with real MSVC). `NameHash` is a pure function of the
//   literal's bytes; sim-TU literals must stay ASCII (docs/CPP-SUBSET.md §5 - a byte >= 0x80
//   read straight into a wider type sign-extends where `char` is signed and not where it is
//   unsigned; the u8 cast below closes that hole one level down).
// Threading: none - both are pure functions, no state.
// Includes: foundation/tl_types.h only. The vendored rapidhash header is included exclusively
//   from hash.cpp (docs/BUILD.md §4 "never included above its wrap module").
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

// The lockstep-contract seed for every tl_hash64 call in the tree ("tidelock1", docs/CANON.md).
constexpr u64 TL_HASH_SEED = 0x7469646c6f636b31ull;

// The one state hash: rapidhash (vendored, pinned config) seeded with `seed`. Used for per-arena
// state hashes, the desync CRC, and `Map` bucket selection (docs/DETERMINISM.md §4). `data` may
// be null only when `len == 0`. Pure function of the bytes; never allocates, never fails.
u64 tl_hash64(const void* data, usize len, u64 seed);

// The compile-time name id (docs/CANON.md "Types"): 64-bit FNV-1a over the byte string, offset
// 0xcbf29ce484222325, prime 0x100000001b3. A distinct family from tl_hash64 - never used for
// state, only for component/action/event/arena/table names.
using NameHash = u64;

constexpr u64 FNV1A64_OFFSET = 0xcbf29ce484222325ull;
constexpr u64 FNV1A64_PRIME  = 0x100000001b3ull;

// FNV-1a 64 over `len` bytes at `s`. Each byte goes through an explicit u8 cast before widening -
// `char` is signed on x86-64 and unsigned on aarch64, and widening a negative signed char
// straight to u64 sign-extends on one target and not the other with no `char` token in sight
// (docs/CPP-SUBSET.md §5); the u8 cast makes the byte value target-invariant before it widens.
constexpr NameHash fnv1a64(const char* s, usize len) {
    u64 h = FNV1A64_OFFSET;
    for (usize i = 0; i < len; ++i) {
        h ^= u64(u8(s[i]));
        h *= FNV1A64_PRIME;
    }
    return h;
}

// `"player_spawn"_id` -> NameHash, evaluated at compile time for a literal (docs/CPP-SUBSET.md
// §7b). Sim-TU literals must be ASCII (docs/CPP-SUBSET.md §5); non-ASCII bytes are caught by
// tools/audit/includes.py, not here.
constexpr NameHash operator""_id(const char* s, usize len) { return fnv1a64(s, len); }
