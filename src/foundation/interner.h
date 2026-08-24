#pragma once
// ---------------------------------------------------------------------------------------------
// interner.h - Interner: StrView -> StrId (u16), process-stable for the run.
//
// Spec: docs/CONTAINERS.md §5 (design), §8.6 (this header); docs/CANON.md "Types" (`StrId` = u16
//   interned id, process-stable, never serialized).
// Purpose: component/action/event/material names, Luau-facing identifiers - populated at init.
//   The 64-bit NameHash is the cross-machine/persistence identity; StrId is a process-local
//   shorthand and is NEVER serialized (docs/CONTAINERS.md §5). This is the interner's job in dev
//   tiers for the debug hash->literal side-table too (docs/CONTAINERS.md §8.6) - not built here
//   until a consumer registers through it (foundation has no runtime table of its own, per
//   hash.h's own contract block).
// Invariants: count < 65535 (StrId's u16 range, one value reserved: 65535 is never issued so a
//   truncating cast never aliases a real id - TL_ASSERT enforces `count < 65535` before issuing).
//   A NameHash collision between two DISTINCT strings is TL_FATAL at registration (docs/CANON.md
//   "NameHash" - "a collision at registration is fatal"). `chars` is a caller-owned VMemArena the
//   interner treats as a raw byte bump-pool (arena_push per string, no alignment beyond 1 - text
//   has no alignment requirement); `offsets`/`lens`/`by_hash` are fixed-capacity from a second
//   caller-owned arena, sized by `max_strings` at init (a signature added over the rev-1 struct,
//   folded into CONTAINERS.md this commit - the doc gives the struct and `intern`/`intern_name`/
//   `intern_hash`, not construction).
// Determinism: never used for state (docs/MEMORY.md §4); the reverse table (chars/offsets/lens)
//   lives in a non-registered, non-hashed arena by the caller's choice - stated here, not
//   enforced, since this header cannot see the caller's ArenaRegistry.
// Threading: one Interner, one writer (init-time population); no locking.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/vmem_arena.h,
//   foundation/array.h, foundation/map.h, foundation/strview.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/vmem_arena.h"
#include "foundation/array.h"
#include "foundation/map.h"
#include "foundation/strview.h"

using StrId = u16;   // docs/CANON.md "Types" - interned id, process-stable, never serialized

struct Interner {
    VMemArena* chars;          // caller-owned; raw byte bump-pool, one arena_push per string
    Array<u32> offsets;        // offsets[id] = byte offset into chars->base
    Array<u16> lens;           // lens[id] = string length in bytes
    Map<NameHash, StrId> by_hash;
    u32 count;
};

// `chars_arena` backs the string bytes (grows unboundedly - a vmem-owned arena the caller
// initialized); `meta_arena` backs offsets/lens (fixed, max_strings entries) and by_hash (fixed,
// sized for max_strings at load <= 0.75). max_strings must be < 65535.
inline void interner_init(Interner* in, VMemArena* chars_arena, VMemArena* meta_arena, u32 max_strings) {
    TL_ASSERT(max_strings < 65535u);
    in->chars = chars_arena;
    array_init_fixed(&in->offsets, meta_arena, max_strings);
    array_init_fixed(&in->lens, meta_arena, max_strings);
    map_init(&in->by_hash, meta_arena, max_strings * 4u / 3u + 1u);
    in->count = 0;
}

// by_hash get -> return; else copy `s`'s bytes into chars, push offsets/lens, put. TL_FATAL on a
// NameHash collision with a DIFFERENT string (docs/CONTAINERS.md §8.6); returns the same StrId on
// a repeated intern of an identical string (idempotent).
inline StrId intern(Interner* in, StrView s) {
    // TL_CHECK, not TL_ASSERT: `lens[]` is u16, so an over-long string does not "fail an
    // assertion" in dev and behave in release - it silently truncates to (u16)s.len and
    // intern_name hands back a shorter string than was interned. Caller-input validation is
    // all-tier by docs/CPP-SUBSET.md §3 (W1 containers review 2).
    TL_CHECK(s.len <= 0xFFFFu);   // lens[] is u16 (docs/CONTAINERS.md section 8.6)
    NameHash h = sv_hash(s);
    StrId* existing = map_get(&in->by_hash, h);
    if (existing != nullptr) {
        StrView stored = StrView{ (const char*)in->chars->base + in->offsets.data[*existing], in->lens.data[*existing] };
        if (!sv_eq(stored, s)) { TL_FATAL("intern: NameHash collision between distinct strings"); }
        return *existing;
    }
    TL_ASSERT(in->count < 65535u);
    u64 offset = arena_mark(in->chars);
    void* dst = arena_push(in->chars, s.len, 1u);
    for (u32 i = 0; i < s.len; ++i) { ((char*)dst)[i] = s.ptr[i]; }
    array_push(&in->offsets, (u32)offset);
    array_push(&in->lens, (u16)s.len);
    StrId id = (StrId)in->count;
    map_put(&in->by_hash, h, id);
    in->count += 1u;
    return id;
}

// Reverse lookup (debug/editor). TL_CHECK(id < count) in all tiers.
inline StrView intern_name(const Interner* in, StrId id) {
    TL_CHECK(id < in->count);
    return StrView{ (const char*)in->chars->base + in->offsets.data[id], in->lens.data[id] };
}

// The NameHash of the interned string at id. TL_CHECK(id < count) in all tiers (via intern_name).
inline NameHash intern_hash(const Interner* in, StrId id) {
    return sv_hash(intern_name(in, id));
}
