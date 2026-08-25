#pragma once
// ---------------------------------------------------------------------------------------------
// world.h - World: the per-tick access hub; entities, component registration, typed access,
//   singletons, the recording API, the reflection hash.
//
// Spec: docs/ECS.md §1/§2/§7 (design), §10.3 (entities), §10.5 (recording), this header's
//   surface; docs/FRAME-LOOP.md §8.2 is the wiring order that calls these doors.
// Purpose: exactly one World per sim (the dual-sim test constructs two); no globals anywhere.
//   World composes the columns, the entity slotmap, the schedule, the command buffers and the
//   event tables, and owns the registration doors (init-only, closed by world_build_schedule).
// Invariants: component registration order = column arena registration order = part of the
//   lockstep contract (docs/ECS.md §2). tick and seed live in the registered "world.singletons"
//   arena (docs/CANON.md), reached through World::state - the World struct itself is never
//   hashed, so hashed facts cannot live in its members (reconciles §7's `u64 tick; u64 seed`
//   member spelling; TODO.md W2 ecs notes). world_spawn reserves an id WITHOUT growing any
//   registered arena (a free-list pop or a fresh-index counter - TODO.md E-3, ruling
//   recommended and built) and realization happens at the barrier via CMD_SPAWN_REALIZE;
//   destroys are commands. Singletons: one instance in its own registered arena
//   (HASHED|SNAPSHOT), reached by world_singleton<T> - systems-as-singletons are banned
//   (docs/DETERMINISM.md §2.5).
// Determinism: typed accessors resolve T by name hash through a Map lookup (cold O(1)); walk
//   order is the packed column order; nothing here reads a clock or allocates outside the
//   world's own arenas. The sparse-pages arenas carry GROWS_AT_BARRIER as well as SNAPSHOT
//   (pages commit during apply; "SNAPSHOT only" in docs/ECS.md §10.3 rules them OUT OF THE
//   HASH, and the guard flag is what makes their in-window commit legal).
// Threading: v0 single-threaded; registration is init-only.
// Includes: core/{reflect,column,schedule,events,commands,phase}.h, foundation/{slotmap,map,
//   arena_registry,scratch}.h.
// ---------------------------------------------------------------------------------------------
#include "core/reflect.h"
#include "core/column.h"
#include "core/schedule.h"
#include "core/events.h"
#include "core/commands.h"
#include "foundation/slotmap.h"
#include "foundation/map.h"
#include "foundation/arena_registry.h"
#include "foundation/scratch.h"
#include "foundation/strview.h"

struct InputFrame;    // docs/INPUT.md §1 (W3 loop+input)
struct DataTables;    // docs/ASSETS-AND-DATA.md §8.3 (W3 assets+data)
struct AlloyWorld;    // docs/ALLOY.md (W2/W3 alloy lanes)
struct RenderQueue;   // docs/RENDER2D.md (W3 render2d)
struct Editor;        // docs/TOOLING.md (W3 editor)
struct Interner;      // foundation/interner.h (forward: only luacomp needs it resolved)

// Per-entity record (docs/ECS.md §10.3): the component count and nothing else - no per-entity
// bitset (D6). Zeroed at spawn; hashed via the entity slotmap's slots arena.
struct EntityRecord {
    u16 comp_count;
    u16 _pad0;
};
static_assert(sizeof(EntityRecord) == 4, "explicit padding (docs/CPP-SUBSET.md section 5)");

// tick + seed - the head of the registered "world.singletons" arena (docs/CANON.md "Ticks...";
// docs/MEMORY.md §5 restores them with everything else).
struct WorldTickState {
    u64 tick;
    u64 seed;
};

// Init-time knobs (docs/ECS.md §10.5 reconciliation: the spec states mechanisms, not budgets;
// budgets are init parameters with stated defaults, TL_FATAL when blown).
struct WorldDesc {
    u64 seed;
    u64 meta_reserve;         // schedule storage, maps, luau tables; 0 = 16 MB
    u64 event_half_reserve;   // per event half; 0 = 8 MB
    u64 cmd_arena_reserve;    // chunk records + payloads per phase window; 0 = 16 MB. Budget
                              // arithmetic: each ACTIVE recorder chunk costs records_cap*16 B
                              // + payload_cap (defaults: ~384 KB -> ~41 active chunks per
                              // window before the over-reserve fatal). A blown budget is a
                              // bug, not silent growth (docs/MEMORY.md §1.1).
    u32 cmd_records_cap;      // per chunk; 0 = 8192
    u32 cmd_payload_cap;      // per chunk, bytes; 0 = 256 KB
};

// The hub (docs/ECS.md §7, reconciled: tick/seed behind `state`; guard/interner/meta members
// added by this lane - each is load-bearing for a spec'd behaviour and recorded in TODO.md).
struct World {
    ArenaRegistry* registry;    // the registered arena set (required)
    Scratch*       scratch;     // per-world scratch (schedule build, appliers' transients)
    ArenaGuard*    guard;       // nullable: when set, apply_commands brackets the barrier window
    const VMemApi* os;

    ComponentTable comps[MAX_COMPONENT_TYPES];
    u16 comp_count;
    u16 _pad0;
    u32 pending_fresh;          // fresh entity ids reserved this window (TODO.md E-3)
    u32 reserved_free;          // free-list entries reserved this window - READ, not popped
    u32 _pad2;                  // (review 1 D3: a mid-window pop moves hashed bytes, so a
                                // snapshot captured with a reservation outstanding could not be
                                // restored consistently; the pops apply at the window's start)

    SlotMap<EntityRecord, Entity> entities;

    Schedule sched;
    CommandBuffers cmds;
    EventTables events;

    const InputFrame* input;    // InputFrame[MAX_PEERS] for the tick (the loop sets it)
    WorldTickState* state;      // in the registered world.singletons arena
    const DataTables* data;
    AlloyWorld* sim;
    RenderQueue* render;
    Editor* editor;
    Interner* interner;         // nullable; luacomp interns names here when present

    VMemArena meta;             // permanent, NON-registered: schedule/map/luau-table storage
    VMemArena sing_arena;       // registered "world.singletons": WorldTickState
    VMemArena cmd_arena;        // transient: chunk records/payloads, reset after every apply
    Map<NameHash, u32> comp_by_name;
    Map<NameHash, u32> event_by_name;
    u8 sealed;                  // set by world_build_schedule; closes every registration door
    u8 _pad1[7];
};

// --- init and registration doors (init only, in docs/FRAME-LOOP.md §8.2's order) -------------

// Builds an empty world: wires registry/scratch/os, creates meta/singleton/command/event
// arenas, the entity slotmap (its four arenas registered HASHED|SNAPSHOT|GROWS_AT_BARRIER),
// and registers the "world.singletons" arena (HASHED|SNAPSHOT). Zero-fills every member first.
// Returns the first failing arena init's code.
ErrCode world_init(World* w, ArenaRegistry* registry, Scratch* scratch, const VMemApi* os,
                   const WorldDesc* desc);

// Registers a component type: assigns the next dense ComponentId, creates the column (three
// arenas - dense/entities HASHED|SNAPSHOT|GROWS_AT_BARRIER, pages SNAPSHOT|GROWS_AT_BARRIER)
// or, for COMP_SINGLETON infos, one zeroed instance in its own HASHED|SNAPSHOT arena.
// info must outlive the world (a constexpr table or a luacomp copy on `meta`). TL_FATAL:
// sealed, MAX_COMPONENT_TYPES, duplicate name_hash (docs/ECS.md §2).
ComponentId world_register_component(World* w, const ComponentInfo* info);

// Registers an event type (docs/ECS.md §5): same POD door, id = the name hash; cap 0 =
// EVENT_DEFAULT_CAP per half. TL_FATAL: sealed, table full, duplicate.
EventTypeId world_register_event(World* w, const ComponentInfo* info, u32 cap);

// Registers a system (forwards to the schedule; docs/ECS.md §3). TL_FATAL after seal.
void world_register_system(World* w, const SystemDesc* desc);

// Builds the schedule (docs/ECS.md §10.6), allocates the command chunk table (one chunk per
// schedule position + the external chunk), and seals every registration door.
void world_build_schedule(World* w);

// --- lookups and typed access ----------------------------------------------------------------

// The dense id for a registered component name, or MAX_COMPONENT_TYPES when unknown. Pure.
u32 world_find_component(const World* w, NameHash name);

// The event table index for an event name, or MAX_EVENT_TYPES when unknown. Pure.
u32 world_find_event(const World* w, NameHash name);

// T's dense id; TL_CHECK that T is registered.
template <typename T>
ComponentId world_component_id(const World* w) {
    const u32 id = world_find_component(w, tl_info_of((const T*)nullptr)->name_hash);
    TL_CHECK(id < MAX_COMPONENT_TYPES);
    return (ComponentId)id;
}

// The packed column of T (docs/ECS.md §2 - the hot path). Not for singletons (TL_CHECK).
// The span is tick-scoped: the next barrier may grow or reorder the column.
template <typename T>
Span<T> world_column(World* w) {
    ComponentTable* t = &w->comps[world_component_id<T>(w)];
    TL_CHECK((t->info->flags & COMP_SINGLETON) == 0);
    TL_CHECK(t->stride == (u32)sizeof(T));
    return Span<T>{ (T*)(void*)t->dense, t->count };
}

// The dense -> entity map parallel to world_column<T> (docs/ECS.md §2).
template <typename T>
Span<Entity> world_entities(World* w) {
    ComponentTable* t = &w->comps[world_component_id<T>(w)];
    TL_CHECK((t->info->flags & COMP_SINGLETON) == 0);
    return Span<Entity>{ t->entities, t->count };
}

// The probe (docs/ECS.md §2): T's row for e, or null when absent OR e is stale. Tick-scoped.
template <typename T>
T* world_get(World* w, Entity e) {
    ComponentTable* t = &w->comps[world_component_id<T>(w)];
    TL_CHECK((t->info->flags & COMP_SINGLETON) == 0);
    return (T*)column_get(t, e);
}

// The singleton instance (docs/ECS.md §2): always present once registered, writable in place
// (value writes are not structural; wholesale swaps go through world_singleton_set_cmd).
template <typename T>
T* world_singleton(World* w) {
    ComponentTable* t = &w->comps[world_component_id<T>(w)];
    TL_CHECK((t->info->flags & COMP_SINGLETON) != 0);
    return (T*)(void*)t->dense;
}

// True iff e names a live, realized entity (reserved-not-yet-realized reads false - the
// queryable-absence idiom, docs/CONTAINERS.md §8.2).
bool world_entity_alive(const World* w, Entity e);

// --- the recording API (docs/ECS.md §4 - deferred, applied at the next barrier) --------------

// Reserves a usable id NOW without touching any registered byte (a free-list READ through the
// reserved_free cursor, or the fresh-index counter - TODO.md E-3, review 1 D2/D3) and records
// CMD_SPAWN_REALIZE. The reserved pops apply at the next window's start and fresh slots push
// at their realize, both inside the barrier; realize order is free (the external chunk records
// first and applies last). The id is stable and commandable immediately; world_entity_alive
// turns true at the barrier. TL_FATAL when the domain is exhausted.
Entity world_spawn(World* w);

// Records CMD_DESTROY. Applying removes e from every column it is in, then frees the slot
// (LIFO reuse; generation wrap quarantines - docs/ECS.md §10.3). Destroying an already-dead
// entity applies as a no-op (the normal stale-reference flow).
void world_destroy(World* w, Entity e);

// Records CMD_ADD of `value` (comp's full row). Applying TL_CHECKs the entity is live and the
// component absent. (Add-after-destroy in one window is currently a fatal - TODO.md E-4.)
void world_add_raw(World* w, Entity e, ComponentId comp, const void* value);

// Typed world_add (docs/ECS.md §4's typed wrapper).
template <typename T>
void world_add(World* w, Entity e, const T& value) {
    world_add_raw(w, e, world_component_id<T>(w), &value);
}

// Records CMD_REMOVE. Applying TL_CHECKs the entity is live and the component present.
void world_remove(World* w, Entity e, ComponentId comp);

// Records CMD_SET_FIELD: writes the field's bytes at its offset in e's existing row at apply
// (editor/Luau cold path - docs/ECS.md §10.5). len must equal the field's size.
void world_set_field_cmd(World* w, Entity e, ComponentId comp, u32 field_index,
                         const void* bytes, u32 len);

// Records CMD_SINGLETON_SET: a wholesale singleton swap applied at the barrier.
void world_singleton_set_cmd(World* w, ComponentId comp, const void* value);

// Explicit mid-phase flush - the single-threaded escape hatch (docs/ECS.md §4).
void world_flush(World* w);

// --- events (typed wrappers over events.h; docs/ECS.md §5) -----------------------------------

// Emits ev into T's write buffer (visible to every reader NEXT tick). TL_CHECK: registered.
template <typename T>
void eq_emit(World* w, const T& ev) {
    const u32 idx = world_find_event(w, tl_info_of((const T*)nullptr)->name_hash);
    TL_CHECK(idx < MAX_EVENT_TYPES);
    events_emit(&w->events, idx, &ev);
}

// Last tick's T events - flat, immutable for the whole tick. TL_CHECK: registered.
template <typename T>
Span<const T> eq_read(World* w) {
    const u32 idx = world_find_event(w, tl_info_of((const T*)nullptr)->name_hash);
    TL_CHECK(idx < MAX_EVENT_TYPES);
    EventSlice s = events_read(&w->events, idx);
    TL_CHECK(s.stride == (u32)sizeof(T));
    return Span<const T>{ (const T*)(const void*)s.data, s.count };
}

// The LAST -> FIRST barrier's event step (docs/CANON.md step 2); the loop calls it after LAST.
void world_events_swap(World* w);

// --- Luau-declared components and events (docs/ECS.md §6.1/§10.7; luacomp.cpp) ---------------

// One declared field: name (any bytes; copied), a kind from the closed enum, count 1..255,
// and the default as raw bits (fx rows cross as raw i32 bits - docs/FX-PALETTE.md §6; 0 for
// "no default"). The core ErrCode range 0x031x names the reject reasons.
struct LuauFieldDecl {
    StrView   name;
    FieldKind kind;
    u8        _pad0;
    u16       count;
    u32       _pad1;
    u64       default_bits;
};

constexpr ErrCode ERR_ECS_DUPLICATE_NAME = (ErrCode)0x0311;  // component/event name already registered
constexpr ErrCode ERR_ECS_BAD_KIND      = (ErrCode)0x0312;   // kind outside the closed enum
constexpr ErrCode ERR_ECS_BAD_COUNT     = (ErrCode)0x0313;   // field count 0 or > 255 (docs/LUAU-LAYER.md §10.6)
constexpr ErrCode ERR_ECS_TABLE_FULL    = (ErrCode)0x0314;   // MAX_COMPONENT_TYPES / MAX_EVENT_TYPES / no fields / too many fields
constexpr ErrCode ERR_ECS_SEALED        = (ErrCode)0x0315;   // declaration after world_build_schedule
constexpr ErrCode ERR_ECS_BAD_NAME      = (ErrCode)0x0316;   // a field name spelling _pad* (the synthesized-pad namespace - review 1 D1)
constexpr ErrCode ERR_ECS_BAD_DEFAULT   = (ErrCode)0x0317;   // default wider than the field's scalar, or on a handle/StrId kind (docs/LUAU-LAYER.md §10.6)

// The declaration-door field bound: worst-case pad synthesis doubles the rows + a tail pad,
// and the save decoder walks at most ENC_MAX_FIELDS (encoder.h) stored rows - a component
// past this could never be decoded again (review 1 D6).
enum : u32 { LUACOMP_MAX_FIELDS = 127 };

// Builds the runtime FieldInfo table with the deterministic packer (declaration order, natural
// alignment, every interior gap and the tail an explicit _padN field - docs/LUAU-LAYER.md
// §10.6; docs/ECS.md §10.7) on the world's permanent meta arena, then registers the column
// exactly like a C++ component (same kinds, inspector, encoder, fingerprint contribution).
// Names are interned when the world carries an interner, and always copied onto `meta`.
// flags: COMP_SINGLETON for `opts = { singleton = true }`. Recoverable errors, never fatal -
// the binding surfaces them as script errors (docs/ASSETS-AND-DATA.md §3 fail-loud compile).
Result<ComponentId> world_register_component_luau(World* w, StrView name,
                                                  const LuauFieldDecl* fields, u32 field_count,
                                                  u32 flags);

// The event-door twin (`ecs.event` - docs/LUAU-LAYER.md §10.6): same packer, same rejects;
// cap 0 = EVENT_DEFAULT_CAP.
Result<EventTypeId> world_register_event_luau(World* w, StrView name,
                                              const LuauFieldDecl* fields, u32 field_count,
                                              u32 cap);

// --- fingerprint input -----------------------------------------------------------------------

// The reflection-table hash (docs/ECS.md §10.2): per-component hashes in registration order,
// then per-event-type hashes in registration order, folded with tl_hash64. One input of
// session_fingerprint (docs/BUILD.md §5).
u64 world_reflection_hash(const World* w);

// The ECS half of the post_restore barrier (docs/MEMORY.md §5): after registry_restore,
// rebuilds every derived non-arena count (slotmap counts/caps, per-column counts) from the
// restored bytes, discards pending commands, and clears both event halves. Call before the
// first tick after any restore.
void world_post_restore(World* w);
