#pragma once
// ---------------------------------------------------------------------------------------------
// sorted.h - SortedMap<K,V> / SortedSet<K>: a sorted Array<K> (+ Array<V> for the map) kept in
//   order by binary-search insert; order is a pure function of the key set.
//
// Spec: docs/CONTAINERS.md §3 (design), §8.4 (this header).
// Purpose: ordered state that outlives the binary - Luau-facing ordered containers, save-file key
//   order, registry dumps (docs/DETERMINISM.md §2.7 names this as Map's order-fragile escape
//   hatch). Insert is O(n) memmove - fine at the sizes that need ordering; no B-tree until a bench
//   proves the memmove shows up (docs/CONTAINERS.md §6).
// Invariants: integral K only (docs/CONTAINERS.md §8.4). Fixed capacity, arena-pushed once
//   (sorted_init is a signature added over the rev-1 spec, folded into CONTAINERS.md this commit -
//   the doc specifies the kept-sorted Array pair and lower_bound/insert, not construction).
// Determinism: walk order is 0..count over the sorted key array - a pure function of the key set,
//   never of insertion order (unlike Map).
// Threading: one container, one writer; no locking.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/vmem_arena.h,
//   foundation/array.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/vmem_arena.h"
#include "foundation/array.h"
#include <string.h>   // memmove only (docs/CPP-SUBSET.md §1 - sanctioned for the overlapping erase/insert move)

// The doc's own instantiation constraint (docs/CPP-SUBSET.md §2): integral or NameHash keys only.
template <typename K> constexpr bool sorted_key_ok() {
    return __is_same(K, u32) || __is_same(K, u64) || __is_same(K, i32) || __is_same(K, i64) || __is_same(K, NameHash);
}

// The first index i in [0, count) with keys[i] >= k, or count if none (standard binary lower
// bound; the keys are kept sorted ascending as an invariant of insert/remove).
template <typename K>
u32 sorted_lower_bound(const K* keys, u32 count, K k) {
    u32 lo = 0, hi = count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (keys[mid] < k) { lo = mid + 1u; } else { hi = mid; }
    }
    return lo;
}

// --- SortedMap<K,V> ----------------------------------------------------------------------------

template <typename K, typename V>
struct SortedMap {
    Array<K> keys;
    Array<V> vals;
};

// Pushes the two fixed-capacity arrays (keys/vals) from `arena`. Init only, never inside a tick.
template <typename K, typename V>
void sorted_map_init(SortedMap<K, V>* m, VMemArena* arena, u32 cap) {
    static_assert(sorted_key_ok<K>(), "SortedMap<K,V>: K must be integral/NameHash (docs/CONTAINERS.md section 3)");
    static_assert(__is_trivially_copyable(V), "SortedMap<K,V>: V must be trivially copyable (docs/CONTAINERS.md section 0)");
    array_init_fixed(&m->keys, arena, cap);
    array_init_fixed(&m->vals, arena, cap);
}

// Inserts or overwrites k -> v, keeping keys ascending (memmove insert). TL_FATAL on overflow
// (fixed capacity, docs/CONTAINERS.md §1).
template <typename K, typename V>
void sorted_map_put(SortedMap<K, V>* m, K k, V v) {
    u32 i = sorted_lower_bound<K>(m->keys.data, m->keys.count, k);
    if (i < m->keys.count && m->keys.data[i] == k) {
        m->vals.data[i] = v;
        return;
    }
    if (m->keys.count == m->keys.cap) { TL_FATAL("sorted_map_put: fixed capacity overflow"); }
    u32 n = m->keys.count;
    if (i < n) {
        memmove(m->keys.data + i + 1u, m->keys.data + i, (usize)(n - i) * sizeof(K));
        memmove(m->vals.data + i + 1u, m->vals.data + i, (usize)(n - i) * sizeof(V));
    }
    m->keys.data[i] = k;
    m->vals.data[i] = v;
    m->keys.count += 1u;
    m->vals.count += 1u;
}

// Returns a pointer to the value for k, or null if absent. Valid until the next put/remove (a
// memmove can relocate it).
template <typename K, typename V>
V* sorted_map_get(SortedMap<K, V>* m, K k) {
    u32 i = sorted_lower_bound<K>(m->keys.data, m->keys.count, k);
    return (i < m->keys.count && m->keys.data[i] == k) ? &m->vals.data[i] : nullptr;
}

// Removes k if present (memmove close the gap). Returns false if absent.
template <typename K, typename V>
bool sorted_map_remove(SortedMap<K, V>* m, K k) {
    u32 i = sorted_lower_bound<K>(m->keys.data, m->keys.count, k);
    if (i >= m->keys.count || m->keys.data[i] != k) { return false; }
    u32 n = m->keys.count;
    if (i + 1u < n) {
        memmove(m->keys.data + i, m->keys.data + i + 1u, (usize)(n - i - 1u) * sizeof(K));
        memmove(m->vals.data + i, m->vals.data + i + 1u, (usize)(n - i - 1u) * sizeof(V));
    }
    m->keys.count -= 1u;
    m->vals.count -= 1u;
    return true;
}

// Walks the sorted key array 0..count - a pure function of the key set.
template <typename K, typename V>
void sorted_map_iter(const SortedMap<K, V>* m, u32* it, K* out_k, V* out_v) {
    TL_ASSERT(*it < m->keys.count);
    *out_k = m->keys.data[*it];
    *out_v = m->vals.data[*it];
    *it += 1u;
}

// --- SortedSet<K> --------------------------------------------------------------------------

template <typename K>
struct SortedSet {
    Array<K> keys;
};

// Pushes the fixed-capacity key array from `arena`. Init only, never inside a tick.
template <typename K>
void sorted_set_init(SortedSet<K>* s, VMemArena* arena, u32 cap) {
    static_assert(sorted_key_ok<K>(), "SortedSet<K>: K must be integral/NameHash (docs/CONTAINERS.md section 3)");
    array_init_fixed(&s->keys, arena, cap);
}

// Inserts k if absent, keeping keys ascending. Returns true if inserted, false if k was already
// present. TL_FATAL on overflow (fixed capacity).
template <typename K>
bool sorted_set_insert(SortedSet<K>* s, K k) {
    u32 i = sorted_lower_bound<K>(s->keys.data, s->keys.count, k);
    if (i < s->keys.count && s->keys.data[i] == k) { return false; }
    if (s->keys.count == s->keys.cap) { TL_FATAL("sorted_set_insert: fixed capacity overflow"); }
    u32 n = s->keys.count;
    if (i < n) {
        memmove(s->keys.data + i + 1u, s->keys.data + i, (usize)(n - i) * sizeof(K));
    }
    s->keys.data[i] = k;
    s->keys.count += 1u;
    return true;
}

// True iff k is present. Pure, never fails.
template <typename K>
bool sorted_set_contains(const SortedSet<K>* s, K k) {
    u32 i = sorted_lower_bound<K>(s->keys.data, s->keys.count, k);
    return i < s->keys.count && s->keys.data[i] == k;
}

// Removes k if present (memmove close the gap). Returns false if absent.
template <typename K>
bool sorted_set_remove(SortedSet<K>* s, K k) {
    u32 i = sorted_lower_bound<K>(s->keys.data, s->keys.count, k);
    if (i >= s->keys.count || s->keys.data[i] != k) { return false; }
    u32 n = s->keys.count;
    if (i + 1u < n) {
        memmove(s->keys.data + i, s->keys.data + i + 1u, (usize)(n - i - 1u) * sizeof(K));
    }
    s->keys.count -= 1u;
    return true;
}
