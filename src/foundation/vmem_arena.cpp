// vmem_arena.cpp - VMemArena implementation. Spec: docs/MEMORY.md §8.2.
// Audited det half: no io, no OS symbol - every syscall goes through the injected VMemApi.
#include "foundation/vmem_arena.h"

#include <string.h>  // memset (docs/CPP-SUBSET.md §1 allowlist)

namespace mem {

// `a` must be a power of two; the sum cannot wrap for any in-reserve offset (reserves are far
// below 2^63 and callers TL_CHECK their extents before arithmetic reaches here).
static inline u64 align_up_u64(u64 v, u64 a) { return (v + (a - 1u)) & ~(a - 1u); }

}  // namespace mem

ErrCode vmem_arena_init(VMemArena* a, NameHash id, u64 reserve_bytes, u32 flags, const VMemApi* os) {
    if (a == nullptr || os == nullptr || os->reserve == nullptr || os->commit == nullptr ||
        reserve_bytes == 0u) {
        return ERR_MEM_BAD_ARG;
    }
    const u64 page = (u64)os->page_size;
    TL_CHECK(page != 0u && (page & (page - 1u)) == 0u);
    // Both are powers of two, so page <= COMMIT_GRANULE is exactly "page divides COMMIT_GRANULE",
    // which is what makes the granule rounding below also a page rounding.
    TL_CHECK(page <= (u64)COMMIT_GRANULE);

    // Rounded up to COMMIT_GRANULE, not to a page (ruled 2026-08-24, TODO.md): arena_push commits
    // in granule multiples and TL_FATALs when align_up(end, COMMIT_GRANULE) > reserved, so a
    // page-rounded reserve made the usable budget round_down(reserved, COMMIT_GRANULE) - every
    // reserve below 64 KB could never push a single byte, and the tail of a non-multiple reserve
    // was unreachable. Address space is free; the stated budget is now the usable budget and the
    // over-reserve fatal coincides with the real edge.
    const u64 reserve = mem::align_up_u64(reserve_bytes, (u64)COMMIT_GRANULE);
    void* base = os->reserve(os->ctx, reserve);
    if (base == nullptr) {
        return ERR_MEM_OOM;
    }
    a->base       = (u8*)base;
    a->reserved   = reserve;
    a->committed  = 0u;
    a->used       = 0u;
    a->high_water = 0u;
    a->page       = os->page_size;
    a->flags      = flags;
    a->id         = id;
    a->os         = os;
    return ERR_OK;
}

void* arena_push(VMemArena* a, u64 bytes, u32 align) {
    TL_ASSERT(a != nullptr && a->base != nullptr);
    TL_ASSERT(align != 0u && (align & (align - 1u)) == 0u && align <= a->page);

    const u64 start = mem::align_up_u64(a->used, (u64)align);
    const u64 end   = start + bytes;
    TL_CHECK(end >= start);  // a wrapped extent is a bug, not a big allocation

    if (end > a->committed) {
        // Checked on `end` BEFORE the granule rounding: align_up of an end within one granule
        // of 2^64 wraps to a small value and would sail past a want-only check with no fatal
        // (W1 mem review 3). With end <= reserved (reserves are far below 2^63) the rounding
        // below cannot wrap.
        if (end > a->reserved) {
            TL_FATAL("arena over reserve - a blown budget is a bug, not silent growth (docs/MEMORY.md section 1.1)");
        }
        const u64 want = mem::align_up_u64(end, (u64)COMMIT_GRANULE);
        if (want > a->reserved) {
            TL_FATAL("arena over reserve - a blown budget is a bug, not silent growth (docs/MEMORY.md section 1.1)");
        }
        const ErrCode e = a->os->commit(a->os->ctx, a->base + a->committed, want - a->committed);
        if (e != ERR_OK) {
            TL_FATAL("vmem commit failed (docs/PLATFORM.md section 9.3)");
        }
        a->committed = want;
    }

    // Hashed memory must be a pure function of state: on a registered arena EVERY dirty byte
    // that enters [base, used) must be re-zeroed - including the ALIGNMENT GAP [used, start),
    // which the hash covers but the caller never writes. Zeroing [used, min(end, high_water))
    // covers gap + block in one span; bytes at/above high_water are OS-zero already.
    if ((a->flags & ARENA_ZERO_ON_PUSH) != 0u && a->used < a->high_water) {
        const u64 z_end = (end < a->high_water) ? end : a->high_water;
        if (z_end > a->used) {
            memset(a->base + a->used, 0, z_end - a->used);
        }
    }

    u8* p = a->base + start;
    a->used = end;
    if (end > a->high_water) {
        a->high_water = end;
    }
    return p;
}

void arena_reset_to(VMemArena* a, u64 mark) {
    TL_ASSERT(a != nullptr);
    // TL_CHECK, not TL_ASSERT (PR #17 review, D7). Violating this does not merely indicate a bug:
    // the write below is unconditional, so a mark ABOVE `used` RAISES `used` and extends the
    // hashed extent over bytes nothing wrote - silent divergence on the lockstep contract, on
    // exactly the two tiers where TL_ASSERT compiles out. `bytes.h` already draws that line and
    // CPP-SUBSET section 3 states it. Cheap here: this is not a per-element path.
    // Live from RR-48 on: column_remove made this the first arena_reset_to call site in the tree
    // that targets an ARENA_HASHED arena, and the old high-water code TOLERATED `used` running
    // ahead of the live extent where the new code does not.
    TL_CHECK(mark <= a->used);
#if TL_DEV
    // Poison so a stale read shows as garbage - gated on ARENA_POISON as the header and the
    // section 8.2 flag table say, not unconditional as first shipped: poisoning EVERY arena made
    // dev-tier dirt identical (0xDD) across worlds, which silently synchronised the "divergent
    // dirt histories" the section 8.8 two-worlds criterion is supposed to hash across
    // (W1 mem review 3). Registered arenas rely on ARENA_ZERO_ON_PUSH for reuse instead.
    if ((a->flags & ARENA_POISON) != 0u && a->used > mark) {
        memset(a->base + mark, 0xDD, a->used - mark);
    }
#endif
    a->used = mark;
}

void arena_decommit_above(VMemArena* a, u64 mark) {
    TL_ASSERT(a != nullptr);
    const u64 keep = mem::align_up_u64(mark, (u64)COMMIT_GRANULE);
    if (keep < a->committed) {
        const ErrCode e = a->os->decommit(a->os->ctx, a->base + keep, a->committed - keep);
        TL_CHECK(e == ERR_OK);
        a->committed = keep;
        if (a->high_water > keep) {
            a->high_water = keep;  // decommitted pages read zero on re-commit
        }
    }
    if (a->used > mark) {
        a->used = mark;
    }
}
