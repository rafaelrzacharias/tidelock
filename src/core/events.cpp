// events.cpp - the two-half event arena. Spec: docs/ECS.md §10.4; contract in events.h.
#include "core/events.h"
#include <string.h>

ErrCode events_init(EventTables* ev, NameHash id_half0, NameHash id_half1, u64 reserve_bytes,
                    const VMemApi* os) {
    memset(ev, 0, sizeof(*ev));
    ErrCode e = vmem_arena_init(&ev->half[0], id_half0, reserve_bytes, 0u, os);
    if (e != ERR_OK) { return e; }
    e = vmem_arena_init(&ev->half[1], id_half1, reserve_bytes, 0u, os);
    if (e != ERR_OK) { return e; }
    ev->write_half = 0;
    ev->count = 0;
    return ERR_OK;
}

u32 events_register(EventTables* ev, const ComponentInfo* info, u32 cap) {
    TL_CHECK(info != nullptr && info->size != 0);
    TL_CHECK(info->size % info->align == 0);   // stride == size, same premise as columns
    if (ev->count == MAX_EVENT_TYPES) { TL_FATAL("events: MAX_EVENT_TYPES exceeded"); }
    if (events_find(ev, info->name_hash) != MAX_EVENT_TYPES) {
        TL_FATAL("events: duplicate event type name");
    }
    if (cap == 0u) { cap = EVENT_DEFAULT_CAP; }
    EventTable* table = &ev->t[ev->count];
    table->info = info;
    table->stride = info->size;
    table->cap = cap;
    table->_pad0 = 0;
    table->block[0] = (u8*)arena_push(&ev->half[0], (u64)cap * table->stride, info->align);
    table->block[1] = (u8*)arena_push(&ev->half[1], (u64)cap * table->stride, info->align);
    table->write = table->block[ev->write_half];
    table->read = table->block[ev->write_half ^ 1u];
    table->write_count = 0;
    table->read_count = 0;
    ev->count += 1u;
    return ev->count - 1u;
}

u32 events_find(const EventTables* ev, EventTypeId id) {
    for (u32 i = 0; i < ev->count; ++i) {
        if (ev->t[i].info->name_hash == id) { return i; }
    }
    return MAX_EVENT_TYPES;
}

void events_emit(EventTables* ev, u32 index, const void* e) {
    TL_CHECK(index < ev->count && e != nullptr);
    EventTable* table = &ev->t[index];
    TL_CHECK(table->write_count < table->cap);   // an overflow is a bug, not a drop
    memcpy(table->write + (u64)table->write_count * table->stride, e, table->info->size);
    table->write_count += 1u;
}

EventSlice events_read(const EventTables* ev, u32 index) {
    TL_CHECK(index < ev->count);
    const EventTable* table = &ev->t[index];
    return EventSlice{ table->read, table->read_count, table->stride };
}

void events_swap(EventTables* ev) {
    const u32 other = ev->write_half ^ 1u;
    for (u32 i = 0; i < ev->count; ++i) {
        EventTable* table = &ev->t[i];
        table->read = table->write;
        table->read_count = table->write_count;
        table->write = table->block[other];
        table->write_count = 0;
    }
    ev->write_half = other;
}

void events_clear_all(EventTables* ev) {
    for (u32 i = 0; i < ev->count; ++i) {
        ev->t[i].write_count = 0;
        ev->t[i].read_count = 0;
    }
}
