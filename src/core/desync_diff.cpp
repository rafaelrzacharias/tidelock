// desync_diff.cpp - see desync_diff.h's contract block. Spec: docs/TOOLING.md §9.3.8.
#include "core/desync_diff.h"

#include "foundation/tl_assert.h"

#include <string.h>

namespace {

// arena_registry.cpp's own alignment (mem::align_up_64b, foundation/vmem_arena.h) - the blob
// layout desync_diff reads must match registry_snapshot's write exactly, or every offset past
// the first flagged arena is wrong.
u64 align_up_64b(u64 v) { return (v + 63u) & ~(u64)63u; }

void report(u32* n, DiffFn out, void* ctx, const DesyncEntry& e) {
    out(ctx, &e);
    *n += 1u;   // the caller's loop condition checks *n against max_n before the NEXT iteration
}

}  // namespace

u32 desync_diff(const ArenaRegistry* reg, const Snapshot* a, const Snapshot* b, u32 max_n,
                 DiffFn out, void* ctx) {
    TL_CHECK(reg != nullptr && a != nullptr && b != nullptr && out != nullptr);
    TL_CHECK(reg->sealed != 0u);
    TL_CHECK(a->count == reg->count && b->count == reg->count);

    if (max_n == 0u) { return 0u; }   // header's "out is called at most max_n times" - 0 means never

    if (memcmp(a->session_fingerprint, b->session_fingerprint, 32u) != 0) {
        DesyncEntry e{};
        e.kind = DIFF_FINGERPRINT_MISMATCH;
        out(ctx, &e);
        return 1u;
    }

    u32 n = 0u;
    u64 off_a = 0u, off_b = 0u;   // independent: a size mismatch means the two blobs desync from
                                  // that arena on, so each side's own offset must advance on its
                                  // own used[], not a shared one.
    for (u32 i = 0; i < reg->count && n < max_n; ++i) {
        const u64 ua = a->used[i], ub = b->used[i];
        const bool flagged = (reg->e[i].flags & ARENA_SNAPSHOT) != 0u;

        if (ua != ub) {
            DesyncEntry e{};
            e.kind = DIFF_USED;
            e.arena_index = i;
            e.arena_id = reg->e[i].id;
            e.used_a = ua;
            e.used_b = ub;
            report(&n, out, ctx, e);
            if (flagged) { off_a = align_up_64b(off_a) + ua; off_b = align_up_64b(off_b) + ub; }
            continue;
        }
        if (!flagged) { continue; }   // used[] recorded but no blob segment to compare (this
                                      // header's Purpose note; matches registry_snapshot/restore)

        off_a = align_up_64b(off_a);
        off_b = align_up_64b(off_b);
        TL_CHECK(off_a + ua <= a->blob_cap && off_b + ub <= b->blob_cap);
        const u8* seg_a = a->blob + off_a;
        const u8* seg_b = b->blob + off_b;
        if (memcmp(seg_a, seg_b, ua) != 0) {
            u64 at = 0u;
            while (at < ua && seg_a[at] == seg_b[at]) { at += 1u; }
            DesyncEntry e{};
            e.kind = DIFF_BYTES;
            e.arena_index = i;
            e.arena_id = reg->e[i].id;
            e.byte_offset = at;
            const u64 snip = (ua - at < 16u) ? (ua - at) : 16u;
            memcpy(e.bytes_a, seg_a + at, snip);
            memcpy(e.bytes_b, seg_b + at, snip);
            report(&n, out, ctx, e);
        }
        off_a += ua;
        off_b += ub;
    }
    return n;
}
