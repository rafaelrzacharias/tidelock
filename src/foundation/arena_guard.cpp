// arena_guard.cpp - the arena-offset guard. Spec: docs/MEMORY.md section 8.4.
// Lives in the NON-det half: guard_tick_end reads tl_crt_alloc_count from alloc_shim, an
// upward symbol the det audit forbids (docs/CPP-SUBSET.md section 4); the guard is dev-only
// engine-side tooling, never sim-called. HEADER-FIRST STUB (docs/ROADMAP.md section 0 rule 1).
#include "foundation/arena_registry.h"
#include "foundation/alloc_shim.h"

#if TL_DEV

void guard_tick_begin(ArenaGuard*, const ArenaRegistry*) {
    TL_FATAL("unimplemented: guard_tick_begin (w1-mem, docs/MEMORY.md section 8.4)");
}

void guard_barrier_begin(ArenaGuard*, const ArenaRegistry*) {
    TL_FATAL("unimplemented: guard_barrier_begin (w1-mem, docs/MEMORY.md section 8.4)");
}

void guard_barrier_end(ArenaGuard*, const ArenaRegistry*) {
    TL_FATAL("unimplemented: guard_barrier_end (w1-mem, docs/MEMORY.md section 8.4)");
}

void guard_tick_end(ArenaGuard*, const ArenaRegistry*) {
    TL_FATAL("unimplemented: guard_tick_end (w1-mem, docs/MEMORY.md section 8.4)");
}

#endif  // TL_DEV
