// arena_registry.cpp - the registered arena set. Spec: docs/MEMORY.md §8.3.
// Audited det half: no io - the R-2 dev overflow is a returned code, the caller logs.
#include "foundation/arena_registry.h"
#include "foundation/snapshot.h"
#include "foundation/hash.h"

#include <string.h>  // memcpy/memcmp/memset (docs/CPP-SUBSET.md §1 allowlist)

namespace mem {

static inline u64 align_up_64b(u64 v) { return (v + 63u) & ~(u64)63u; }

// Commit-on-demand for a restore target, the same granule policy as arena_push: a restore may
// land in a fresh world (rejoin) whose arenas have never committed a page. Duplicated from
// arena_push on purpose - "ensure committed without moving used" is not a public arena verb.
static void ensure_committed(VMemArena* a, u64 bytes) {
    if (bytes <= a->committed) { return; }
    const u64 want = (bytes + (u64)COMMIT_GRANULE - 1u) & ~(u64)(COMMIT_GRANULE - 1u);
    TL_CHECK(want <= a->reserved);
    const ErrCode e = a->os->commit(a->os->ctx, a->base + a->committed, want - a->committed);
    if (e != ERR_OK) { TL_FATAL("vmem commit failed during restore (docs/PLATFORM.md section 9.3)"); }
    a->committed = want;
}

}  // namespace mem

void registry_add(ArenaRegistry* r, NameHash id, VMemArena* a, u32 flags) {
    TL_CHECK(r != nullptr && a != nullptr);
    if (r->sealed != 0u) {
        TL_FATAL("registry_add after seal - registration order is the lockstep contract (docs/MEMORY.md section 1.2)");
    }
    if (r->count >= MAX_ARENAS) {
        TL_FATAL("MAX_ARENAS exceeded (docs/CANON.md)");
    }
    for (u32 i = 0; i < r->count; ++i) {
        if (r->e[i].id == id) { TL_FATAL("duplicate arena id - ids key the registry"); }
    }
    r->e[r->count].id = id;
    r->e[r->count].arena = a;
    r->e[r->count].flags = flags;
    r->e[r->count]._pad0 = 0u;
    r->count += 1u;
}

void registry_seal(ArenaRegistry* r) {
    TL_CHECK(r != nullptr);
    TL_CHECK(r->sealed == 0u);
    r->sealed = 1u;
}

void registry_set_fingerprint(ArenaRegistry* r, const u8 fingerprint[32]) {
    TL_CHECK(r != nullptr && fingerprint != nullptr);
    if (r->sealed == 0u) {
        TL_FATAL("set_fingerprint before seal - the fingerprint folds the sealed ids (docs/BUILD.md section 5)");
    }
    memcpy(r->session_fingerprint, fingerprint, 32u);
}

u64 registry_hash_all(const ArenaRegistry* r, u64 out_per_arena[MAX_ARENAS]) {
    TL_CHECK(r != nullptr && out_per_arena != nullptr);
    TL_CHECK(r->sealed != 0u);   // the fold order below IS the lockstep contract
    for (u32 i = 0; i < r->count; ++i) {
        const ArenaEntry* e = &r->e[i];
        out_per_arena[i] = (e->flags & ARENA_HASHED) != 0u
                               ? tl_hash64(e->arena->base, e->arena->used, TL_HASH_SEED)
                               : 0u;
    }
    return tl_hash64(out_per_arena, (usize)r->count * 8u, TL_HASH_SEED);
}

ErrCode registry_snapshot(const ArenaRegistry* r, Snapshot* s, u64 tick) {
    TL_CHECK(r != nullptr && s != nullptr && s->blob != nullptr);
    TL_CHECK(r->sealed != 0u);

    // Size pass first, so an overflow refuses BEFORE any byte moves (no partial snapshots).
    u64 need = 0u;
    for (u32 i = 0; i < r->count; ++i) {
        if ((r->e[i].flags & ARENA_SNAPSHOT) != 0u) {
            need = mem::align_up_64b(need) + r->e[i].arena->used;
        }
    }
    if (need > s->blob_cap) {
        // Budget violation (docs/MEMORY.md section 7 R-2). Invalidate the slot so a stale blob
        // cannot be found under the new tick, then fail per tier.
        s->tick = 0u;
        s->count = 0u;
#if TL_DEV
        return ERR_MEM_RING_OVERFLOW;   // the caller warns once and grows at the next barrier
#else
        TL_FATAL("snapshot blob over budget - lockstep peers must agree on limits (docs/MEMORY.md section 7 R-2)");
#endif
    }

    u64 off = 0u;
    for (u32 i = 0; i < r->count; ++i) {
        const VMemArena* a = r->e[i].arena;
        s->used[i] = a->used;   // recorded for every entry (diagnostics); only SNAPSHOT restore
        if ((r->e[i].flags & ARENA_SNAPSHOT) != 0u) {
            off = mem::align_up_64b(off);
            memcpy(s->blob + off, a->base, a->used);
            off += a->used;
        }
    }
    for (u32 i = r->count; i < MAX_ARENAS; ++i) { s->used[i] = 0u; }
    memcpy(s->session_fingerprint, r->session_fingerprint, 32u);
    s->tick = tick;
    s->count = r->count;
    s->_pad0 = 0u;
    return ERR_OK;
}

ErrCode registry_restore(ArenaRegistry* r, const Snapshot* s) {
    TL_CHECK(r != nullptr && s != nullptr && s->blob != nullptr);
    TL_CHECK(r->sealed != 0u);

    // Fail-loud gate, nothing touched on mismatch. The ids themselves are not stored in the
    // snapshot: they fold into session_fingerprint (docs/BUILD.md section 5), so the fingerprint
    // check IS the id check; count + registry order pin the blob layout.
    if (s->count != r->count ||
        memcmp(s->session_fingerprint, r->session_fingerprint, 32u) != 0) {
        return ERR_SNAPSHOT_MISMATCH;
    }

    u64 off = 0u;
    for (u32 i = 0; i < r->count; ++i) {
        if ((r->e[i].flags & ARENA_SNAPSHOT) == 0u) { continue; }
        VMemArena* a = r->e[i].arena;
        const u64 want = s->used[i];
        TL_CHECK(want <= a->reserved);
        mem::ensure_committed(a, want);
        off = mem::align_up_64b(off);
        memcpy(a->base, s->blob + off, want);
        off += want;
        a->used = want;
        if (a->high_water < want) { a->high_water = want; }
    }
    return ERR_OK;
}
