# Memory — arenas, the registered set, handles (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §7. Expands `PIVOT-DESIGN.md` §4.
> **Lineage:** D3 / ALLOY §9.1 / §13 / D16-M1 from foundry, ported nearly verbatim.
> **Owns:** `src/foundation/vmem_arena.h`, `arena_registry.h`, `scratch.h`, `handle.h`, `mem_pool.h`.

---

## 0. The rules everything else follows from

1. **Four arena types, no general `free()`.** Wanting a general freeing allocator in engine/sim
   code is a design smell, not a missing feature.
2. **No pointers in authoritative state — handles/indices only.** Partially machine-enforced: the
   reflection walk asserts no pointer-typed members in registered components (`ECS.md` §6); hand-
   rolled pools by review. Transient raw pointers *within a pass* are safe because bases are stable.
3. **Ticks allocate nothing** outside scratch; registered arenas grow only inside barrier windows.
4. **The registered arena set is simultaneously** the snapshot unit, the per-tick hash unit, and the
   rollback ring's payload. One registry, three consumers.

---

## 1. The four arena types (DECIDED)

### 1.1 `VMemArena` — reserve address space, commit on demand

```cpp
struct VMemArena { u8* base; u64 reserved; u64 committed; u64 used; u32 page; u32 flags; };
ErrCode vmem_arena_init(VMemArena*, u64 reserve_bytes, const VMemApi* os);  // VirtualAlloc/mmap via PLATFORM seam
void*   arena_push(VMemArena*, u64 bytes, u32 align);   // bump; commits pages as needed; zeroed (OS pages are zero)
u64     arena_mark(const VMemArena*);                   // = used
void    arena_reset_to(VMemArena*, u64 mark);           // debug: poison 0xDD above mark
void    arena_decommit_above(VMemArena*, u64 mark);     // returns pages to the OS (streaming, ALLOY §13)
```

- **Stable bases forever** → columns grow without relocation → raw pointers are valid within a
  pass. ~150 lines per platform, behind `PLATFORM.md` §4's `VMemApi` fn-ptrs (foundation never
  includes an OS header).
- **Zeroed pages from the OS** solve padding determinism for fresh memory: a freshly pushed struct
  has zero padding without a memset, and the hash of a never-written tail is the hash of zeros.
  Memory re-used after `arena_reset_to` is **not** zero (it's poisoned in debug) — pools that
  reuse must zero explicitly. This asymmetry is stated in every pool header.
- **Commit granularity is explicit** (page multiples, `page` recorded) so Alloy's per-chunk
  commit/decommit terrain arena is an extension, not a rewrite.
- **Reserve sizes are generous and fixed at init** from a table in `app/` (per arena, per platform
  tier); exceeding `reserved` is `TL_FATAL` — a blown budget is a bug, not silent growth. A
  low-RAM peer's table is smaller. T-A-03 replaces guesses with measurements.

### 1.2 Permanent arenas + the registered arena set

Every long-lived authoritative pool lives in an arena registered at init:

```cpp
struct ArenaEntry { NameHash id; VMemArena* arena; u32 flags; /* HASHED | SNAPSHOT | GROWS_AT_BARRIER */ };
struct ArenaRegistry { ArenaEntry e[MAX_ARENAS /*4096*/]; u32 count; };
void  registry_add(ArenaRegistry*, NameHash id, VMemArena*, u32 flags);   // init only; order = registration order
u64   registry_hash_all(const ArenaRegistry*, u64 hashes_out[]);           // per-arena rapidhash over [base, used); world hash over those
void  registry_snapshot(const ArenaRegistry*, Snapshot*);                  // memcpy each [base, used) + used table + fingerprint
ErrCode registry_restore(ArenaRegistry*, const Snapshot*);                 // fail-loud on fingerprint/layout mismatch
```

- **Registration order is part of the lockstep contract** (the world hash folds in that order;
  the snapshot layout is that order). It is explicit in the `app/` wiring file.
- **`HASHED` implies `SNAPSHOT`; `registry_add` `TL_FATAL`s on the combination without it**
  (ruled 2026-08-24). A hashed arena that is not snapshotted cannot be rolled back, so a mid-run
  restore cannot reproduce the hash trace (§8.8) — a desync trap wired at registration and found
  weeks later. Immutable compiled tables (§5) take `SNAPSHOT` anyway: they are small, and
  restoring bytes that never mutated is a no-op-equivalent `memcpy`. `SNAPSHOT` without `HASHED`
  stays legal (state that is restored but deliberately outside the hash), as does membership with
  neither flag (`GROWS_AT_BARRIER` alone — the guard's business, §2).
- **Every container on an `ARENA_HASHED` arena is SIZED AT INIT** (ruled 2026-08-24). A container
  that grows by bump-allocating a new block orphans its old one below `used`, where the arena's
  hash covers it forever — so the hash encodes allocation history, not state. This does *not*
  desync a session (lockstep peers run identical op histories, so their orphans are identical, and
  checkpoints are raw arena images — `DETERMINISM.md` §5 — so a joiner inherits the exact bytes);
  what it costs is hygiene: unbounded hashed garbage, and a hash that moves for a reason no state
  change explains. `Array<T>` already had the fixed mode (`CONTAINERS.md` §8.1); `Map<K,V>` gained
  `map_init_fixed` to match (`CONTAINERS.md` §3), and that *is* the enforcement — a container
  cannot see its own arena's registry flags, but a fixed-mode container cannot grow anywhere, so
  sizing at init is checked where the growth would happen rather than where the flag lives.
- **Snapshot ring (T-F-04):** `N = CONFIRMATION_HORIZON_TICKS` slots allocated **once** from a
  dedicated arena at init; each slot sized at `Σ reserved` of snapshotted arenas at the *budget*,
  with the used extents recorded per slot so a restore copies only `[base, used)`. Cannot be sized
  for real until T-A-03.
- Built day one (~50 lines). The ECS columns, Alloy pools, the tick counter and RNG seed, and the
  compiled data tables all live in registered arenas. **Net buffers, render state, Luau heaps, and
  scratch are never registered.**

### 1.3 Frame/scratch arenas — one per worker thread

`Scratch { VMemArena a; }` with push/pop markers as the everyday API; `reset()` at frame end
(the main thread's) or at the barrier (workers'). Command buffers, broadphase transients,
neighbor lists, coarse-sampling grids, the render packet and the draw command list live here.
(Event queues do **not** — their read half must outlive the frame; they have a two-half arena of
their own, `ECS.md` §10.4.)
Debug: poison `0xDD` on reset. Per-worker scratch is for *allocation locality*; nothing keyed by
worker identity may be read back into results (`JOBS.md` §1).

### 1.4 Slot reuse lives inside `SlotMap` and nowhere else

`SlotMap<T>` (`CONTAINERS.md` §2) is the only place a slot is freed and reused; it does so with a
generation bump so stale handles fail loudly (debug) or read as absent (release).

### 1.5 The one exception: vendor heaps (DECIDED — alternatives recorded)

Luau, ImGui, SDL, ENet, and stb need `realloc`/`free` semantics. None of their heaps is
authoritative; none is hashed or snapshotted. Options weighed:

| Option | Determinism | Performance | LOC / cognitive | Correctness surface |
|---|---|---|---|---|
| CRT `malloc` for vendor libs | safe (non-authoritative) | unbounded fragmentation; no budget; OS-dependent timing | zero | invisible to the alloc guard; Luau GC spikes unmeasured |
| **size-class freelist pool over a `VMemArena` reserve, per vendor** (chosen) | safe | bounded, budgeted, locality; Luau's VM heap is one reserve | ~200 lines | the *only* general allocator in the binary; grep-enforced off-limits to `src/` code |
| region-reset per script reload | safe | zero fragmentation | small | wrong for ImGui/SDL lifetimes; Luau GC still needs `free` |

`mem_pool.h` is a power-of-two size-class freelist (≤ 64 KB classes; larger → direct page
commit) with a per-pool budget and a counter the profiler reads. **Engine and sim code never call
it** (CI grep: `pool_alloc` appears only in `vendor_glue/`). Luau gets one pool per VM; ImGui and
SDL share one; ENet its own (net buffers are sized by the protocol anyway).

---

## 2. The guards (DECIDED)

- **Arena-offset guard:** at tick start record `used` of every registered arena; at tick end
  assert only scratch moved — *except* inside the barrier-apply window, where arenas flagged
  `GROWS_AT_BARRIER` (ECS columns, Alloy pools during pass 5) may grow. A growth outside the window
  is `TL_FATAL` in debug. This is the Layr zero-alloc guard, made structural.
- **Global allocator shim:** in `dev` and `netcode` tiers, `operator new`/`malloc` from `src/`
  code is a link error (the symbol audit) for sim libs and a `TL_FATAL` counting shim for the rest;
  vendor libs are routed through `mem_pool` via their hook APIs (`lua_newstate(alloc_fn)`,
  `ImGui::SetAllocatorFunctions`, `SDL_SetMemoryFunctions`, `enet_initialize_with_callbacks`,
  `STBI_MALLOC`). A nonzero per-frame CRT-malloc count in steady state is a test failure.
- **Hash-region integrity test per pool** (`DETERMINISM.md` §4).

---

## 3. Handles (DECIDED)

```cpp
template <typename Tag, int IDX_BITS, int GEN_BITS> struct Handle { uint_fit<IDX_BITS+GEN_BITS> bits; };
// null = bits == 0; generation 0 is never issued → zero-init memory is never a valid handle
```

| Domain | Width | Cap / reuses | Notes |
|---|---|---|---|
| `Entity` | `Handle<EntityTag, 22, 10>` → u32 | 4M / 1024 | the ECS id; survives snapshot/restore (bits are state) |
| textures, audio clips, fonts, data tables | `Handle<_, 12, 4>` → u16 | 4K / 16 | resources: few, low churn; 2 MB saved per million sprites vs u32 |
| Alloy bodies, constraints, plants, cavities, basins | `Handle<_, 22, 10>` or `<16,16>` per pool | per `ALLOY.md` §1.1 | cross-tick identity |
| Alloy particles | **plain `u32` index**, tick-scoped validity | — | high churn; pass-5 compaction emits a remap; persistent refs target handle citizens only |
| layers, cameras, views | `u8` dense index | tens | not generational |

- **Generation-wrap arithmetic is done per domain before widths are frozen** (gate finding B): a
  domain that can churn through 2^GEN reuses of one slot within a session must either widen GEN or
  quarantine the slot (never reissue after wrap). Entities: 1024 reuses of one slot in a session is
  plausible under bullet-hell churn → **quarantine on wrap** (the slot retires until world reset).
- Handles are the *only* cross-pool reference type. `get(handle) → T*` (nullable, tick-scoped
  pointer) on every `SlotMap`; stale → null + `TL_ASSERT` in debug.

---

## 4. Strings and owning copies

No `String` class. `arena_copy(StrView)` into the permanent or scratch arena is the owning copy;
the interner owns long-lived names (`CONTAINERS.md` §5).

---

## 5. What snapshot/restore covers, precisely

| Restored by `registry_restore` | Not restored (must be derivable or non-authoritative) |
|---|---|
| ECS columns + sparse pages + entity slot table | spatial index caches, broadphase, neighbor lists |
| Alloy pools, SDF stores, graphs, basins, constraint lists | Luau heaps (scripts rebuild their transient tables from world state — `LUAU-LAYER.md` §2) |
| tick counter, RNG seed, compiled data tables (restored but fingerprint-checked — they are hashed, so §1.2's `HASHED` ⇒ `SNAPSHOT` rule puts them here even though they never mutate) | render double-buffers (prev/current are re-primed with a snap bit after restore) |
| singleton components (system state) | net/log rings, ImGui, audio |

A restore is followed by a `post_restore` barrier where derived caches are marked dirty and the
render interpolation is snapped. That is the only hook; nothing else may observe a restore.

---

## 6. Budget sketch (initial reserves; T-A-03 replaces)

| Arena | Reserve | Notes |
|---|---|---|
| ECS columns (one VMem range per column) | 64 MB total address space | pages commit as entities appear |
| Alloy pools | 256 MB | the unknown; sets snapshot/ring/rejoin sizes |
| data tables | 8 MB | compiled Luau tables |
| snapshot ring | N × Σ used (budget) | allocated once; N from NETCODE App. B |
| scratch (per worker) | 64 MB each | reset per tick/barrier |
| Luau sim VM pool / UI VM pool | 64 MB / 64 MB | budgeted; GC step budget per tick (`LUAU-LAYER.md` §5) |

---

## 7. Rulings (closed 2026-08-22 — nothing open)

- **R-1 One VMem range per ECS column** (and per Alloy pool column). Stable base per column,
  growth without a cap, 64-bit address space is free; the reserve count (a few hundred) is far
  below any OS mapping limit. A shared sliced range would impose a fixed entity cap for nothing.
- **R-2 Ring overflow is a budget violation.** `netcode`/`ship` tiers: `TL_FATAL` with the arena
  named (lockstep peers must agree on limits, so a budget is part of the contract and is
  fingerprinted via the reserve table). `dev` tier: the snapshot call returns
  `ERR_MEM_RING_OVERFLOW` and the **caller** (the loop) logs the warning once and grows the ring
  at the next barrier, so a tuning session is not killed by a guess in the reserve table; the
  warning is the signal to raise the budget in `app/`. *(Mechanism split recorded by the W1 mem
  lane, 2026-08-24: the registry/snapshot TUs are in the audited det half, whose symbol
  allowlist is closed to io — `CPP-SUBSET.md` §4 — so `TL_LOG_WARN` cannot be called from
  there; the error code is the det-legal spelling of the same ruling.)*

## 8. Implementation specification

### 8.1 Files (`src/foundation/`)

| File | Contents |
|---|---|
| `vmem_api.h` | `struct VMemApi`, transcribed verbatim from `PLATFORM.md` §9.2 (its owner): foundation is a leaf (`ARCHITECTURE.md` §1) and cannot include platform.h, so the table's definition lives here and platform.h includes it — the `foundation/atomic.h` precedent (ruling request with the platform lane, `TODO.md`) |
| `vmem_arena.h/.cpp` | `VMemArena`, push/mark/reset/decommit; takes a `const VMemApi*` at init; the mem `ErrCode` range `0x0101..0x0105` |
| `arena_registry.h/.cpp` | `ArenaRegistry`, `registry_add/seal/set_fingerprint/hash_all/snapshot/restore`; `ArenaGuard` declarations |
| `arena_guard.cpp` | the guard implementations — in the NON-det half: `guard_tick_end` reads the CRT counter from `alloc_shim`, an upward symbol the det audit forbids; the guard is dev-only engine-side tooling, never sim-called |
| `snapshot.h/.cpp` | `Snapshot`, `SnapshotRing`, `ring_init/push/find` |
| `scratch.h` | `Scratch` (a `VMemArena` + a `SCRATCH_MAX_SCOPES = 16` marker stack), `scratch_init/push/scope_begin/scope_end/reset`, `TL_SCRATCH_SCOPE_BEGIN/_END` (the explicit pair of `CPP-SUBSET.md` §7b) |
| `handle.h` | `Handle<Tag,IDX,GEN>`, `handle_make/index/gen/is_null` |
| `mem_pool.h/.cpp` | the vendor-heap pool (§1.5); the per-lib adaptor functions live in `vendor_glue/` (the W2 vendor lane) |
| `alloc_shim.h/.cpp` | `dev`/`netcode` tiers: global `operator new/delete` → `TL_FATAL`; `malloc` hook counters via the CRT debug hook (Windows) / `__malloc_hook`-free approach: a link-time wrapper (`-Wl,--wrap=malloc` on Linux, detours in the CRT debug heap on Windows) that counts calls per frame. The header declares `tl_alloc_shim_install()` (returns `ERR_MEM_UNSUPPORTED` where counting cannot work) and the cumulative `tl_crt_alloc_count()` the guard reads |

### 8.2 `VMemArena`

```cpp
struct VMemArena {
    u8*  base;        // reserved range start, page aligned
    u64  reserved;    // bytes reserved, a COMMIT_GRANULE multiple (init rounds up)
    u64  committed;   // bytes committed, multiple of COMMIT_GRANULE
    u64  used;        // bump pointer
    u64  high_water;  // max(used) ever — memory in [used, high_water) is dirty; [high_water, committed) is OS-zero
    u32  page;        // OS page size (queried)
    u32  flags;       // ARENA_POISON (debug), ARENA_ZERO_ON_PUSH
    NameHash id;
    const VMemApi* os;
};
enum { COMMIT_GRANULE = 64 * 1024 };

ErrCode vmem_arena_init(VMemArena* a, NameHash id, u64 reserve_bytes, u32 flags, const VMemApi* os);
// reserve_bytes rounded up to COMMIT_GRANULE; os->reserve; committed = used = high_water = 0.
// The granule, not the page (ruled 2026-08-24): the commit path below fatals when align_up(end, COMMIT_GRANULE) > reserved, so a page-rounded reserve made the usable budget round_down(reserved, COMMIT_GRANULE) — a sub-64 KB reserve could never push a byte and a non-multiple reserve had an unreachable tail. Address space is free; the stated budget is the usable budget.
void* arena_push(VMemArena* a, u64 bytes, u32 align) {
    u64 start = align_up(a->used, align);           // align is a power of two, ≤ page
    u64 end   = start + bytes;
    if (end > a->committed) {                        // commit in COMMIT_GRANULE multiples
        u64 want = align_up(end, COMMIT_GRANULE);
        if (want > a->reserved) TL_FATAL("arena %s over reserve", name);
        os->commit(a->base + a->committed, want - a->committed);   // fresh pages are zero (PLATFORM.md §4)
        a->committed = want;
    }
    u8* p = a->base + start;
    if ((a->flags & ARENA_ZERO_ON_PUSH) && used < high_water)      // reused region: not zero
        memset(base + used, 0, min(end, high_water) - used);       // from OLD used, not from start: the ALIGNMENT GAP [used, start) enters [base, used) and is hashed, so it must be re-zeroed with the block (W1 mem, 2026-08-24)
    a->used = end; if (end > a->high_water) a->high_water = end;
    return p;
}
u64  arena_mark(const VMemArena* a) { return a->used; }
void arena_reset_to(VMemArena* a, u64 mark) { TL_ASSERT(mark <= a->used); #if TL_DEBUG if (flags & ARENA_POISON) memset(base+mark, 0xDD, used-mark); #endif a->used = mark; }   // poison is gated on the flag: unconditional poison synchronised every arena's dev-tier dirt to 0xDD, defeating §8.8's divergent-dirt criterion (W1 mem review 3)
void arena_decommit_above(VMemArena* a, u64 mark) { u64 p = align_up(mark, COMMIT_GRANULE); if (p < committed) { os->decommit(base+p, committed-p); committed = p; high_water = min(high_water, p); } used = min(used, mark); }
```

Registered arenas use `ARENA_ZERO_ON_PUSH` (hashed memory must be a pure function of state, so a
reused byte range is re-zeroed before use); scratch arenas do not (speed; debug poison instead).

### 8.3 `ArenaRegistry`, hashing, snapshots

```cpp
struct ArenaEntry { NameHash id; VMemArena* arena; u32 flags; u32 _pad0; };
struct ArenaRegistry { ArenaEntry e[MAX_ARENAS]; u32 count; u8 sealed; u8 _pad[3]; u8 session_fingerprint[32]; };
void registry_add(ArenaRegistry* r, NameHash id, VMemArena* a, u32 flags);   // TL_FATAL if sealed, count == MAX_ARENAS, or duplicate id
void registry_seal(ArenaRegistry* r);                                       // after init; registration order frozen; its ids fold into session_fingerprint
void registry_set_fingerprint(ArenaRegistry* r, const u8 fp[32]);           // after seal (the app computes the fingerprint OVER the sealed ids, BUILD.md §5, then hands it back); stamped into every snapshot, checked on restore; until set it holds seal's own id fold, so the restore id/order gate is never vacuous (W1 mem review 2)
u64  registry_hash_all(const ArenaRegistry* r, u64 out_per_arena[MAX_ARENAS]) {
    for i in 0..count: out[i] = (e[i].flags & HASHED) ? tl_hash64(e[i].arena->base, e[i].arena->used, TL_HASH_SEED) : 0;
    return tl_hash64(out, count * 8, TL_HASH_SEED);
}
struct Snapshot { u8 session_fingerprint[32]; u64 tick; u32 count; u32 _pad0; u64 used[MAX_ARENAS]; u8* blob; u64 blob_cap; };
// blob layout: arena 0 [base, used) ‖ arena 1 ‖ … in registry order, each 64-byte aligned
ErrCode registry_snapshot(const ArenaRegistry* r, Snapshot* s, u64 tick);   // Σ used > blob_cap (budget violation): TL_FATAL in netcode/ship, ERR_MEM_RING_OVERFLOW in dev (§7 R-2)
ErrCode registry_restore(ArenaRegistry* r, const Snapshot* s);              // fingerprint + count + ids must match → else ERR_SNAPSHOT_MISMATCH; memcpy back; set used; high_water = max(high_water, used)
struct SnapshotRing { Snapshot slot[CONFIRMATION_HORIZON_TICKS]; u32 head; u32 count; };
ErrCode ring_init(SnapshotRing*, u64 slot_cap_bytes, VMemArena* backing);   // once at init: pushes every slot's blob (64-byte aligned) from the dedicated backing arena; slot_cap = the budgeted Σ used of snapshotted arenas (§6; T-A-03 replaces the guess)
Snapshot* ring_push(SnapshotRing*, u64 tick);  const Snapshot* ring_find(const SnapshotRing*, u64 tick);
```

`tl_hash64` = rapidhash (vendored, `foundation/hash.h`) with the pinned seed; `TL_HASH_SEED` from
`CANON.md`.

### 8.4 The guards

```cpp
struct ArenaGuard { u64 used_at_start[MAX_ARENAS]; u64 crt_allocs_at_start; u8 in_barrier; u8 _pad[7]; };
void guard_tick_begin(ArenaGuard*, const ArenaRegistry*);       // baselines used[] and the CRT counter
void guard_barrier_begin/end(ArenaGuard*, const ArenaRegistry*); // the GROWS_AT_BARRIER window: begin TL_FATALs if a barrier-flagged arena already grew this tick (growth is legal only INSIDE the window — §2, which the one-arg spelling could not enforce); end re-baselines barrier-flagged arenas
void guard_tick_end(ArenaGuard*, const ArenaRegistry*);   // for each arena: if used != used_at_start and !(flags & GROWS_AT_BARRIER) → TL_FATAL(name)
```

`dev`/`debug` only; compiled out elsewhere. The CRT-malloc counter is read at `guard_tick_end`:
nonzero → `TL_FATAL` in `dev` (vendor libs allocate only through pools, so a CRT malloc during a
tick is a leak of discipline somewhere).

**"Never inside a tick" is the guard's clause, and only the guard's** (ruled 2026-08-24). Container
growth that bump-allocates — `map_grow`'s rehash is the case that raised it (`CONTAINERS.md` §8.3)
— must not happen mid-tick, but the container cannot check it: tick-window knowledge lives here,
not in `map.h`, and no `in_tick` facility exists in foundation for a container to read. It needs
none. The rule is already enforced from this side and in the right currency: `guard_tick_end`
`TL_FATAL`s on any arena whose `used` moved during a tick without `GROWS_AT_BARRIER`, and
`guard_barrier_begin` `TL_FATAL`s if a barrier-flagged arena already grew this tick — a growing
container's `arena_push` *is* that `used` movement, so it is caught by name, at the tick boundary,
for every container at once rather than one assert per container. With `MEMORY.md` §1.2's
sized-at-init rule, growth exists only on non-hashed arenas, where `GROWS_AT_BARRIER` is exactly
the discipline these two calls police. The hook itself lands when the barrier-window API exists
(the guard owner's lane, `TODO.md`); this section is its home from now on.

### 8.5 `Handle`

```cpp
template <typename Tag, int IDX_BITS, int GEN_BITS> struct Handle {
    uint_fit<IDX_BITS + GEN_BITS> bits;
    static constexpr u32 IDX_MASK = (1u << IDX_BITS) - 1, GEN_MAX = (1u << GEN_BITS) - 1;
};
template <typename H> constexpr H handle_make(u32 idx, u32 gen);     // gen in [1, GEN_MAX]; TL_ASSERT
template <typename H> constexpr u32 handle_index(H h), handle_gen(H h);
template <typename H> constexpr bool handle_is_null(H h) { return h.bits == 0; }
```

### 8.6 `mem_pool` — the vendor heap

Size classes `16, 32, 64, 128, 256, 512, 1K, 2K, 4K, 8K, 16K, 32K, 64K` (13). Each class owns a
freelist; class pages are 64 KB carved from the pool's `VMemArena` (`arena_push` on demand, never
returned) — except the 64 K class, whose pages are TWO granules (128 KB, one block): a 64 KB
block cannot share a 64 KB page with its header (W1 mem, 2026-08-24). A block has no header — the
class is recovered from the page's header (first 16 bytes: `{u16 class; u16 large; u32 live;
u64 size}`; blocks start at offset 64), which works because every carve — class page or large
block — starts at a 64 KB-ALIGNED ADDRESS (`p & ~0xFFFF` finds it; the pool pays the alignment
gap out of its own reserve). Allocations > 64 KB: a dedicated 64 KB-granular carve with the same
64-byte header (`large = 1`, `size` = the request); freed by `decommit` of exactly that range
(the bump pointer is not moved — fragmentation of the *address* space is accepted, pages are
returned and the freed carve leaves the budget). `pool_realloc`: same class (or same large
carve) → return the same pointer; else alloc + memcpy(min) + free. Budget: `pool.budget_bytes`
checked at every carve; exceeding it returns `NULL` (Luau raises its own memory error; ImGui/SDL
assert). Stats (`live_bytes`, `peak`, per-class counts, `large_count`) are read by the profiler.

Adaptors (one per vendored lib, in `vendor_glue/`): `tl_luau_alloc(void* ud, void* p, size_t
osize, size_t nsize)`, `tl_imgui_alloc/free`, `tl_sdl_malloc/calloc/realloc/free`
(`SDL_SetMemoryFunctions` before `SDL_Init`), `tl_enet_malloc/free` (`ENetCallbacks`),
`STBI_MALLOC/REALLOC/FREE` macros pointing at the shared pool. Grep rule: `pool_alloc` appears
only in `mem_pool.cpp` and `vendor_glue/`.

### 8.7 Tests (`tests/foundation/`)

`vmem_arena.test.cpp`: push/align/commit growth, over-reserve fatal-expected, reset poison (debug),
decommit then re-push is zero, `high_water` re-zero rule; `registry.test.cpp`: hash changes iff
a registered byte changes (hash-region integrity), snapshot/restore round-trip equality per arena,
fingerprint mismatch refused, ring push/find/wrap; `guard.test.cpp`: growth outside the barrier →
fatal-expected, inside → ok; `handle.test.cpp`: make/index/gen, null, gen 0 refused;
`mem_pool.test.cpp`: every class alloc/free/reuse, large path, realloc same/different class,
budget → NULL, stats; a Luau VM lifecycle under the pool (in `tests/script/`).

### 8.8 Done criteria

Two worlds' registries in one process hash identically under the dual-sim harness; the
arena-offset guard passes a 10k-tick headless run; a snapshot restore mid-run reproduces the
original hash trace from that tick onward.

*Rev 1 — 2026-08-22; §7 R-2 mechanism split, §8.1 file list (vmem_api.h transcription,
alloc_shim.h, arena_guard.cpp non-det placement), §8.3 (`registry_set_fingerprint`, `ring_init`,
the dev overflow code), §8.4 (two-arg barrier guards) reconciled with the shipped headers by the
W1 mem lane, 2026-08-24.*
