#pragma once
// ---------------------------------------------------------------------------------------------
// map.h - Map<K,V>: open addressing, linear probing, power-of-two capacity, backward-shift
//   deletion. Integer/NameHash keys only - no string keys (intern first).
//
// Spec: docs/CONTAINERS.md §3 (design), §8.3 (this header).
// Purpose: registries, editor tables, caches - anything whose order does NOT outlive the binary.
//   **Sim code keys on integers and never iterates a Map** (docs/DETERMINISM.md §2.7): walk order
//   is deterministic per insertion sequence but order-fragile across refactors. Ordered state that
//   must outlive the binary (saves, wire, Luau-visible) uses SortedMap/SortedSet instead.
// Invariants: load factor <= 0.75; tombstone-free (backward-shift deletion, docs/CONTAINERS.md
//   §8.3); hash = tl_hash64(&k, sizeof k, TL_HASH_SEED). Growth is a full rehash into a fresh
//   arena_push - registries and editor only, TL_ASSERT(!in_tick) in debug (never in a tick).
//   map_init/map_grow zero the three blocks they push (never assume arena_push returns zeros -
//   it does so only above high_water or under ARENA_ZERO_ON_PUSH), and map_remove zeroes the
//   slot it empties.
// Arena: a growing Map ORPHANS its old keys/vals/state blocks inside the arena, below `used`, so
//   the arena a Map may grow in must NOT be ARENA_HASHED - orphaned blocks would be hashed
//   forever and the hash would encode growth history. Derived from bump allocation + "hashes
//   cover [base, used)" (docs/MEMORY.md §1.2). RULED 2026-08-24 (TODO.md R2): fixed-shape on
//   hashed arenas, not "never" - any container on an ARENA_HASHED arena is SIZED AT INIT, and
//   Map carries Array's fixed mode to make that the only honest enforcement (a Map cannot see
//   its arena's registry flags, but a fixed-mode Map cannot grow anywhere). map_init_fixed
//   leaves `arena` null; the insert that would grow TL_FATALs. docs/MEMORY.md §1.2 and
//   docs/CONTAINERS.md §3 carry the ruling.
// Determinism: bucket order is a pure function of the insertion sequence and the pinned hash
//   (docs/DETERMINISM.md §4); this is exactly the "order-fragile" property the header above
//   warns about - two instances fed the same op sequence produce identical bucket layout.
// Threading: one Map, one writer; no locking.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/vmem_arena.h,
//   foundation/hash.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/vmem_arena.h"
#include "foundation/hash.h"
#include <string.h>   // memset - state must start empty and a vacated slot is zeroed

enum : u8 { MAP_SLOT_EMPTY = 0, MAP_SLOT_FULL = 1 };

template <typename K, typename V>
struct Map {
    K* keys;
    V* vals;
    u8* state;        // 0 empty, 1 full (tombstone-free: backward-shift deletion)
    u32 cap;           // power of two
    u32 count;
    VMemArena* arena;  // grow: full rehash into a fresh arena_push from this arena. null = fixed (Array<T>::grow_arena's shape).
};

// The doc's own instantiation constraint (docs/CPP-SUBSET.md §2): integral or NameHash keys only.
template <typename K> constexpr bool map_key_ok() {
    return __is_same(K, u32) || __is_same(K, u64) || __is_same(K, i32) || __is_same(K, i64) || __is_same(K, NameHash);
}

// The smallest power of two >= n (n >= 1). Pure, never fails.
inline u32 map_round_pow2(u32 n) {
    u32 p = 1;
    while (p < n) { p <<= 1; }
    return p;
}

// Pushes the three parallel arrays (keys/vals/state) from `arena` at capacity round_pow2(max(cap,
// 2)), and keeps `arena` as the GROW arena. Init only, never inside a tick. The arena must not be
// ARENA_HASHED (see the Arena block above) - use map_init_fixed there.
template <typename K, typename V>
void map_init(Map<K, V>* m, VMemArena* arena, u32 cap) {
    static_assert(map_key_ok<K>(), "Map<K,V>: K must be an integral or NameHash key (docs/CONTAINERS.md section 3)");
    static_assert(__is_trivially_copyable(V), "Map<K,V>: V must be trivially copyable (docs/CONTAINERS.md section 0)");
    u32 p = map_round_pow2(cap < 2u ? 2u : cap);
    m->keys = (K*)arena_push(arena, (u64)p * sizeof(K), alignof(K));
    m->vals = (V*)arena_push(arena, (u64)p * sizeof(V), alignof(V));
    m->state = (u8*)arena_push(arena, (u64)p, 1u);
    // Explicitly zeroed, never assumed zero. arena_push returns OS-zero pages only ABOVE the
    // arena's high_water; below it, bytes are zero only under ARENA_ZERO_ON_PUSH. A Map built
    // from a reused plain or scratch arena therefore starts with garbage `state`, every slot
    // reads MAP_SLOT_FULL, and map_probe spins forever looking for an empty slot - a hang, not a
    // wrong answer (W1 containers review 2).
    memset(m->state, MAP_SLOT_EMPTY, (usize)p);
    memset(m->keys, 0, (usize)p * sizeof(K));
    memset(m->vals, 0, (usize)p * sizeof(V));
    m->cap = p;
    m->count = 0;
    m->arena = arena;
}

// Fixed-shape init: the same three arena_pushes, but `arena` is NOT retained, so the Map can
// never grow and the insert that would grow TL_FATALs instead - exactly array_init_fixed's shape
// (docs/CONTAINERS.md §8.1). This is what a Map on an ARENA_HASHED arena uses: it cannot orphan a
// block below `used`, so the arena's hash stays a function of state rather than of growth history
// (RULED 2026-08-24, TODO.md R2; docs/MEMORY.md §1.2, docs/CONTAINERS.md §3). Capacity is
// round_pow2(max(cap, 2)) as always, and the usable load is 0.75 * that - size it accordingly.
template <typename K, typename V>
void map_init_fixed(Map<K, V>* m, VMemArena* arena, u32 cap) {
    map_init(m, arena, cap);
    m->arena = nullptr;
}

// The pinned state hash of one key's bytes (docs/DETERMINISM.md §4). Pure, never fails.
template <typename K> u64 map_hash(K k) { return tl_hash64(&k, sizeof(K), TL_HASH_SEED); }

// Internal: the probe-sequence slot index for `k`, positioned at either its live slot or the
// first empty slot on the probe path (linear probing, power-of-two capacity).
template <typename K, typename V>
u32 map_probe(const Map<K, V>* m, K k) {
    u32 mask = m->cap - 1u;
    u32 i = (u32)(map_hash<K>(k) & (u64)mask);
    for (;;) {
        if (m->state[i] == MAP_SLOT_EMPTY) { return i; }
        if (m->state[i] == MAP_SLOT_FULL && m->keys[i] == k) { return i; }
        i = (i + 1u) & mask;
    }
}

// Forward declaration: full rehash at double capacity - see the definition below. Never in a tick.
// TL_FATALs on a fixed-mode Map (map_init_fixed).
template <typename K, typename V> void map_grow(Map<K, V>* m);

// Inserts or overwrites k -> v. Grows (full rehash) first if load would exceed 0.75.
template <typename K, typename V>
void map_put(Map<K, V>* m, K k, V v) {
    if ((u64)(m->count + 1u) * 4u > (u64)m->cap * 3u) { map_grow(m); }
    u32 i = map_probe(m, k);
    if (m->state[i] == MAP_SLOT_EMPTY) {
        m->state[i] = MAP_SLOT_FULL;
        m->keys[i] = k;
        m->count += 1u;
    }
    m->vals[i] = v;
}

// Returns a pointer to the value for k, or null if absent. The pointer is valid until the next
// map_put/map_grow (a grow relocates every slot).
template <typename K, typename V>
V* map_get(Map<K, V>* m, K k) {
    u32 i = map_probe(m, k);
    return m->state[i] == MAP_SLOT_FULL ? &m->vals[i] : nullptr;
}

// Backward-shift deletion (tombstone-free, docs/CONTAINERS.md §8.3): removes k if present, then
// slides every subsequent entry on the same probe run back one slot until an empty slot or a key
// already at its own ideal position is found. Returns false if k was absent.
template <typename K, typename V>
bool map_remove(Map<K, V>* m, K k) {
    u32 mask = m->cap - 1u;
    u32 i = map_probe(m, k);
    if (m->state[i] != MAP_SLOT_FULL) { return false; }
    m->state[i] = MAP_SLOT_EMPTY;
    m->count -= 1u;
    u32 j = (i + 1u) & mask;
    for (;;) {
        if (m->state[j] == MAP_SLOT_EMPTY) { break; }
        u32 ideal = (u32)(map_hash<K>(m->keys[j]) & (u64)mask);
        // distance from ideal to the gap, vs distance from ideal to j, wrapping - move j back to
        // i only if the gap lies on j's own probe run (the standard backward-shift condition).
        u32 dist_to_gap = (i - ideal) & mask;
        u32 dist_to_j    = (j - ideal) & mask;
        if (dist_to_gap <= dist_to_j) {
            m->keys[i] = m->keys[j];
            m->vals[i] = m->vals[j];
            m->state[i] = MAP_SLOT_FULL;
            m->state[j] = MAP_SLOT_EMPTY;
            i = j;
        }
        j = (j + 1u) & mask;
    }
    // The final gap's key/value bytes are stale - the removed entry's, or the last entry the
    // shift moved out. Zero them so a Map's bytes are a function of its live contents and not of
    // its deletion history (docs/CPP-SUBSET.md §5; §2's dead-slot rule applied to this container).
    m->keys[i] = (K)0;
    memset(&m->vals[i], 0, sizeof(V));
    return true;
}

// The live entry count. Pure, never fails.
template <typename K, typename V>
u32 map_count(const Map<K, V>* m) { return m->count; }

// Order-fragile iterator (docs/DETERMINISM.md §2.7): walks bucket order 0..cap, skipping empty
// slots. `it` is a plain slot index in/out parameter - pass 0 to start, the returned index + 1 to
// continue. Returns false (K*, V* left untouched) once no more full slots remain.
template <typename K, typename V>
bool map_iter(const Map<K, V>* m, u32* it, K* out_k, V* out_v) {
    while (*it < m->cap) {
        u32 i = *it;
        *it += 1u;
        if (m->state[i] == MAP_SLOT_FULL) {
            *out_k = m->keys[i];
            *out_v = m->vals[i];
            return true;
        }
    }
    return false;
}

// Full rehash into a fresh arena_push at double capacity (docs/CONTAINERS.md §8.3 - "grow by
// rehash into a fresh arena_push", never in a tick). The old block is orphaned in the arena (bump
// allocation, no free()) - the same tradeoff registries and editor tables already accept.
// A fixed-mode Map (null arena, map_init_fixed) TL_FATALs here instead, in EVERY tier - the guard
// sits at the single growth choke point rather than in map_put, so a direct map_grow call is
// covered too (RULED 2026-08-24, TODO.md R2).
template <typename K, typename V>
void map_grow(Map<K, V>* m) {
    if (m->arena == nullptr) { TL_FATAL("map_grow: fixed map overflow"); }
    u32 old_cap = m->cap;
    K* old_keys = m->keys;
    V* old_vals = m->vals;
    u8* old_state = m->state;
    u32 new_cap = old_cap * 2u;
    m->keys = (K*)arena_push(m->arena, (u64)new_cap * sizeof(K), alignof(K));
    m->vals = (V*)arena_push(m->arena, (u64)new_cap * sizeof(V), alignof(V));
    m->state = (u8*)arena_push(m->arena, (u64)new_cap, 1u);
    memset(m->state, MAP_SLOT_EMPTY, (usize)new_cap);   // same reason as map_init
    memset(m->keys, 0, (usize)new_cap * sizeof(K));
    memset(m->vals, 0, (usize)new_cap * sizeof(V));
    m->cap = new_cap;
    m->count = 0;
    for (u32 i = 0; i < old_cap; ++i) {
        if (old_state[i] == MAP_SLOT_FULL) { map_put(m, old_keys[i], old_vals[i]); }
    }
}
