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
void   eq_emit<T>(World*, T ev);    // bump-append into the WRITE buffer (scratch), O(1)
Span<T> eq_read<T>(World*);         // flat scan of last tick's READ buffer, immutable for the whole tick
```

- **Double-buffered, one-tick latency, swap+clear at the `LAST → FIRST` barrier** (the same point
  the `InputFrame` folds). Every reader sees *all* of last tick's events regardless of system order
  → producer and consumer are fully decoupled, no `before/after` coupling.
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
`ecs.declare_component("Health", { {"hp","i32"}, {"hp_max","i32"}, {"last_attacker","Entity"} })`.
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

*Rev 1 — 2026-08-22.*
