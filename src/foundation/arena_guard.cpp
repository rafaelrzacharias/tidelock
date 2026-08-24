// arena_guard.cpp - the arena-offset guard. Spec: docs/MEMORY.md §2, §8.4.
// NON-det half on purpose: guard_tick_end reads tl_crt_alloc_count from alloc_shim, an upward
// symbol the det audit forbids (docs/CPP-SUBSET.md §4); the guard is dev-only engine-side
// tooling, never sim-called. Enforcement is one notch stronger than the §8.4 pseudocode:
// GROWS_AT_BARRIER arenas are re-baselined at barrier end, so growth AFTER the window is
// caught too (the §2 semantics - growth is legal only INSIDE the window).
#include "foundation/arena_registry.h"
#include "foundation/alloc_shim.h"

#if TL_DEV

void guard_tick_begin(ArenaGuard* g, const ArenaRegistry* r) {
    TL_CHECK(g != nullptr && r != nullptr);
    for (u32 i = 0; i < r->count; ++i) {
        g->used_at_start[i] = r->e[i].arena->used;
    }
    for (u32 i = r->count; i < MAX_ARENAS; ++i) { g->used_at_start[i] = 0u; }
    g->crt_allocs_at_start = tl_crt_alloc_count();
    g->in_barrier = 0u;
    for (u32 i = 0; i < 7u; ++i) { g->_pad[i] = 0u; }
}

void guard_barrier_begin(ArenaGuard* g, const ArenaRegistry* r) {
    TL_CHECK(g != nullptr && r != nullptr);
    TL_CHECK(g->in_barrier == 0u);
    for (u32 i = 0; i < r->count; ++i) {
        if ((r->e[i].flags & ARENA_GROWS_AT_BARRIER) != 0u &&
            r->e[i].arena->used != g->used_at_start[i]) {
            TL_FATAL("registered arena grew before the barrier window (docs/MEMORY.md section 2)");
        }
    }
    g->in_barrier = 1u;
}

void guard_barrier_end(ArenaGuard* g, const ArenaRegistry* r) {
    TL_CHECK(g != nullptr && r != nullptr);
    TL_CHECK(g->in_barrier == 1u);
    for (u32 i = 0; i < r->count; ++i) {
        if ((r->e[i].flags & ARENA_GROWS_AT_BARRIER) != 0u) {
            g->used_at_start[i] = r->e[i].arena->used;   // window growth is now the baseline
        }
    }
    g->in_barrier = 0u;
}

void guard_tick_end(ArenaGuard* g, const ArenaRegistry* r) {
    TL_CHECK(g != nullptr && r != nullptr);
    TL_CHECK(g->in_barrier == 0u);
    for (u32 i = 0; i < r->count; ++i) {
        if (r->e[i].arena->used != g->used_at_start[i]) {
            TL_FATAL("registered arena moved outside its sanctioned window (docs/MEMORY.md section 2)");
        }
    }
    if (tl_crt_alloc_count() != g->crt_allocs_at_start) {
        TL_FATAL("CRT allocation during a tick - a vendor heap leaked past its pool (docs/MEMORY.md section 8.4)");
    }
}

#endif  // TL_DEV
