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

*Rev 1 — 2026-08-22.*
