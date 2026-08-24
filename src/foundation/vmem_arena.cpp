// vmem_arena.cpp - VMemArena implementation. Spec: docs/MEMORY.md section 8.2.
// HEADER-FIRST STUB (docs/ROADMAP.md section 0 rule 1): every entry is TL_FATAL until the
// implementation slice lands; dependent lanes compile against vmem_arena.h from this commit.
#include "foundation/vmem_arena.h"

ErrCode vmem_arena_init(VMemArena*, NameHash, u64, u32, const VMemApi*) {
    TL_FATAL("unimplemented: vmem_arena_init (w1-mem, docs/MEMORY.md section 8.2)");
}

void* arena_push(VMemArena*, u64, u32) {
    TL_FATAL("unimplemented: arena_push (w1-mem, docs/MEMORY.md section 8.2)");
}

void arena_reset_to(VMemArena*, u64) {
    TL_FATAL("unimplemented: arena_reset_to (w1-mem, docs/MEMORY.md section 8.2)");
}

void arena_decommit_above(VMemArena*, u64) {
    TL_FATAL("unimplemented: arena_decommit_above (w1-mem, docs/MEMORY.md section 8.2)");
}
