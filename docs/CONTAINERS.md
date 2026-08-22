# Containers and strings (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED.** Expands `PIVOT-DESIGN.md` §5.
> **Owns:** `src/foundation/array.h`, `slotmap.h`, `map.h`, `sorted.h`, `ring.h`, `bitset.h`,
> `sort.h`, `strview.h`, `interner.h`, `fmt.h`.

---

## 0. Policy

STL containers are triply disqualified (RAII contract vs no-destructors; implementation-defined
iteration/hash order and `std::hash`; compile-time weight). We build **only what has a consumer
today**, each one arena-backed, POD-only, with a deterministic walk order that is a documented
property. Nothing else until pulled — the SparseSet lesson. Every container's tests are part of
the determinism harness: property tests vs a naive reference model + two-instance
identical-order/identical-hash checks.

All containers `static_assert(is_trivially_copyable<T>)` — that is what makes memcpy-grow,
snapshot and hash *legal*.

---

## 1. `Array<T>` and `Span<T>` (DECIDED)

```cpp
template <typename T> struct Array { T* data; u32 count; u32 cap; VMemArena* arena; /* or null for fixed */ };
template <typename T> struct Span  { T* data; u32 count; };
```

- **Two backings:** (a) *own VMem range* — grows in place, stable base, never relocates (ECS
  columns, Alloy pools); (b) *arena-pushed fixed capacity* — `array_reserve(arena, cap)` once,
  `TL_FATAL` on overflow (scratch lists, command buffers). There is no relocating `realloc` growth
  in the runtime: a relocating grow invalidates every transient pointer in the pass and is the
  bug class stable bases exist to kill.
- API: `push`, `pop`, `swap_remove(i)`, `last`, `clear`, `span()`, `slice(a,b)`; `at(i)` is
  bounds-checked at `TL_CHECK` level (all tiers).
- Walk order: `0..count`, packed. `swap_remove` changes order deterministically (it's a pure
  function of the call sequence) — callers that need stable order use `SlotMap` or tombstones.

---

## 2. `SlotMap<T>` + `Handle` (DECIDED)

```cpp
template <typename T, typename H> struct SlotMap {
    Array<T> slots; Array<u16> gen; Array<u32> free_list; u32 live;  // gen parallel column; free list LIFO
};
H    slotmap_insert(SlotMap*, T value);        // reuses the most recently freed slot (LIFO) → deterministic
T*   slotmap_get(SlotMap*, H h);               // null if gen mismatch; TL_ASSERT in debug
bool slotmap_remove(SlotMap*, H h);            // bumps gen; gen wrap → slot quarantined (MEMORY.md §3)
```

- **Deterministic slot order:** the free list is LIFO and insertion is a pure function of the
  call sequence, which is itself deterministic (system order). **Iterate `0..slot_cap()`, skipping
  dead slots — never `0..live`.**
- Gen-wrap arithmetic per domain first (`MEMORY.md` §3). `gen` starts at 1; 0 is never issued.
- The slots array is hashed over `[0, slot_cap)` including dead slots (they are zeroed on remove so
  the hash is a pure function of state, not history). Stated in the header.

---

## 3. `Map<K,V>`, `SortedMap<K,V>`, `SortedSet<K>` (DECIDED)

**`Map<K,V>`** — open addressing, linear probing, power-of-two capacity, rapidhash with the pinned
seed, integer or `NameHash` keys only (no string keys — intern first). Tombstone-free (backward-
shift deletion). Walk order is deterministic per insertion sequence but **order-fragile across
refactors** → `Map` is for registries, editor, caches — anything whose order outlives the binary
uses a sorted walk. **Sim code keys on integers and never iterates a `Map`**
(`DETERMINISM.md` §2.7; LESSONS finding G).

**`SortedMap<K,V>` / `SortedSet<K>`** — a sorted `Array` + binary search. Order = pure function of
the key set. ~100 lines. Insert is O(n) memmove — fine at the sizes that need ordering (Luau-facing
ordered containers, save-file key order, registry dumps). **No B-tree until a bench proves the
memmove shows up.**

---

## 4. `RingBuffer<T>`, `Bitset`, sorting (DECIDED)

- `RingBuffer<T>`: fixed capacity (power of two), overwrite-oldest optional flag, `push`/`pop`/
  `peek(i)`; consumers: event queues' persistent mode, the netcode redundancy window, the log
  ring, Alloy's event stream.
- `Bitset`: `u64` words, fixed bit count at init; `set/clear/test/find_first/popcount`; walk
  order = bit order. Used for sleep flags, dirty chunks, action-map masks.
- **Sorting:** `sort_u32_kv(keys, vals, n, scratch)` — LSD radix, base 256, stable, integer keys
  only. `sort_u64_kv` likewise. A comparison sort exists only for tools (`tools/` may use
  `qsort`). There is no generic `sort<T, Cmp>` in the runtime: sim sorts are on integer keys by
  rule (`NETCODE.md` §14.1 heritage), and a stable integer radix is deterministic by construction.
  The radix needs a scratch buffer of `n` entries → comes from the scratch arena.

---

## 5. Strings (DECIDED — no `String` class)

The runtime has almost no strings by design. Three narrow tools:

| Tool | Shape | Use |
|---|---|---|
| `StrView { const char* ptr; u32 len; }` | non-owning, ~30 lines: `eq`, `hash`, `starts_with`, `split_at` | every string parameter |
| **Interner** | `intern(StrView) → StrId (u16)`; string arena + `Map<NameHash, StrId>` + reverse table | component/action/event/material names, Luau-facing identifiers; populated at init; **process-stable** for the run; the 64-bit `NameHash` is the cross-machine/persistence identity, the `u16` is never serialized |
| `fmt` | `fmt_buf(Span<char>, "...", ...)` over stb_sprintf (locale-free); no heap | logs, CSV, editor |

Owning copies are `arena_copy(StrView)`. **Firewall:** no strings or string-hash *order* in
authoritative sim state — interned ids and integers only inside the tick. The interner's reverse
table lives in a *non-registered* arena (it's debug/editor data).

`NameHash` = `constexpr` FNV-1a 64 (`"player"_id`); the debug side-table (hash → literal) is
filled by the interner at registration so the inspector can print names; a collision at
registration is fatal.

---

## 6. Alternatives recorded

| Alternative | Verdict |
|---|---|
| a general `Vector<T>` with relocating growth | rejected — invalidates in-pass pointers; stable bases or fixed caps only |
| per-entity bitset for queries | rejected (D6) — `cap × entities` overhead; iterate-smallest + probe |
| Swiss-table `Map` | rejected — complexity for no consumer; linear probing at power-of-two is enough for registries |
| B-tree ordered map | deferred — sorted array until a bench |
| `std::string`/any owning string class | rejected — the runtime doesn't need one; strings are ids |
| generic comparison sort in the runtime | rejected — integer radix only; `tools/` may use `qsort` |

---

## 7. Tests (the per-container rubric)

Fresh instance per test; happy path per public fn; error path per failure mode (overflow →
fatal-in-test harness hook; stale handle → null); edge matrix (0/1/many, empty/full, min/max,
null handle, wrap); **determinism:** two instances fed the same op sequence produce identical walk
order and identical hash; **property:** random op sequences vs a naive reference; **zero-alloc:**
arena-offset delta around every hot operation.

## 8. Implementation specification

### 8.1 `Array<T>` / `Span<T>` (`array.h`)

```cpp
template <typename T> struct Span  { T* data; u32 count; };
template <typename T> struct Array { T* data; u32 count; u32 cap; VMemArena* grow_arena; /* null = fixed */ };
template <typename T> void  array_init_vmem (Array<T>*, VMemArena* own_range);        // data = base; cap grows by arena_push of whole pages
template <typename T> void  array_init_fixed(Array<T>*, VMemArena* arena, u32 cap);    // one arena_push(cap*sizeof T); grow_arena = null
template <typename T> T*    array_push(Array<T>* a, T v);        // vmem: if count == cap → cap += page/sizeof(T) via arena_push; fixed: TL_FATAL on overflow
template <typename T> T     array_pop(Array<T>*);  template <typename T> void array_swap_remove(Array<T>*, u32 i);
template <typename T> T&    array_at(Array<T>*, u32 i);          // TL_CHECK(i < count) in all tiers
template <typename T> Span<T> array_span(Array<T>*), array_slice(Array<T>*, u32 a, u32 b);
template <typename T> void  array_clear(Array<T>*);              // count = 0; does NOT release; a hashed array's tail is re-zeroed by arena policy on reuse
```

`static_assert(__is_trivially_copyable(T))` in every function template. A vmem-backed array's
range is its own `VMemArena` (`MEMORY.md` §7 R-1); the array stores nothing but `base/count/cap`.

### 8.2 `SlotMap<T,H>` (`slotmap.h`)

```cpp
template <typename T, typename H> struct SlotMap {
    Array<T>   slots;      // vmem range; dead slots are zeroed (hash is a function of state)
    Array<u16> gen;        // parallel; starts at 1; 0 never issued
    Array<u32> free_list;  // LIFO; vmem range
    Bitset     live;       // one bit per slot
    u32        live_count; u32 quarantined;
};
H    slotmap_insert(SlotMap*, const T* v);  // pop free_list (LIFO) else slots.count++; gen stays; live.set; memcpy; return handle_make(idx, gen[idx])
T*   slotmap_get   (SlotMap*, H h);         // idx < slots.count && live.test(idx) && gen[idx] == handle_gen(h) ? &slots[idx] : null (TL_ASSERT in debug on stale)
bool slotmap_remove(SlotMap*, H h);         // get → memset(slot, 0) → live.clear → if gen == GEN_MAX { quarantined++ /* never pushed to free_list */ } else { gen++; free_list push }
// iteration: for (u32 i = 0; i < slots.count; ++i) if (live.test(i)) …   — never 0..live_count
```

### 8.3 `Map<K,V>` (`map.h`)

Open addressing, linear probing, power-of-two capacity, load ≤ 0.75, backward-shift deletion.
`K` is `u32`/`u64`/`NameHash` (static_assert integral); hash = `tl_hash64(&k, sizeof k, TL_HASH_SEED)`.
Storage: parallel `Array<K> keys`, `Array<V> vals`, `Array<u8> state` (0 empty, 1 full) on the
owning arena; grow by rehash into a fresh `arena_push` (registries and editor only — never in a
tick; `TL_ASSERT(!in_tick)` in debug). API: `map_init(arena, cap)`, `map_put`, `map_get → V*`,
`map_remove`, `map_count`, `map_iter(&it) → K,V*` (order-fragile by contract; `DETERMINISM.md`
§2.7).

### 8.4 `SortedMap<K,V>` / `SortedSet<K>` (`sorted.h`)

`Array<K> keys` + `Array<V> vals` kept sorted; `lower_bound` binary search; insert = memmove;
`sorted_iter` walks `0..count`. Integral `K` only.

### 8.5 `RingBuffer<T>`, `Bitset`, sorting (`ring.h`, `bitset.h`, `sort.h`)

```cpp
template <typename T> struct RingBuffer { T* data; u32 cap /* pow2 */; u32 head, tail; u8 overwrite_oldest; };
// push: if full → overwrite_oldest ? tail++ : return false; peek(i) from tail; pop from tail.
struct Bitset { u64* words; u32 bit_count; };   // set/clear/test/find_first(from)/popcount/clear_all; words on the owning arena
void sort_u32_kv(u32* keys, u32* vals, u32 n, Scratch* s);   // LSD radix base 256, 4 passes, stable
void sort_u64_kv(u64* keys, u32* vals, u32 n, Scratch* s);   // 8 passes; early-out on a pass whose histogram has one bucket
// pass: hist[256] over byte b of keys; prefix sum; scatter keys/vals into scratch copies; swap buffers. n ≤ 2^32-1.
```

### 8.6 Strings (`strview.h`, `interner.h`, `fmt.h`)

```cpp
struct StrView { const char* ptr; u32 len; };      // sv("lit") constexpr; sv_eq; sv_hash (FNV-1a 64, same as NameHash); sv_starts_with; sv_split_at
struct Interner { VMemArena* chars; Array<u32> offsets; Array<u16> lens; Map<NameHash, StrId> by_hash; u32 count; };
StrId   intern(Interner*, StrView s);      // by_hash get → return; else copy into chars, push offsets/lens, put; TL_FATAL on hash collision with a different string; count < 65535
StrView intern_name(const Interner*, StrId);   // reverse lookup (debug/editor)
NameHash intern_hash(const Interner*, StrId);
u32  fmt_buf(Span<char> out, const char* fmt, ...);   // stb_sprintf; returns length; truncates, never overflows
```

`NameHash operator""_id(const char*, usize)` is `constexpr` FNV-1a 64 (`CANON.md`). The debug
side-table (hash → literal) is the interner itself in dev tiers: every `"x"_id` that is also
registered by name (components, actions, events, arenas) goes through `intern` at registration.

### 8.7 Tests (`tests/foundation/containers/`)

`array.test.cpp` (vmem growth across page boundary keeps `data` stable; fixed overflow fatal-
expected; swap_remove order model), `slotmap.test.cpp` (LIFO reuse, stale handle null, gen wrap →
quarantine, zeroed dead slot → hash equals a fresh map with the same live set), `map.test.cpp`
(put/get/remove model vs a naive array; backward-shift correctness; two instances same op
sequence → identical iteration), `sorted.test.cpp`, `ring.test.cpp` (wrap, overwrite flag),
`bitset.test.cpp`, `sort.test.cpp` (stability with duplicate keys; 1M random keys vs a reference
insertion sort on a sample; all-equal keys early-out), `strview_interner_fmt.test.cpp` (intern
idempotence, collision fatal-expected with a crafted pair, `fmt_buf` truncation). Every file:
`TL_ASSERT_NO_ALLOC` around the hot op.

*Rev 1 — 2026-08-22.*
