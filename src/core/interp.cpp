// interp.cpp - the end-of-tick barrier's step 3: prev <- current ping-pong, plus the explicit
//   per-entity snap for teleports/camera cuts (docs/FRAME-LOOP.md §3, §4). See loop.h for the
//   registration API (interp_register_pair) and this file's contract note on why the mechanism
//   is generic (the concrete columns are render2d's, not yet landed).
#include "core/loop.h"
#include "core/column.h"
#include <string.h>

void interp_register_pair(Engine* e, ComponentId current, ComponentId prev) {
    if (e->ticked != 0u) { TL_FATAL("interp_register_pair: called after the first tick (init only)"); }
    if (e->interp_pair_count >= INTERP_MAX_PAIRS) { TL_FATAL("interp_register_pair: INTERP_MAX_PAIRS exceeded"); }
    for (u32 i = 0; i < e->interp_pair_count; ++i) {
        if (e->interp_pairs[i].current == current) { TL_FATAL("interp_register_pair: duplicate current id"); }
    }
    e->interp_pairs[e->interp_pair_count] = InterpPair{ current, prev };
    e->interp_pair_count += 1u;
}

void interp_pingpong(World* w, const InterpPair* pairs, u32 pair_count) {
    for (u32 i = 0; i < pair_count; ++i) {
        ComponentTable* cur = &w->comps[pairs[i].current];
        ComponentTable* prv = &w->comps[pairs[i].prev];
        TL_CHECK(cur->stride == prv->stride);
        for (u32 d = 0; d < cur->count; ++d) {
            const Entity ent = cur->entities[d];
            void* prev_row = column_get(prv, ent);
            TL_CHECK(prev_row != nullptr);   // the two columns of a pair are added/removed together (caller's contract)
            // review round 2 defect 3: "added/removed together" bounds PRESENCE, not DENSE ORDER -
            // column_remove is swap-remove (column.cpp), so a column's own dense order is a function
            // of its individual add/remove history, not registration order. A consumer that pairs
            // `cur`/`prev` BY DENSE INDEX instead of by entity (as render/extract.cpp does, main)
            // would silently smear entity `ent`'s current pose against a DIFFERENT entity's previous
            // one the moment the two columns' dense orders diverge - equal `count` still holds, so
            // that consumer's only guard passes. This function itself looks up `prev_row` by entity,
            // so it is not itself broken by the divergence - but it is the one place that CAN see
            // it happen, so it fails loudly here rather than let it propagate silently downstream.
            TL_CHECK(prv->entities[d].bits == ent.bits);
            memcpy(prev_row, cur->dense + (u64)d * cur->stride, cur->stride);
        }
    }
}

void interp_snap_entity(World* w, const InterpPair* pairs, u32 pair_count, Entity e) {
    for (u32 i = 0; i < pair_count; ++i) {
        ComponentTable* cur = &w->comps[pairs[i].current];
        ComponentTable* prv = &w->comps[pairs[i].prev];
        void* cur_row = column_get(cur, e);
        if (cur_row == nullptr) { continue; }   // not every pair need be present on every entity
        void* prev_row = column_get(prv, e);
        TL_CHECK(prev_row != nullptr);
        memcpy(prev_row, cur_row, cur->stride);
    }
}
