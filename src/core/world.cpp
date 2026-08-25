// world.cpp - init, the registration doors, entity reservation, lookups, the reflection hash,
// and the post-restore fixup. Spec: docs/ECS.md §2/§7/§10.3; contracts in world.h.
#include "core/world.h"
#include "foundation/bytes.h"
#include <string.h>

namespace {

// Derives an arena id from a component name hash + a role suffix (FNV-1a continuation - the
// same family as ""_id, so ids are deterministic and collision-checked by registry_add).
NameHash name_hash_suffix(NameHash base, const char* suffix) {
    u64 h = base;
    for (u32 i = 0; suffix[i] != '\0'; ++i) {
        h ^= u64(u8(suffix[i]));
        h *= FNV1A64_PRIME;
    }
    return h;
}

// The world's default budgets (world.h WorldDesc: 0 = these).
constexpr u64 WORLD_META_RESERVE_DEFAULT  = 16u * 1024u * 1024u;
constexpr u64 WORLD_EVENT_HALF_DEFAULT    = 8u * 1024u * 1024u;
constexpr u64 WORLD_CMD_ARENA_DEFAULT     = 16u * 1024u * 1024u;
constexpr u32 WORLD_CMD_RECORDS_DEFAULT   = 8192u;
constexpr u32 WORLD_CMD_PAYLOAD_DEFAULT   = 256u * 1024u;

}  // namespace

ErrCode world_init(World* w, ArenaRegistry* registry, Scratch* scratch, const VMemApi* os,
                   const WorldDesc* desc) {
    TL_CHECK(w != nullptr && registry != nullptr && scratch != nullptr && os != nullptr && desc != nullptr);
    memset(w, 0, sizeof(*w));
    w->registry = registry;
    w->scratch = scratch;
    w->os = os;

    ErrCode e = vmem_arena_init(&w->meta, "world.meta"_id,
                                desc->meta_reserve != 0u ? desc->meta_reserve : WORLD_META_RESERVE_DEFAULT,
                                0u, os);
    if (e != ERR_OK) { return e; }
    e = vmem_arena_init(&w->sing_arena, "world.singletons"_id, sizeof(WorldTickState),
                        ARENA_ZERO_ON_PUSH, os);
    if (e != ERR_OK) { return e; }
    e = vmem_arena_init(&w->cmd_arena, "world.commands"_id,
                        desc->cmd_arena_reserve != 0u ? desc->cmd_arena_reserve : WORLD_CMD_ARENA_DEFAULT,
                        0u, os);
    if (e != ERR_OK) { return e; }
    e = events_init(&w->events, "world.events0"_id, "world.events1"_id,
                    desc->event_half_reserve != 0u ? desc->event_half_reserve : WORLD_EVENT_HALF_DEFAULT, os);
    if (e != ERR_OK) { return e; }
    e = slotmap_init(&w->entities, "world.entities.slots"_id, "world.entities.gen"_id,
                     "world.entities.free"_id, "world.entities.live"_id, os);
    if (e != ERR_OK) { return e; }

    // tick + seed live in the registered singleton arena (docs/CANON.md); the World member is
    // a pointer into it so restore rewinds them with everything else.
    w->state = (WorldTickState*)arena_push(&w->sing_arena, sizeof(WorldTickState), alignof(WorldTickState));
    w->state->tick = 0;
    w->state->seed = desc->seed;

    // Registration order = the lockstep contract's head (docs/FRAME-LOOP.md §8.2 step 2:
    // world_singletons, then the entity slotmap's four columns; component columns follow as
    // they register).
    registry_add(registry, "world.singletons"_id, &w->sing_arena, ARENA_HASHED | ARENA_SNAPSHOT);
    const u32 ent_flags = ARENA_HASHED | ARENA_SNAPSHOT | ARENA_GROWS_AT_BARRIER;
    registry_add(registry, "world.entities.slots"_id, &w->entities._slots_arena, ent_flags);
    registry_add(registry, "world.entities.gen"_id, &w->entities._gen_arena, ent_flags);
    registry_add(registry, "world.entities.free"_id, &w->entities._free_arena, ent_flags);
    registry_add(registry, "world.entities.live"_id, &w->entities._live_arena, ent_flags);

    schedule_init(&w->sched, &w->meta);
    map_init_fixed(&w->comp_by_name, &w->meta, 2048u);   // 0.75 x 2048 covers the 1024 cap
    map_init_fixed(&w->event_by_name, &w->meta, 512u);
    w->cmds.records_cap = desc->cmd_records_cap != 0u ? desc->cmd_records_cap : WORLD_CMD_RECORDS_DEFAULT;
    w->cmds.payload_cap = desc->cmd_payload_cap != 0u ? desc->cmd_payload_cap : WORLD_CMD_PAYLOAD_DEFAULT;
    return ERR_OK;
}

ComponentId world_register_component(World* w, const ComponentInfo* info) {
    TL_CHECK(info != nullptr);
    TL_CHECK(w->sealed == 0);
    if (w->comp_count == MAX_COMPONENT_TYPES) { TL_FATAL("world: MAX_COMPONENT_TYPES exceeded"); }
    if (map_get(&w->comp_by_name, info->name_hash) != nullptr) {
        TL_FATAL("world: duplicate component name");
    }
    // The reflection walk's pointer ban (docs/MEMORY.md section 0 rule 2): the kind set is
    // closed and pointer-free by construction; registration re-checks the table's integrity.
    for (u32 i = 0; i < info->field_count; ++i) {
        TL_CHECK(info->fields[i].kind < K_COUNT);
        TL_CHECK(info->fields[i].size == kind_scalar_size(info->fields[i].kind) * info->fields[i].count);
    }
    const ComponentId id = w->comp_count;
    ComponentTable* t = &w->comps[id];
    const NameHash id_dense = name_hash_suffix(info->name_hash, ".dense");
    if ((info->flags & COMP_SINGLETON) != 0u) {
        // One zeroed instance in its own small registered arena (docs/ECS.md §2). The column
        // machinery is not used; dense_arena holds the instance.
        TL_CHECK(info->size % info->align == 0);
        TL_CHECK(vmem_arena_init(&t->dense_arena, id_dense, info->size, ARENA_ZERO_ON_PUSH, w->os) == ERR_OK);
        t->info = info;
        t->stride = info->size;
        t->dense = (u8*)arena_push(&t->dense_arena, info->size, info->align);
        t->count = 1;
        registry_add(w->registry, id_dense, &t->dense_arena, ARENA_HASHED | ARENA_SNAPSHOT);
    } else {
        TL_CHECK(column_init(t, info, id_dense, name_hash_suffix(info->name_hash, ".entities"),
                             name_hash_suffix(info->name_hash, ".pages"), w->os) == ERR_OK);
        const u32 col_flags = ARENA_HASHED | ARENA_SNAPSHOT | ARENA_GROWS_AT_BARRIER;
        registry_add(w->registry, id_dense, &t->dense_arena, col_flags);
        registry_add(w->registry, name_hash_suffix(info->name_hash, ".entities"), &t->entity_arena, col_flags);
        // Pages: out of the hash ("SNAPSHOT only", docs/ECS.md §10.3 - derivable from
        // entities[]) but page commits happen inside the apply window, so the guard flag is
        // required for the growth to be legal (world.h contract block).
        registry_add(w->registry, name_hash_suffix(info->name_hash, ".pages"), &t->page_arena,
                     ARENA_SNAPSHOT | ARENA_GROWS_AT_BARRIER);
    }
    map_put(&w->comp_by_name, info->name_hash, (u32)id);
    w->comp_count += 1u;
    return id;
}

EventTypeId world_register_event(World* w, const ComponentInfo* info, u32 cap) {
    TL_CHECK(info != nullptr);
    TL_CHECK(w->sealed == 0);
    const u32 idx = events_register(&w->events, info, cap);
    map_put(&w->event_by_name, info->name_hash, idx);
    return info->name_hash;
}

void world_register_system(World* w, const SystemDesc* desc) {
    TL_CHECK(w->sealed == 0);
    schedule_register(&w->sched, &w->meta, desc);
}

void world_build_schedule(World* w) {
    TL_CHECK(w->sealed == 0);
    schedule_build(&w->sched, w->scratch);
    // One command chunk per schedule position + the external chunk (commands.h).
    w->cmds.chunk_count = w->sched.systems.count + 1u;
    w->cmds.chunks = (CmdChunk*)arena_push(&w->meta, (u64)w->cmds.chunk_count * sizeof(CmdChunk),
                                           alignof(CmdChunk));
    memset(w->cmds.chunks, 0, (usize)w->cmds.chunk_count * sizeof(CmdChunk));
    w->sealed = 1;
}

u32 world_find_component(const World* w, NameHash name) {
    // map_get is non-mutating for a present-or-absent probe; the map member is logically const.
    u32* id = map_get(const_cast<Map<NameHash, u32>*>(&w->comp_by_name), name);
    return id != nullptr ? *id : (u32)MAX_COMPONENT_TYPES;
}

u32 world_find_event(const World* w, NameHash name) {
    u32* idx = map_get(const_cast<Map<NameHash, u32>*>(&w->event_by_name), name);
    return idx != nullptr ? *idx : (u32)MAX_EVENT_TYPES;
}

bool world_entity_alive(const World* w, Entity e) {
    return slotmap_alive(&w->entities, e);
}

void world_events_swap(World* w) {
    events_swap(&w->events);
}

u64 world_reflection_hash(const World* w) {
    // Per-table hashes folded LE in registration order: components, then event types
    // (world.h: both are field tables; docs/BUILD.md §5 wants every one in the fingerprint).
    u64 h = TL_HASH_SEED;
    u8 buf[8];
    for (u32 i = 0; i < w->comp_count; ++i) {
        ByteWriter bw;
        bw_init(&bw, buf, sizeof(buf));
        bw_put_u64(&bw, tl_reflect_component_hash(w->comps[i].info));
        h = tl_hash64(buf, bw.len, h);
    }
    for (u32 i = 0; i < w->events.count; ++i) {
        ByteWriter bw;
        bw_init(&bw, buf, sizeof(buf));
        bw_put_u64(&bw, tl_reflect_component_hash(w->events.t[i].info));
        h = tl_hash64(buf, bw.len, h);
    }
    return h;
}

void world_post_restore(World* w) {
    // Rebuild every derived (non-arena) count from restored arena bytes (docs/MEMORY.md §5's
    // post_restore barrier, the ECS half). Derivations: gen entries are never 0 for allocated
    // slots, so slots.count = the first zero u16; a column's entities[] rows are non-null for
    // live rows and zeroed above count; live_count = popcount; quarantined = dead slots frozen
    // at GEN_MAX; free_list.count = allocated - live - quarantined (its ORDER is arena bytes).
    SlotMap<EntityRecord, Entity>* sm = &w->entities;
    const u32 gen_rows = (u32)(sm->_gen_arena.used / sizeof(u16));
    u32 slot_count = gen_rows;
    for (u32 i = 0; i < gen_rows; ++i) {
        if (((const u16*)(const void*)sm->_gen_arena.base)[i] == 0u) { slot_count = i; break; }
    }
    sm->slots.count = slot_count;
    sm->gen.count = slot_count;
    sm->slots.cap = (u32)(sm->_slots_arena.used / sizeof(EntityRecord));
    sm->gen.cap = (u32)(sm->_gen_arena.used / sizeof(u16));
    sm->free_list.cap = (u32)(sm->_free_arena.used / sizeof(u32));
    u32 live = 0;
    u32 quarantined = 0;
    for (u32 i = 0; i < slot_count; ++i) {
        if (bitset_test(&sm->live, i)) {
            live += 1u;
        } else if (sm->gen.data[i] == (u16)Entity::GEN_MAX) {
            quarantined += 1u;
        }
    }
    sm->live_count = live;
    sm->quarantined = quarantined;
    TL_CHECK(slot_count >= live + quarantined);
    sm->free_list.count = slot_count - live - quarantined;

    for (u32 c = 0; c < w->comp_count; ++c) {
        ComponentTable* t = &w->comps[c];
        if ((t->info->flags & COMP_SINGLETON) != 0u) { continue; }
        const u32 rows = (u32)(t->entity_arena.used / sizeof(Entity));
        u32 count = rows;
        for (u32 i = 0; i < rows; ++i) {
            if (t->entities[i].bits == 0u) { count = i; break; }
        }
        t->count = count;
    }

    // Transients: pending commands and both event halves are cleared (docs/ECS.md §10.4;
    // docs/FRAME-LOOP.md §8.3's rollback sequence).
    commands_discard(w);
    events_clear_all(&w->events);
}
