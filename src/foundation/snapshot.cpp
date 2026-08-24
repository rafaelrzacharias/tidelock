// snapshot.cpp - the rollback SnapshotRing (T-F-04). Spec: docs/MEMORY.md §8.3.
// Audited det half: memcpy-free bookkeeping only; the blob copies live in arena_registry.cpp.
#include "foundation/snapshot.h"

ErrCode ring_init(SnapshotRing* g, u64 slot_cap_bytes, VMemArena* backing) {
    if (g == nullptr || backing == nullptr || slot_cap_bytes == 0u) {
        return ERR_MEM_BAD_ARG;
    }
    g->head = 0u;
    g->count = 0u;
    for (u32 i = 0; i < CONFIRMATION_HORIZON_TICKS; ++i) {
        Snapshot* s = &g->slot[i];
        for (u32 b = 0; b < 32u; ++b) { s->session_fingerprint[b] = 0u; }
        s->tick = 0u;
        s->count = 0u;
        s->_pad0 = 0u;
        for (u32 u = 0; u < MAX_ARENAS; ++u) { s->used[u] = 0u; }
        s->blob = (u8*)arena_push(backing, slot_cap_bytes, 64u);   // once, never resized here
        s->blob_cap = slot_cap_bytes;
    }
    return ERR_OK;
}

Snapshot* ring_push(SnapshotRing* g, u64 tick) {
    TL_CHECK(g != nullptr);
    TL_CHECK(g->slot[0].blob != nullptr);   // ring_init ran
    Snapshot* s = &g->slot[g->head];
    g->head = (g->head + 1u) % (u32)CONFIRMATION_HORIZON_TICKS;
    if (g->count < (u32)CONFIRMATION_HORIZON_TICKS) { g->count += 1u; }
    s->tick = tick;   // registry_snapshot re-stamps on success and clears on overflow
    return s;
}

const Snapshot* ring_find(const SnapshotRing* g, u64 tick) {
    TL_CHECK(g != nullptr);
    // Newest first: a tick evicted by wrap is simply absent. `count` bounds the walk so a
    // never-pushed slot's default tick 0 is unreachable.
    for (u32 i = 0; i < g->count; ++i) {
        const u32 j = (g->head + (u32)CONFIRMATION_HORIZON_TICKS - 1u - i) % (u32)CONFIRMATION_HORIZON_TICKS;
        const Snapshot* s = &g->slot[j];
        if (s->tick == tick && s->count != 0u) {
            return s;
        }
    }
    return nullptr;
}
