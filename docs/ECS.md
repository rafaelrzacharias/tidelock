# ECS — world, components, systems, commands, events, reflection (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §9. Expands `PIVOT-DESIGN.md` §6;
> carries foundry D4/D5/D6/D7/D12/D15 + FOUNDRY-API §3 as written, in C++.
> **Owns:** `src/core/world.h`, `column.h`, `schedule.h`, `commands.h`, `events.h`, `reflect.h`.

---

## 0. Shape (DECIDED — flecs and EnTT rejected)

Own minimal ECS: entities + **type-erased paged sparse-set columns** on `VMemArena`; queries =
iterate-smallest + O(1) probe; **and stop**. No query DSL, no archetype graph, no change detection,
no relationships (a `Parent` component + one pass when a consumer appears), no prefabs (Luau spawn
functions + data tables), no observers (`EventQueue<T>`).

flecs rejected: its allocators put gameplay state outside the registered arena set (breaks
snapshot/hash); its determinism (id recycling, table order, deferred merge) is an audit debt re-
paid per upgrade; ~90% of its surface unused. EnTT rejected on the template/compile-time rule.

Gameplay entities are the *small* population (thousands). Alloy's particles/bodies stay in their
own SoA pools with plain indices/handles (`ALLOY.md` §1.1); an entity that *has* a body holds a
`BodyHandle` component.

---

## 1. Entities (DECIDED)

`Entity = Handle<EntityTag, 22, 10>` (u32). A `SlotMap<EntityRecord>` holds per-entity: generation
(in the handle column), a `u16` component-count, and nothing else. There is no per-entity bitset
(D6). `world_spawn` returns a usable reserved id immediately; realization (slot commit) happens at
the barrier (§4).

---

## 2. Components and columns (DECIDED)

```cpp
ComponentId world_register_component(World*, const ComponentInfo* info);   // init only; id = dense u16; 1024 max
// ComponentInfo = { NameHash name; u32 size; u32 align; const FieldInfo* fields; u32 field_count; u32 flags; }
```

- **POD enforced at the door:** `static_assert(is_trivially_copyable<T>)` in the typed wrapper;
  the reflection walk asserts no pointer-typed members (debug). Same assert gates event types.
- **Column = packed dense `Array<T>` (own VMem range) + `Array<Entity>` (dense→entity) + paged
  sparse (entity index → dense index, pages of 4096 `u32`, committed on demand).** Add/remove are
  O(1); remove is swap-remove; iteration is `0..count` packed.
- **Walk order is deterministic for a given world state** (packed order) and reproducible across
  runs/machines because structure changes only at barriers with a fixed apply order.
- **Each column is a registered arena** (flags `HASHED | SNAPSHOT | GROWS_AT_BARRIER`) — the
  column *is* the hash/snapshot unit, so a desync localizes to a component type. Registration
  order = component registration order = part of the lockstep contract.
- **Singleton components** (world resources, persistent system state): a component registered
  with `SINGLETON`, one instance in its own small registered arena; `world_singleton<T>(w)`.
  Systems-as-singletons are banned (`DETERMINISM.md` §2.5).
- Direct column access is the hot path: `Span<T> world_column<T>(World*)` + `Span<Entity>
  world_entities<T>(World*)`. `T* world_get<T>(World*, Entity)` is the probe (nullable,
  tick-scoped).

---

## 3. Systems and the schedule (DECIDED)

```cpp
typedef void (*SystemFn)(World* w);
struct SystemDesc { SystemFn fn; NameHash label; Phase phase;
                    Span<const ComponentId> reads, writes; Span<const NameHash> before, after; u32 flags; };
void world_register_system(World*, const SystemDesc*);   // init only; order = registration order
```

- **Systems are stateless free functions.** A system receives `World*` and nothing else; it reads
  the `InputFrame`, columns, singletons, event read-buffers; it writes columns, commands, event
  write-buffers. There is no way to call another system.
- **Ordering:** registration order, refined by `before`/`after` on stable labels; topo-sorted with
  registration order as the tie-break (total, reproducible); a cycle is `TL_FATAL` at
  registration. **Built once at startup** (rebuilt on Luau script reload re-registration — a
  sealed, tick-stamped event). The per-tick loop executes the schedule; zero scheduling cost.
- **`reads`/`writes` are for parallelism, never ordering.** v0 runs serial. When `JOBS.md` lands,
  non-conflicting systems within a phase run in parallel, conflicting ones serialize in the fixed
  order. A debug mode checks actual access against declarations (column access through a checked
  accessor records the touch).
- **Phases:** `FIRST · PRE_UPDATE · UPDATE · POST_UPDATE · LAST` (sim, per tick) ·
  `PRE_RENDER · RENDER` (render, per frame). Position-named, not role-named: role names bake a
  real-time-physics pipeline assumption (the soft Layr mistake). Conventions in `FRAME-LOOP.md` §2.
- Luau-registered systems are C++ trampolines with the same `SystemDesc` (`LUAU-LAYER.md` §4);
  ordering treats them identically.

---

## 4. Structural change — the deferred command buffer (DECIDED)

All structural changes (spawn/destroy entity, add/remove component, singleton swaps) and all sim
edits (`ALLOY.md` §9.2) are **recorded, never performed in place**:

```cpp
Entity world_spawn(World*);                    // reserved id now; realized at the barrier
void   world_destroy(World*, Entity);
void   world_add(World*, Entity, ComponentId, const void* value);   // typed wrapper world_add<T>
void   world_remove(World*, Entity, ComponentId);
void   world_flush(World*);                    // explicit mid-phase flush — escape hatch, single-threaded only
```

- Recorded into per-chunk buffers on scratch (single-threaded v0: one buffer per system);
  **applied at every phase barrier in chunk order, record order within a chunk**. Every phase
  boundary is a barrier, so each phase sees the prior phase's changes.
- Mid-iteration mutation is structurally impossible (the API defers) — there is no footgun to
  assert on. Iteration always runs on a stable snapshot.
- Command records are POD `{ kind: u8, entity, component_id, payload_offset }` with payloads copied
  into scratch; applying them is the `GROWS_AT_BARRIER` window for columns (`MEMORY.md` §2).
- The same buffer carries **editor mutations** (dev: ImGui writes become commands — never mid-frame
  column pokes) and **Luau edits**. Commands are therefore the one channel through which anything
  external changes structure, which is what makes replay of editor sessions possible later.

---

## 5. Events — `EventQueue<T>` (DECIDED — D15 as written)

```cpp
EventTypeId world_register_event(World*, const ComponentInfo* info);   // same POD door; id = NameHash
void   eq_emit<T>(World*, T ev);    // bump-append into the WRITE half of the event arena, O(1)
Span<T> eq_read<T>(World*);         // flat scan of last tick's READ buffer, immutable for the whole tick
```

- **Double-buffered, one-tick latency, swap+clear at the `LAST → FIRST` barrier** (the same point
  the `InputFrame` folds). Every reader sees *all* of last tick's events regardless of system order
  → producer and consumer are fully decoupled, no `before/after` coupling. Buffers live in a
  dedicated **event arena with two halves** (not scratch — the read half must survive past the
  frame's scratch reset; see §10.4).
- Drain order within a type = emission order = the total system order → deterministic.
- **Never in the state hash** (transient derived data; their effects on columns are hashed).
- Same-tick coupling is **not an event**: express it as shared-component dataflow in phase order or
  a deferred command. One canonical event form; no fire-and-forget dispatch (the Layr `Signal<T>`
  mistake); no callbacks stored in components (they'd dangle across Luau reload and don't
  serialize).
- Direct callbacks exist only at the engine/platform boundary (window events → engine), never for
  gameplay.
- Alloy's event stream (`RingBuffer`, per-chunk merged) is bridged into typed `EventQueue`s by one
  system at `POST_UPDATE` so Luau reacts to sim events through the same reader.

---

## 6. Reflection — one X-macro per component (DECIDED)

```cpp
#define TL_FIELDS_Health(X, XA, XH) \
    X (i32, hp) X (i32, hp_max) XH(Entity, last_attacker) XA(u8, flags, 4)
TL_COMPONENT(Health)
```

expands to the POD struct, a `constexpr FieldInfo Health_fields[]`
(`{ name, name_hash, kind, offset, size, count }`), a `ComponentInfo`, and `static_assert`s:
trivially copyable; `sizeof == Σ field sizes` (explicit padding — name pads `_padN`); no pointer
kinds. `kind` is a **closed enum**: the fx palette rows, `i8..i64/u8..u64`, `bool`, every `Handle`
domain, `StrId`, fixed arrays of those. A field of any other type fails to compile.

One table feeds four consumers:

1. **Generic ImGui inspector** — walk fields, switch on kind; every component inspectable the
   moment it is declared. Optional per-component custom-draw hook and per-system `debug_draw`
   (`TOOLING.md` §2). Dev only; edits go through the command buffer.
2. **M2 durable saves** — name-keyed encoder/decoder (renames via alias entries; added fields via
   declared defaults) — `ASSETS-AND-DATA.md` §5.
3. **Desync dumps** — field-by-field diff of a diverging component (`DETERMINISM.md` §7).
4. **Luau access by field name** (`LUAU-LAYER.md` §3) — and the Luau-declared components below.

**The field tables hash into the build fingerprint** (name-hash + kind + offset per field, per
component, in registration order) — the cross-peer layout check (`BUILD.md` §5).

### 6.1 Luau-declared components and events

Games declare their own components from Luau at init:
`ecs.component("Health", { {"hp","i32"}, {"hp_max","i32"}, {"last_attacker","Entity"} })`.
The engine builds a runtime `FieldInfo` table in the permanent arena with a **deterministic packer**
(declaration order, natural alignment, explicit tail pad to the max alignment) and registers the
column exactly like a C++ component. Same kinds, same inspector, same encoder, same fingerprint
contribution. Declaration order in the script is the registration order; the script is the same
bytecode on every peer, so every peer builds the same table.

---

## 7. The `World` (DECIDED)

```cpp
struct World {
    ArenaRegistry* registry;  Scratch* scratch;            // memory
    ComponentTable comps[1024]; u16 comp_count;             // columns + infos
    SlotMap<EntityRecord, Entity> entities;
    Schedule sched;                                         // built once
    CommandBuffers cmds;  EventTables events;
    const InputFrame* input;  u64 tick;  u64 seed;          // tick + seed are in a registered singleton arena
    const DataTables* data;                                 // compiled Luau tables (ASSETS-AND-DATA.md §3)
    AlloyWorld* sim;  RenderQueue* render;  /* dev */ Editor* editor;
};
```

`World*` is the per-tick access hub; there is exactly one per sim (the dual-sim test constructs
two). No globals.

---

## 8. Tests

Container rubric + : spawn/destroy/add/remove sequences vs a naive model with order checks; command
application order under multiple recording systems; event double-buffer semantics (emit in N visible
in N+1 only; cleared in N+2); schedule topo-sort determinism incl. ties and a cycle → fatal;
reflection: every kind round-trips through the encoder; two worlds, same inputs → identical
per-column hashes every tick.

---

## 9. Rulings (closed 2026-08-22 — nothing open)

- **R-1 `MAX_COMPONENT_TYPES = 1024`, `ComponentId` is `u16`.** It is a registry bound (tens of
  KB), not a per-entity cost; 256 would save nothing measurable and cap a modded game.
- **R-2 Luau-declared components may contain fx-row fields.** The kind enum names every palette
  row; Luau reads/writes them as raw bits (`FX-PALETTE.md` §6). **No write-side range check exists**:
  every `i32` bit pattern is a legal value of a 32-bit row by definition (the row's range *is*
  the type's range), so there is nothing to check; semantic limits (e.g. `V_MAX_WORLD`) are the
  data validator's job at `init()`, and the integrator's debug assert catches a runaway at runtime.

## 10. Implementation specification

### 10.1 Files (`src/core/`)

| File | Contents |
|---|---|
| `reflect.h` | `FieldKind`, `FieldInfo`, `ComponentInfo`, the `TL_FIELDS_*`/`TL_COMPONENT`/`TL_POOL_ROW`/`TL_WIRE_STRUCT` macros, `kind_of<T>` |
| `world.h/.cpp` | `World`, init/shutdown, entity slotmap, component registration, `world_get/column/entities` |
| `column.h/.cpp` | `ComponentTable` (paged sparse set) add/remove/probe |
| `schedule.h/.cpp` | `SystemDesc` storage, topo-sort, phase arrays, `run_phase` |
| `commands.h/.cpp` | command records, per-chunk buffers, `apply_commands` |
| `events.h/.cpp` | `EventTable`, the two-half event arena, emit/read/swap |
| `luacomp.cpp` | Luau-declared component packer (§6.1) — builds `FieldInfo` tables at runtime |
| `encoder.h/.cpp` | the name-keyed save encoder/decoder over field tables (`ASSETS-AND-DATA.md` §5) |
| `diff.cpp` | field-by-field diff of two component rows (desync dumps) |

Plus one file below `src/core/`: **`src/foundation/bytes.h`** — the little-endian
`ByteWriter`/`ByteReader` pair the `TL_WIRE_STRUCT`-generated `wire_write_*`/`wire_read_*`
functions (§10.2) write through. `NETCODE.md` §1 homes the byte pair in `src/foundation/` (it
must sit below every consumer: `net/wire.h`, the save encoder, `InputFrame`); it landed from
this lane as the macro's first consumer, and the net lane's `wire.h` layers varint/zigzag
helpers on top (`NETCODE.md` §20.1). Writer overflow is a bug (`TL_CHECK`); reader underflow is
data — a sticky `ErrCode` checked once after the last field read (W2 ecs, 2026-08-25).

### 10.2 Reflection

```cpp
enum FieldKind : u8 { K_i8, K_u8, K_i16, K_u16, K_i32, K_u32, K_i64, K_u64, K_bool,
    K_pos, K_vel, K_invmass, K_stiff, K_q, K_angle, K_omega, K_dt, K_scalar,     // palette rows
    K_Entity, K_Tex, K_Font, K_Audio, K_Clip, K_Data, K_Body, K_Constraint, K_Agent, K_Plant, K_Cavity, K_Basin,  // handle domains
    K_StrId, K_COUNT };
struct FieldInfo { const char* name; NameHash name_hash; FieldKind kind; u8 _pad0; u16 count /* 1, or array length */; u32 offset; u32 size; };
struct ComponentInfo { const char* name; NameHash name_hash; u32 size; u32 align; const FieldInfo* fields; u32 field_count; u32 flags /* SINGLETON, HIDDEN */; };
```

**Kinds are token-keyed, not type-keyed** (reconciled 2026-08-25, W2 ecs — `TODO.md` E-1). Rev 1
specified `kind_of` as overloaded functions (`kind_of(pos_t*)`, `kind_of(invmass_t*)`, …), which
cannot exist under RR-5's format-keyed rows ruling: `pos_t` and `invmass_t` are ONE C++ type, so
those two overloads are one redefined function and a per-row answer is unreachable from a type.
The spelled *token* in the field list is the only place the row survives to compile time, so the
kind lookup is `tl_field_kind_##T` — one `constexpr FieldKind tl_field_kind_<spelling>` constant
per legal spelling, still a closed set, and an unlisted type still fails to compile (undeclared
identifier). Field lists therefore spell the canonical row name (`pos_t`, never `fx<i32,18>` —
whose comma an X-macro argument cannot carry anyway). Owners of not-yet-built handle domains add
their constants beside their type definitions; the enum rows already exist.

```cpp
#define TL_X_FIELD(T, n)        T n;
#define TL_X_ARRAY(T, n, N)     T n[N];
#define TL_X_HANDLE(T, n)       T n;
#define TL_X_INFO(T, n)         { #n, fnv1a64(#n, sizeof(#n) - 1), tl_field_kind_##T, 0, 1, offsetof(TL_SELF, n), sizeof(T) },
#define TL_X_INFO_A(T, n, N)    { #n, fnv1a64(#n, sizeof(#n) - 1), tl_field_kind_##T, 0, N, offsetof(TL_SELF, n), sizeof(T) * N },
#define TL_COMPONENT(Name)                                                            \
    struct Name { TL_FIELDS_##Name(TL_X_FIELD, TL_X_ARRAY, TL_X_HANDLE) };            \
    static_assert(__is_trivially_copyable(Name));                                     \
    struct Name##_tbl { using TL_SELF = Name;                                         \
        static constexpr FieldInfo rows[] = { TL_FIELDS_##Name(TL_X_INFO, TL_X_INFO_A, TL_X_INFO) }; }; \
    static_assert(tl_fields_sum_size(Name##_tbl::rows) == sizeof(Name), "explicit padding required"); \
    inline constexpr const FieldInfo* Name##_fields = Name##_tbl::rows;               \
    inline constexpr ComponentInfo Name##_info = { #Name, fnv1a64(#Name, sizeof(#Name) - 1), sizeof(Name), alignof(Name), Name##_tbl::rows, tl_count(Name##_tbl::rows), 0 }; \
    constexpr const ComponentInfo* tl_info_of(const Name*) { return &Name##_info; }   // the typed-API hook
```

(`TL_SELF` is an alias in `Name##_tbl`'s scope — a macro cannot emit `#define`, so rev 1's
"defined/undefined around the expansion" was unimplementable as written; the nested table struct
is where `offsetof` finds its subject. Same reconciliation: `#n##_id` cannot paste a string
literal onto an identifier, so the name hash is spelled `fnv1a64(#n, sizeof(#n) - 1)` — the same
function `""_id` runs. `tl_fields_sum_size` is `constexpr`.) `TL_POOL_ROW` = the same without
the typed-API hook (pool rows are indexed by their pool, never an ECS column); `TL_WIRE_STRUCT` adds
`u32 format_version` as field 0, a `static_assert(offsetof(Name, f) == expected)` per field from a
parallel `TL_OFFSETS_Name` list, and generates `wire_write_Name(ByteWriter*, const Name*)` /
`wire_read_Name(ByteReader*, Name*) → ErrCode` over little-endian writers (`CPP-SUBSET.md` §9 R-2).

The **reflection table hash** (part of `session_fingerprint`): for each registered component in
registration order, `tl_hash64` over `(name_hash, size, align, for each field: name_hash, kind,
count, offset, size)`.

### 10.3 Columns and entities

```cpp
struct ComponentTable {
    const ComponentInfo* info;
    VMemArena dense_arena, entity_arena, page_arena;   // three own VMem ranges; dense+entity are registered (HASHED|SNAPSHOT|GROWS_AT_BARRIER)
    u8*      dense;       u32 count;                   // stride = info->size (rounded up to align)
    Entity*  entities;                                 // dense → entity
    u32**    pages;       u32 page_count;              // entity index >> 12 → page of 4096 u32 (dense index or NONE = 0xFFFFFFFF); pages committed on demand
};
enum { PAGE_SHIFT = 12, PAGE_SIZE = 4096, NONE = 0xFFFFFFFFu };

u32* sparse_slot(ComponentTable* t, u32 eidx) { u32 p = eidx >> PAGE_SHIFT; if (p >= page_count) grow_pages(t, p+1); if (!pages[p]) pages[p] = push page filled with NONE; return &pages[p][eidx & (PAGE_SIZE-1)]; }
void column_add(ComponentTable* t, Entity e, const void* v) { u32* s = sparse_slot(t, idx(e)); TL_CHECK(*s == NONE); *s = t->count; memcpy(dense + count*stride, v, size); entities[count] = e; count++; }
void column_remove(ComponentTable* t, Entity e) { u32* s = sparse_slot(...); u32 d = *s; TL_CHECK(d != NONE); u32 last = count-1;
    if (d != last) { memcpy(dense+d*stride, dense+last*stride, size); entities[d] = entities[last]; *sparse_slot(t, idx(entities[d])) = d; }
    memset(dense+last*stride, 0, stride); entities[last] = Entity{0}; *s = NONE; count--; }
void* column_get(ComponentTable* t, Entity e) { u32 p = idx(e) >> PAGE_SHIFT; if (p >= page_count || !pages[p]) return null; u32 d = pages[p][idx(e) & 4095]; return d == NONE || entities[d].bits != e.bits ? null : dense + d*stride; }
```

The sparse pages are *not* hashed (they are derivable from `entities[]`); they are snapshotted
anyway for O(1) restore (flag `SNAPSHOT` only). The generation check in `column_get` makes a stale
`Entity` read as absent.

`World` holds `SlotMap<EntityRecord, Entity> entities` (`EntityRecord { u16 comp_count; u16
_pad; }`) in a registered arena; `world_spawn` reserves an id immediately by inserting a zero
record (the slotmap's LIFO order is deterministic), and `world_destroy` is a command.

### 10.4 Events — the two-half event arena

```cpp
struct EventTable { const ComponentInfo* info; u32 stride; u32 write_count, read_count; u8* write; u8* read; u32 cap; };
struct EventTables { VMemArena half[2]; u32 write_half; EventTable t[MAX_EVENT_TYPES /*256*/]; u32 count; };
```

Each type gets a fixed capacity (declared at registration, default 4096 events) in **both**
halves, pushed at init. `eq_emit`: `TL_CHECK(write_count < cap)` (an overflow is a bug, not a
drop), memcpy into `write + write_count*stride`, `write_count++`. `eq_read`: `Span{read,
read_count}`. At the barrier: for every table `read = write; read_count = write_count; write =
other half's block; write_count = 0`. The halves are not registered arenas (events are never
hashed or snapshotted); after a rollback both halves are cleared.

### 10.5 Commands

```cpp
enum CmdKind : u8 { CMD_SPAWN_REALIZE, CMD_DESTROY, CMD_ADD, CMD_REMOVE, CMD_SET_FIELD, CMD_SINGLETON_SET, CMD_ALLOY /* forwarded to the edit channel */, CMD_SCRIPT_RELOAD, CMD_DATA_RELOAD, CMD_ASSET_READY };
struct CmdRecord { CmdKind kind; u8 _pad0; u16 comp; Entity e; u32 payload_off; u32 payload_len; };   // 16 B
struct CmdChunk  { Array<CmdRecord> recs; Array<u8> payload; u32 chunk_id; };   // on the recording worker's scratch
```

v0 (single-threaded): one chunk per system, `chunk_id` = system index in schedule order.
`apply_commands(World*)` at each barrier: chunks in ascending `chunk_id`, records in order;
`CMD_ADD` copies payload into the column; `CMD_DESTROY` removes the entity from every column it is
in (walk all tables — 1024 probes max; entities are few) then frees the slot; `CMD_SET_FIELD`
writes `payload_len` bytes at `field.offset` (editor/Luau cold path). After applying, every
chunk's scratch is released. The apply window sets `guard_barrier_begin/end`.

### 10.6 Schedule

```cpp
struct SystemRec { SystemDesc d; u32 reg_index; u32 phase_pos; };
struct Schedule { Array<SystemRec> systems; u32 phase_begin[PHASE_COUNT + 1]; /* indices into `order` */ Array<u32> order; };
```

`schedule_build`: per phase, collect systems; build edges from `before`/`after` (label lookup via
a `Map<NameHash,u32>`; unknown label → `TL_FATAL`); Kahn's algorithm where the ready set is
scanned for the **lowest `reg_index`** each step (O(n²), n ≈ hundreds, startup only); leftover
nodes → cycle → `TL_FATAL` naming them. `run_phase(World*, Phase)`: for `i` in the phase's slice
set `w->sched.running = { order[i], label }` (the Luau trampoline and the profiler auto-scope
read it — `LUAU-LAYER.md` §10.6), call `systems[order[i]].d.fn(world)`, clear `running`; then
`apply_commands`. With `JOBS.md` the same slice is partitioned into conflict-free groups by
`reads/writes` intersection (computed once at build) and each group is one `parallel_for` over its
systems with grain 1; groups run in order.

### 10.7 Luau-declared components (packer)

Input: ordered list of `(name, kind, count)`. `offset = 0; for each field: align = kind_align(kind)
(the natural alignment of its C type); offset = align_up(offset, align); record; offset += size*count`.
`size = align_up(offset, max_align)`; if `size > Σ sizes` a trailing `_padN` field of kind `K_u8`
with `count = size − Σ` is appended so the explicit-padding invariant holds and the row hashes as
zeros. `FieldInfo` array and `ComponentInfo` are pushed into the permanent arena; names are
interned. Registration then proceeds exactly as for C++ components.

### 10.8 Tests (`tests/core/`)

`reflect.test.cpp` (every kind round-trips through the encoder; padding assert trips on a crafted
struct — compile-fail test via the negatives lane), `column.test.cpp` (add/remove/get model vs a
naive map over 100k random ops; stale entity reads absent; page growth), `commands.test.cpp`
(apply order across chunks; destroy removes from all columns; spawn id usable before realize),
`events.test.cpp` (emit in N visible in N+1 only; cleared in N+2; overflow fatal-expected),
`schedule.test.cpp` (tie-break by registration; before/after; cycle fatal-expected; unknown
label fatal-expected), `luacomp.test.cpp` (packer layouts vs C++ `offsetof` for mirrored structs),
`world_dual.test.cpp` (two worlds, same inputs → identical per-column hashes per tick).

*Rev 1 — 2026-08-22.*
