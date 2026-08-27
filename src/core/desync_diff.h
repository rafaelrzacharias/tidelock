#pragma once
// ---------------------------------------------------------------------------------------------
// desync_diff.h - the reflection diff walker: registry order -> arena -> bytes.
//
// Spec: docs/TOOLING.md §9.3.8 (algorithm), §9.1 (this file's row). `reg`, `DesyncEntry`,
//   `DiffFn`, and the `DiffKind` enum are a "signature added over spec" completion (matching
//   docs/CONTAINERS.md §8.6a's own precedent for this class of gap): §9.3.8's pseudocode gives
//   `desync_diff(const Snapshot* a, const Snapshot* b, ...)` with no way to know which arenas are
//   `ARENA_SNAPSHOT`-flagged (only flagged arenas occupy blob space - `arena_registry.cpp`'s
//   `registry_snapshot`/`registry_restore` both take the registry for exactly this reason), so a
//   third parameter is required, not optional.
// Purpose: given two snapshots of the SAME sealed registry (same arena set, same order), find
//   where they diverge - the DETERMINISM.md §7 workflow's step 3. Scoped to what a plain
//   byte-level walk can report without a component/pool table: a size mismatch (`DIFF_USED`) or,
//   same size, the first differing byte run (`DIFF_BYTES`). The per-field ECS-column case
//   (`table = component info`) and the Alloy pool-table case from §9.3.8's pseudocode are NOT
//   built here - the first needs `World`'s registered `ComponentInfo` cross-referenced against
//   `ArenaEntry::id`, real work not yet scoped; the second needs Alloy, which has not landed. Both
//   stay `DIFF_BYTES` (the honest fallback) until they land, rather than a wrong or invented
//   report.
// Invariants: `reg` must be sealed and be the SAME registry (id/order) both `a` and `b` were
//   captured from - callers who need to prove that first should compare `session_fingerprint`
//   themselves or rely on `DIFF_FINGERPRINT_MISMATCH`'s short-circuit. `out` is called at most
//   `max_n` times; the return value is the number of calls made (0 = identical). `DIFF_BYTES`
//   copies at most 16 bytes from the first differing offset into each of `bytes_a`/`bytes_b`,
//   zero-padded past `used` - a diagnostic snippet, not the full differing region.
// Determinism: diagnostic only - reads snapshots, writes nothing, never touches live sim state;
//   not on any sim path itself (`tl_driver --diff`/a future panel call it), but the walker itself
//   has no float, no io, no allocation, so it carries no det-subset exemption of its own.
// Threading: none - single-threaded, matching every other reflection walker in the tree.
// Includes: foundation/tl_types.h, foundation/arena_registry.h, foundation/snapshot.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/arena_registry.h"
#include "foundation/snapshot.h"

enum DiffKind : u32 {
    DIFF_FINGERPRINT_MISMATCH = 0,   // a.session_fingerprint != b.session_fingerprint - whole-snapshot, walk short-circuits
    DIFF_USED,                       // arena_index's used[] extent differs - no byte comparison attempted
    DIFF_BYTES,                      // same used[] extent, first differing byte run (component/pool table not consulted)
};

// One reported difference. `arena_index`/`arena_id` are 0 for DIFF_FINGERPRINT_MISMATCH (no
// single arena is at fault - the snapshots do not agree on identity at all). `used_a`/`used_b`
// are DIFF_USED's payload (0 for the other kinds). `byte_offset` is the first differing byte's
// offset within the arena's own segment (not the blob); `bytes_a`/`bytes_b` are up to 16 bytes
// read starting there, zero-padded past `used` - DIFF_BYTES's payload (0/zeroed for the others).
struct DesyncEntry {
    DiffKind kind;
    u32      arena_index;
    NameHash arena_id;
    u64      used_a, used_b;
    u64      byte_offset;
    u8       bytes_a[16];
    u8       bytes_b[16];
};

typedef void (*DiffFn)(void* ctx, const DesyncEntry* e);

// Walks `reg`'s registry order over `a`/`b`. Fingerprint mismatch: one DIFF_FINGERPRINT_MISMATCH
// call, returns 1, no further comparison. Otherwise, for each of `reg->count` arenas: a used[]
// mismatch reports DIFF_USED and moves to the next arena (no byte comparison, per this header's
// Purpose note); equal used[] and equal bytes moves on silently; equal used[] and unequal bytes
// reports DIFF_BYTES at the first differing offset. Stops (returns max_n) the instant `out` has
// been called max_n times. TL_CHECK: reg/a/b/out non-null, reg->sealed, a->count == b->count ==
// reg->count (the same precondition registry_restore enforces via ERR_SNAPSHOT_MISMATCH - this
// walker TL_CHECKs it instead since a caller diffing two snapshots of different registries is a
// caller bug, not a runtime condition to recover from).
u32 desync_diff(const ArenaRegistry* reg, const Snapshot* a, const Snapshot* b, u32 max_n,
                 DiffFn out, void* ctx);
