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
  tier); exceeding `reserved` is `TL_FATAL` — a blown budget is a bug, not silent growth. The Pi's
  table is smaller. T-A-03 replaces guesses with measurements.

### 1.2 Permanent arenas + the registered arena set

Every long-lived authoritative pool lives in an arena registered at init:

```cpp
struct ArenaEntry { NameHash id; VMemArena* arena; u32 flags; /* HASHED | SNAPSHOT | GROWS_AT_BARRIER */ };
struct ArenaRegistry { ArenaEntry e[MAX_ARENAS /*64*/]; u32 count; };
void  registry_add(ArenaRegistry*, NameHash id, VMemArena*, u32 flags);   // init only; order = registration order
u64   registry_hash_all(const ArenaRegistry*, u64 hashes_out[]);           // per-arena rapidhash over [base, used); world hash over those
void  registry_snapshot(const ArenaRegistry*, Snapshot*);                  // memcpy each [base, used) + used table + fingerprint
ErrCode registry_restore(ArenaRegistry*, const Snapshot*);                 // fail-loud on fingerprint/layout mismatch
```

- **Registration order is part of the lockstep contract** (the world hash folds in that order;
  the snapshot layout is that order). It is explicit in the `app/` wiring file.
- **Snapshot ring (T-F-04):** `N = CONFIRMATION_HORIZON_TICKS` slots allocated **once** from a
  dedicated arena at init; each slot sized at `Σ reserved` of snapshotted arenas at the *budget*,
  with the used extents recorded per slot so a restore copies only `[base, used)`. Cannot be sized
  for real until T-A-03.
- Built day one (~50 lines). The ECS columns, Alloy pools, the tick counter and RNG seed, and the
  compiled data tables all live in registered arenas. **Net buffers, render state, Luau heaps, and
  scratch are never registered.**

### 1.3 Frame/scratch arenas — one per worker thread

`Scratch { VMemArena a; }` with push/pop markers as the everyday API; `reset()` at frame end
(the main thread's) or at the barrier (workers'). Command buffers, event write buffers,
broadphase transients, neighbor lists, coarse-sampling grids, and the draw command list live here.
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
| tick counter, RNG seed, compiled data tables (restored but fingerprint-checked) | render double-buffers (prev/current are re-primed with a snap bit after restore) |
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

## 7. Open

- **O-1** Whether the ECS column per-type reserve is one VMem range per column (stable base per
  column, many reserves) or one big range sliced by max-entities × stride (fewer reserves, fixed
  cap). Lean: per column (growth without a cap; 64-bit address space is free). Decide at ECS build.
- **O-2** Snapshot ring slot sizing when Alloy pools grow past the budget mid-session: fatal
  (budget is a contract) vs ring re-allocation at a barrier. Lean: fatal in `netcode` tier
  (lockstep peers must agree on limits anyway), warn-and-grow in `dev`.

*Rev 1 — 2026-08-22.*
