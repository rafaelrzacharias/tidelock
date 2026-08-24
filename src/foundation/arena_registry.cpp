// arena_registry.cpp - the registered arena set. Spec: docs/MEMORY.md section 8.3.
// HEADER-FIRST STUB (docs/ROADMAP.md section 0 rule 1). registry_hash_all additionally waits on
// foundation/hash.h (tl_hash64, the w1-rng-hash lane) - TODO.md, W1 mem notes.
#include "foundation/arena_registry.h"
#include "foundation/snapshot.h"

void registry_add(ArenaRegistry*, NameHash, VMemArena*, u32) {
    TL_FATAL("unimplemented: registry_add (w1-mem, docs/MEMORY.md section 8.3)");
}

void registry_seal(ArenaRegistry*) {
    TL_FATAL("unimplemented: registry_seal (w1-mem, docs/MEMORY.md section 8.3)");
}

void registry_set_fingerprint(ArenaRegistry*, const u8[32]) {
    TL_FATAL("unimplemented: registry_set_fingerprint (w1-mem, docs/MEMORY.md section 8.3)");
}

u64 registry_hash_all(const ArenaRegistry*, u64[MAX_ARENAS]) {
    TL_FATAL("unimplemented: registry_hash_all (waits on w1-rng-hash tl_hash64 - TODO.md W1 mem notes)");
}

ErrCode registry_snapshot(const ArenaRegistry*, Snapshot*, u64) {
    TL_FATAL("unimplemented: registry_snapshot (w1-mem, docs/MEMORY.md section 8.3)");
}

ErrCode registry_restore(ArenaRegistry*, const Snapshot*) {
    TL_FATAL("unimplemented: registry_restore (w1-mem, docs/MEMORY.md section 8.3)");
}
