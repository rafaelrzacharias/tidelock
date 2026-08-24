// snapshot.cpp - Snapshot + SnapshotRing. Spec: docs/MEMORY.md section 8.3.
// HEADER-FIRST STUB (docs/ROADMAP.md section 0 rule 1).
#include "foundation/snapshot.h"

ErrCode ring_init(SnapshotRing*, u64, VMemArena*) {
    TL_FATAL("unimplemented: ring_init (w1-mem, docs/MEMORY.md section 8.3)");
}

Snapshot* ring_push(SnapshotRing*, u64) {
    TL_FATAL("unimplemented: ring_push (w1-mem, docs/MEMORY.md section 8.3)");
}

const Snapshot* ring_find(const SnapshotRing*, u64) {
    TL_FATAL("unimplemented: ring_find (w1-mem, docs/MEMORY.md section 8.3)");
}
