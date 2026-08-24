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
- **Every operation that vacates a slot zeroes it** (`pop`, `swap_remove`, `clear`). A vmem-backed
  array's arena `used` covers its whole committed capacity, so `[count, cap)` sits inside the
  hashed extent; leaving a removed element's bytes there would make a hashed array's hash a
  function of removal history, which `CPP-SUBSET.md` §5 forbids and which `SlotMap` (§2) already
  avoids the same way. Rev 1 said the tail was "re-zeroed by arena policy on reuse" — that
  mechanism does not exist for an in-place refill (`ARENA_ZERO_ON_PUSH` only re-zeroes bytes an
  `arena_push` walks over, and a cleared array pushes nothing until `count` climbs back past
  `cap`). Corrected by the W1 containers review, 2026-08-24; cost is `O(count·sizeof T)` on
  `clear` and `O(sizeof T)` on the other two.
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
shift deletion): removing a key leaves the table byte-identical to one built from the survivors in
the same relative insertion order, so nothing about a *deleted* key survives in the walk order.
`map_init`/`map_grow` zero the blocks they push (never assume `arena_push` returns zeros — it does
so only above `high_water` or under `ARENA_ZERO_ON_PUSH`) and `map_remove` zeroes the slot it
empties. A **growing** `Map` orphans its old blocks below the arena's `used`, so its arena must not
be `ARENA_HASHED` (W1 containers review; `TODO.md` carries the ruling request). Walk order is deterministic per insertion sequence but **order-fragile across
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
  order = bit order. Used for sleep flags, dirty chunks, action-map masks, and `SlotMap`'s live
  column — which is authoritative state, so the struct carries an explicit zeroed `_pad0` per
  `CPP-SUBSET.md` §5 rather than four bytes of compiler tail padding (W1 containers review).
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
`sorted_iter` walks `0..count`. Integral `K` only. **Keys are unique — there is no tie:**
`sorted_map_put` on a present key overwrites its value and leaves `count` unchanged;
`sorted_set_insert` on a present key returns `false` and changes nothing. (Stated by the W1
containers review; rev 1 gave the operations but not the duplicate policy the tests must pin.)

### 8.5 `RingBuffer<T>`, `Bitset`, sorting (`ring.h`, `bitset.h`, `sort.h`)

```cpp
template <typename T> struct RingBuffer { T* data; u32 cap /* pow2 */; u32 head, tail; u8 overwrite_oldest, _pad0[3]; };
// head = next pop / peek(0); tail = next push. Both are FREE-RUNNING u32 counters, masked only at
// the point of indexing, so count = tail - head is a wrapping subtract that stays exact across the
// u32 boundary. push: if full → overwrite_oldest ? head++ (drop the oldest), else return false.
// (Rev 1 wrote the two roles the other way round — "peek/pop from tail, overwrite bumps tail";
// the behaviour it describes is the same, the field names were swapped. Corrected against the
// shipped header by the W1 containers review, 2026-08-24.)
struct Bitset { u64* words; u32 bit_count; u32 _pad0; };   // set/clear/test/find_first(from)/popcount/clear_all; words on the owning arena
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

### 8.6a Construction signatures added over rev-1 (W1 containers, 2026-08-24)

Rev-1 gave every container's struct and operations but not always its constructor — filed here per
`docs/ROADMAP.md` §0 rule 1 ("header first… signatures added over spec are folded into the doc,
same commit"):

- `bitset_init(Bitset*, VMemArena* arena, u32 bit_count)`, `ring_init(RingBuffer<T>*, VMemArena*
  arena, u32 cap, bool overwrite_oldest)`, `sorted_map_init`/`sorted_set_init(…, VMemArena* arena,
  u32 cap)` — one `arena_push` each, the same shape as `array_init_fixed`.
- `interner_init(Interner*, VMemArena* chars_arena, VMemArena* meta_arena, u32 max_strings)` —
  `chars` stays the caller-owned pointer §5 already specifies; `offsets`/`lens`/`by_hash` are fixed
  at `max_strings` (< 65535) from `meta_arena`. `StrId` (`= u16`) is declared in `interner.h`, not
  `tl_types.h` — `CANON.md`'s "Types" row names the alias, not its file.
- `slotmap_init(SlotMap<T,H>*, NameHash id_slots, NameHash id_gen, NameHash id_free, NameHash
  id_live, const VMemApi*)`. `SlotMap<T,H>` gains four `VMemArena` members
  (`_slots_arena/_gen_arena/_free_arena/_live_arena`) beyond §8.2's four columns: each column is
  its own VMem range per `MEMORY.md` §1.2 R-1, reserved to the handle domain's full capacity
  (`H::IDX_MASK + 1`) — cheap, since address-space reservation is free, and it lets `live`
  (fixed-size at init, §4) need no separate growth policy. Four ids are required, not derived, so
  the app's `ArenaRegistry` can register each column under its own name (all four are part of the
  pool's authoritative state). Each column's reserve is its exact byte size: `vmem_arena_init`
  rounds every reserve up to `COMMIT_GRANULE` (`MEMORY.md` §8.2), so a small-cap domain needs no
  caller-side floor — the one this lane shipped against the pre-ruling `vmem_arena_init` was
  deleted by the W1 containers review, guarded by
  `slotmap_small_cap_domain_needs_no_caller_reserve_floor`.

### 8.6b `fmt_buf` ships as a stub (W1 containers, 2026-08-24)

`fmt.h`/`fmt.cpp` carry the full contract (§8.6) but `fmt_buf` is `TL_FATAL("unimplemented")`:
`vendor/CMakeLists.txt` assigns `stb_sprintf`'s arrival to the W1 platform lane ("SDL3 + stb arrive
with the W1 platform lane"), which had not landed it as of this commit. Vendoring it from this lane
instead was rejected — it would duplicate a decision already owned elsewhere and risk a second
`vendor/stb_sprintf/` tree. Replace the stub the day the vendor tree lands (`TODO.md`).

### 8.7 Tests (`tests/foundation/`, flat — matching every sibling lane's layout, not the
`containers/` subdirectory this section originally named)

`array.test.cpp` (vmem growth across page boundary keeps `data` stable; fixed overflow fatal-
expected; swap_remove order model), `slotmap.test.cpp` (LIFO reuse, stale handle null, gen wrap →
quarantine, zeroed dead slot → hash equals a fresh map with the same live set), `map.test.cpp`
(put/get/remove model vs a naive array; backward-shift correctness; two instances same op
sequence → identical iteration), `sorted.test.cpp`, `ring.test.cpp` (wrap, overwrite flag),
`bitset.test.cpp`, `sort.test.cpp` (stability with duplicate keys; 1M random keys vs a reference
insertion sort on a sample; all-equal keys early-out), `strview_interner_fmt.test.cpp` (intern
idempotence, collision fatal-expected with a crafted pair, `fmt_buf` truncation deferred — see
§8.6b). Every file: a manual `arena_mark`-before/after check around one representative hot op per
container (the `TL_ASSERT_NO_ALLOC` macro does not compile yet — a `static_assert` stub pending
the runner lane's `alloc_shim.cpp` wiring, `TODO.md`).

*Rev 1 — 2026-08-22; §8.6a/§8.6b, §8.7 path correction added by the W1 containers lane, 2026-08-24;
§8.6a reserve-floor paragraph retired by the W1 containers review, 2026-08-24.*
