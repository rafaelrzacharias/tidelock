// commands.cpp - recording and the barrier applier. Spec: docs/ECS.md §4/§10.5; contracts in
// commands.h and world.h (the recording API is world_*'s surface).
#include "core/commands.h"
#include "core/world.h"
#include "core/cvar.h"
#include <string.h>

namespace {

// The recording chunk for the current context: the running system's SCHEDULE position, or the
// external chunk (editor/app - the last slot). Activates the chunk's arrays on first use.
CmdChunk* cmd_chunk_for_context(World* w) {
    TL_CHECK(w->sealed == 1);   // recording needs the chunk table (world_build_schedule)
    u32 slot;
    if (w->sched.running.index != RUNNING_NONE) {
        slot = w->sched.systems.data[w->sched.running.index].phase_pos;
    } else {
        slot = w->cmds.chunk_count - 1u;
    }
    CmdChunk* c = &w->cmds.chunks[slot];
    if (c->active == 0u) {
        array_init_fixed(&c->recs, &w->cmd_arena, w->cmds.records_cap);
        array_init_fixed(&c->payload, &w->cmd_arena, w->cmds.payload_cap);
        c->chunk_id = slot;
        c->active = 1;
    }
    return c;
}

// Appends one record + a two-segment payload (TL_FATAL on either cap - a blown budget is a
// bug). Two segments so CMD_SET_FIELD's header + bytes need no staging copy anywhere.
void cmd_record2(World* w, CmdKind kind, Entity e, u16 comp,
                 const void* p1, u32 l1, const void* p2, u32 l2) {
    CmdChunk* c = cmd_chunk_for_context(w);
    TL_CHECK(l1 + l2 <= c->payload.cap - c->payload.count);
    CmdRecord rec;
    rec.kind = kind;
    rec._pad0 = 0;
    rec.comp = comp;
    rec.e = e;
    rec.payload_off = c->payload.count;
    rec.payload_len = l1 + l2;
    if (l1 != 0u) { memcpy(c->payload.data + c->payload.count, p1, l1); c->payload.count += l1; }
    if (l2 != 0u) { memcpy(c->payload.data + c->payload.count, p2, l2); c->payload.count += l2; }
    array_push(&c->recs, rec);
}

// The single-segment spelling every other recorder uses.
void cmd_record(World* w, CmdKind kind, Entity e, u16 comp, const void* payload, u32 len) {
    cmd_record2(w, kind, e, comp, payload, len, nullptr, 0u);
}

// CMD_SPAWN_REALIZE's applier: commits the reserved id into the slotmap (world.h E-3 design).
// Fresh ids realize once each but NOT necessarily in reservation order - the external chunk
// records first and applies last (review 1 D2) - so a realize pushes zero slots up to its own
// idx and a skipped-over id's later realize finds its slot already pushed (gen 1, dead). A
// gen-1 handle is fresh by construction: a freed slot's gen is bumped at remove (>= 2) and a
// quarantined slot never reissues, so gen 1 exists only on fresh reservations.
void apply_spawn_realize(World* w, Entity e) {
    SlotMap<EntityRecord, Entity>* sm = &w->entities;
    const u32 idx = handle_index(e);
    const u32 gen = handle_gen(e);
    if (gen == 1u && idx >= sm->slots.count) {
        while (sm->slots.count <= idx) {
            EntityRecord zero = { 0u, 0u };
            array_push(&sm->slots, zero);
            array_push(&sm->gen, (u16)1);
        }
    }
    TL_CHECK(idx < sm->slots.count);
    TL_CHECK(sm->gen.data[idx] == (u16)gen);   // the reserved generation is still current
    TL_CHECK(!bitset_test(&sm->live, idx));
    bitset_set(&sm->live, idx);
    sm->live_count += 1u;
    if (gen == 1u) {                            // one decrement per fresh reservation, any order
        TL_CHECK(w->pending_fresh > 0u);
        w->pending_fresh -= 1u;
    }
}

// True iff e is a reservation of THIS window awaiting its realize (review 1 D4): a fresh id
// (gen 1 - only fresh reservations carry it), or a dead gen-matching slot WHOSE ENTRY THE
// WINDOW'S DRAIN POPPED. The membership requirement is review 2 R1: a stale handle to a
// QUARANTINED slot is also dead with gen.data[idx] == handle_gen (the gen froze at GEN_MAX -
// slotmap.h: "only the live bit separates it from its last handle"), and it must stay the
// normal no-op stale flow. gen != GEN_MAX cannot separate them either - a free-list slot
// legitimately sits at GEN_MAX (bumped from GEN_MAX-1, then pushed), so a pending last-gen
// reservation would lose its D4 protection; membership in the popped set is exact.
bool spawn_pending(const World* w, Entity e) {
    const SlotMap<EntityRecord, Entity>* sm = &w->entities;
    const u32 idx = handle_index(e);
    const u32 gen = handle_gen(e);
    if (gen == 1u && idx >= sm->slots.count) { return true; }
    if (idx >= sm->slots.count) { return false; }
    if (bitset_test(&sm->live, idx) || sm->gen.data[idx] != (u16)gen) { return false; }
    if (gen == 1u) { return true; }   // a loop-pushed fresh slot awaiting its own realize
    for (u32 i = 0; i < w->window_popped_count; ++i) {
        if (w->window_popped[i] == idx) { return true; }
    }
    return false;
}

// CMD_DESTROY's applier: a dead target is the normal stale-reference flow (no-op) UNLESS it is
// a reserved-but-unrealized entity - its realize applies later in this window and would
// resurrect a destroy we silently dropped, so that order is loud (review 1 D4; the policy
// ruling is E-4's, filed with add-after-destroy). A live one leaves every column it was in
// (walk all tables - docs/ECS.md §10.5), then frees the slot.
void apply_destroy(World* w, Entity e) {
    if (!slotmap_alive(&w->entities, e)) {
        if (spawn_pending(w, e)) {
            TL_FATAL("commands: destroy of a reserved-but-unrealized entity (TODO.md E-4)");
        }
        return;
    }
    for (u32 c = 0; c < w->comp_count; ++c) {
        ComponentTable* t = &w->comps[c];
        if ((t->info->flags & COMP_SINGLETON) != 0u) { continue; }
        if (column_get(t, e) != nullptr) { column_remove(t, e); }
    }
    TL_CHECK(slotmap_remove(&w->entities, e));
}

}  // namespace

Entity world_spawn(World* w) {
    // Reservation without touching ANY registered byte (TODO.md E-3, review 1 D3): the free
    // list is READ through the reserved_free cursor - the actual pops apply at the next
    // window's start, so a snapshot captured with a reservation outstanding holds bytes its
    // derived counts still agree with. A fresh id is the next count + pending.
    SlotMap<EntityRecord, Entity>* sm = &w->entities;
    u32 idx;
    u32 gen;
    if (w->reserved_free < sm->free_list.count) {
        idx = sm->free_list.data[sm->free_list.count - 1u - w->reserved_free];
        w->reserved_free += 1u;
        gen = sm->gen.data[idx];
    } else {
        const u64 fresh = (u64)sm->slots.count + w->pending_fresh;
        if (fresh > (u64)Entity::IDX_MASK) { TL_FATAL("world: entity domain exhausted"); }
        idx = (u32)fresh;
        gen = 1u;
        w->pending_fresh += 1u;
    }
    const Entity e = handle_make<Entity>(idx, gen);
    cmd_record(w, CMD_SPAWN_REALIZE, e, 0u, nullptr, 0u);
    return e;
}

void world_destroy(World* w, Entity e) {
    TL_CHECK(!handle_is_null(e));
    cmd_record(w, CMD_DESTROY, e, 0u, nullptr, 0u);
}

void world_add_raw(World* w, Entity e, ComponentId comp, const void* value) {
    TL_CHECK(!handle_is_null(e) && comp < w->comp_count && value != nullptr);
    const ComponentInfo* info = w->comps[comp].info;
    TL_CHECK((info->flags & COMP_SINGLETON) == 0u);
    cmd_record(w, CMD_ADD, e, comp, value, info->size);
}

void world_remove(World* w, Entity e, ComponentId comp) {
    TL_CHECK(!handle_is_null(e) && comp < w->comp_count);
    TL_CHECK((w->comps[comp].info->flags & COMP_SINGLETON) == 0u);
    cmd_record(w, CMD_REMOVE, e, comp, nullptr, 0u);
}

void world_set_field_cmd(World* w, Entity e, ComponentId comp, u32 field_index,
                         const void* bytes, u32 len) {
    TL_CHECK(comp < w->comp_count && bytes != nullptr);
    const ComponentInfo* info = w->comps[comp].info;
    TL_CHECK(field_index < info->field_count);
    TL_CHECK(len == info->fields[field_index].size);
    // Payload = LE u32 field index, then the field's bytes (commands.h record contract).
    u8 head[4];
    ByteWriter bw;
    bw_init(&bw, head, sizeof(head));
    bw_put_u32(&bw, field_index);
    cmd_record2(w, CMD_SET_FIELD, e, comp, head, 4u, bytes, len);
}

void world_singleton_set_cmd(World* w, ComponentId comp, const void* value) {
    TL_CHECK(comp < w->comp_count && value != nullptr);
    const ComponentInfo* info = w->comps[comp].info;
    TL_CHECK((info->flags & COMP_SINGLETON) != 0u);
    cmd_record(w, CMD_SINGLETON_SET, Entity{ 0 }, comp, value, info->size);
}

// Ruled exception (docs/ROADMAP.md §0 rule 2, RR-33/RR-35, commands.h's CMD_SET_CVAR comment).
void world_set_cvar_cmd(World* w, NameHash key, u32 bits) {
    TL_CHECK(w->cvars != nullptr);
    const CvarDesc* d = cvar_find(w->cvars, key);
    TL_CHECK(d != nullptr && (d->flags & CVAR_SIM) != 0u);
    // Payload = LE NameHash key, then LE u32 bits (commands.h's CMD_SET_CVAR record contract).
    u8 head[8];
    ByteWriter bw;
    bw_init(&bw, head, sizeof(head));
    bw_put_u64(&bw, key);
    cmd_record2(w, CMD_SET_CVAR, Entity{ 0 }, 0u, head, 8u, &bits, 4u);
}

void world_flush(World* w) {
    apply_commands(w);
}

void apply_commands(World* w) {
    if (w->guard != nullptr) { guard_barrier_begin(w->guard, w->registry); }
    // The window's deferred free-list pops land first, in reservation order - byte-identical
    // to the recording-time pops the rev-1 design performed, but inside the barrier window
    // (review 1 D3). Destroys applying below may push freed slots back afterwards as usual.
    // The popped idxs are kept on the command arena for spawn_pending's membership check
    // (review 2 R1); the arena reset below ends their lifetime with the window.
    if (w->reserved_free != 0u) {
        w->window_popped = (u32*)arena_push(&w->cmd_arena, (u64)w->reserved_free * sizeof(u32),
                                            alignof(u32));
        while (w->reserved_free != 0u) {
            w->window_popped[w->window_popped_count] = array_pop(&w->entities.free_list);
            w->window_popped_count += 1u;
            w->reserved_free -= 1u;
        }
    }
    for (u32 ci = 0; ci < w->cmds.chunk_count; ++ci) {
        CmdChunk* c = &w->cmds.chunks[ci];
        if (c->active == 0u) { continue; }
        for (u32 ri = 0; ri < c->recs.count; ++ri) {
            const CmdRecord* rec = &c->recs.data[ri];
            const u8* payload = c->payload.data + rec->payload_off;
            switch (rec->kind) {
                case CMD_SPAWN_REALIZE: {
                    apply_spawn_realize(w, rec->e);
                    break;
                }
                case CMD_DESTROY: {
                    apply_destroy(w, rec->e);
                    break;
                }
                case CMD_ADD: {
                    TL_CHECK(slotmap_alive(&w->entities, rec->e));   // add-after-destroy: TODO.md E-4
                    ComponentTable* t = &w->comps[rec->comp];
                    TL_CHECK(rec->payload_len == t->info->size);
                    column_add(t, rec->e, payload);
                    EntityRecord* er = slotmap_get(&w->entities, rec->e);
                    er->comp_count += 1u;
                    break;
                }
                case CMD_REMOVE: {
                    TL_CHECK(slotmap_alive(&w->entities, rec->e));
                    ComponentTable* t = &w->comps[rec->comp];
                    TL_CHECK(column_get(t, rec->e) != nullptr);   // removing an absent component is a bug
                    column_remove(t, rec->e);
                    EntityRecord* er = slotmap_get(&w->entities, rec->e);
                    TL_CHECK(er->comp_count > 0u);
                    er->comp_count -= 1u;
                    break;
                }
                case CMD_SET_FIELD: {
                    ComponentTable* t = &w->comps[rec->comp];
                    ByteReader br;
                    br_init(&br, payload, rec->payload_len);
                    const u32 fi = br_get_u32(&br);
                    TL_CHECK(br_ok(&br) && fi < t->info->field_count);
                    const FieldInfo* f = &t->info->fields[fi];
                    TL_CHECK(rec->payload_len == f->size + 4u);
                    u8* row;
                    if ((t->info->flags & COMP_SINGLETON) != 0u) {
                        row = t->dense;
                    } else {
                        TL_CHECK(slotmap_alive(&w->entities, rec->e));
                        row = (u8*)column_get(t, rec->e);
                        TL_CHECK(row != nullptr);
                    }
                    memcpy(row + f->offset, payload + 4u, f->size);
                    break;
                }
                case CMD_SINGLETON_SET: {
                    ComponentTable* t = &w->comps[rec->comp];
                    TL_CHECK((t->info->flags & COMP_SINGLETON) != 0u);
                    TL_CHECK(rec->payload_len == t->info->size);
                    memcpy(t->dense, payload, t->info->size);
                    break;
                }
                case CMD_SET_CVAR: {
                    // Ruled exception (RR-33/RR-35, commands.h's CMD_SET_CVAR comment).
                    TL_CHECK(rec->payload_len == 12u && w->cvars != nullptr);
                    ByteReader br;
                    br_init(&br, payload, rec->payload_len);
                    const NameHash key = br_get_u64(&br);
                    const u32 bits = br_get_u32(&br);
                    TL_CHECK(br_ok(&br));
                    TL_CHECK(cvar_apply_sim_raw(w->cvars, key, bits) == ERR_OK);
                    break;
                }
                default: {
                    // CMD_ALLOY / reload / asset kinds: their consumers land with their lanes
                    // (commands.h); meeting one now is a wiring bug, never a silent skip.
                    TL_FATAL("commands: unwired command kind");
                }
            }
        }
        c->active = 0;
        c->recs = Array<CmdRecord>{ nullptr, 0, 0, nullptr };
        c->payload = Array<u8>{ nullptr, 0, 0, nullptr };
    }
    w->window_popped = nullptr;
    w->window_popped_count = 0;
    arena_reset_to(&w->cmd_arena, 0u);
    TL_ASSERT(w->pending_fresh == 0u);   // every fresh reservation realized this window
    if (w->guard != nullptr) { guard_barrier_end(w->guard, w->registry); }
}

void commands_discard(World* w) {
    // Reservations are cursors, not byte moves (review 1 D3), so a discard is clean on its
    // own: nothing was mutated between recording and the barrier. The rollback path calls it
    // before registry_restore (docs/FRAME-LOOP.md §8.3).
    for (u32 ci = 0; ci < w->cmds.chunk_count; ++ci) {
        CmdChunk* c = &w->cmds.chunks[ci];
        c->active = 0;
        c->recs = Array<CmdRecord>{ nullptr, 0, 0, nullptr };
        c->payload = Array<u8>{ nullptr, 0, 0, nullptr };
    }
    arena_reset_to(&w->cmd_arena, 0u);
    w->pending_fresh = 0;
    w->reserved_free = 0;
    w->window_popped = nullptr;
    w->window_popped_count = 0;
}
