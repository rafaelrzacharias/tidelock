#pragma once
// ---------------------------------------------------------------------------------------------
// vmem_arena.h - VMemArena: reserve address space once, commit on demand, bump-allocate.
//
// Spec: docs/MEMORY.md §1.1 (design) and §8.2 (this header, the build contract);
//   docs/PIVOT-DESIGN.md §4 is the ruling it expands.
// Purpose: the allocator everything authoritative lives in. Stable base forever, so columns and
//   pools grow without relocation and raw pointers are valid within a pass; no general free()
//   exists by design (docs/MEMORY.md section 0 rule 1).
// Invariants: used <= committed <= reserved, all page-tracked; committed grows in COMMIT_GRANULE
//   multiples; exceeding `reserved` is TL_FATAL - a blown budget is a bug, not silent growth.
//   Fresh pages are OS-zero (docs/PLATFORM.md section 9.3); memory below high_water that is
//   re-pushed is NOT zero unless ARENA_ZERO_ON_PUSH re-zeroes it - every pool header must state
//   its side of that asymmetry (docs/MEMORY.md section 1.1).
// Determinism: registered (hashed) arenas set ARENA_ZERO_ON_PUSH so hashed bytes are a pure
//   function of state, never of allocation history (docs/CPP-SUBSET.md section 5, padding rule).
//   This TU is in the audited det half: no io, no OS symbol - the VMemApi table is injected.
// Threading: one arena has one writer at a time; per-worker scratch gets its own arena
//   (docs/MEMORY.md section 1.3). No internal locking.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/vmem_api.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/vmem_api.h"

// The mem module's ErrCode range is 0x01xx; ERR_MEM_OOM's value is docs/CPP-SUBSET.md section 3's
// own example. ERR_SNAPSHOT_MISMATCH is named by docs/MEMORY.md section 8.3.
constexpr ErrCode ERR_MEM_OOM              = (ErrCode)0x0101;  // address-space reserve failed / pool budget exhausted
constexpr ErrCode ERR_SNAPSHOT_MISMATCH    = (ErrCode)0x0102;  // fingerprint/count/id mismatch on restore
constexpr ErrCode ERR_MEM_RING_OVERFLOW    = (ErrCode)0x0103;  // snapshot blob budget exceeded, dev tiers (docs/MEMORY.md section 7 R-2)
constexpr ErrCode ERR_MEM_BAD_ARG          = (ErrCode)0x0104;  // malformed init argument (zero reserve, null table)
constexpr ErrCode ERR_MEM_UNSUPPORTED      = (ErrCode)0x0105;  // facility unavailable in this tier/CRT (alloc shim)

// Log-side name for a mem ErrCode; returns "ERR_?" for codes outside the mem range.
constexpr const char* err_mem_name(ErrCode e) {
    return e == ERR_OK ? "ERR_OK"
         : e == ERR_MEM_OOM ? "ERR_MEM_OOM"
         : e == ERR_SNAPSHOT_MISMATCH ? "ERR_SNAPSHOT_MISMATCH"
         : e == ERR_MEM_RING_OVERFLOW ? "ERR_MEM_RING_OVERFLOW"
         : e == ERR_MEM_BAD_ARG ? "ERR_MEM_BAD_ARG"
         : e == ERR_MEM_UNSUPPORTED ? "ERR_MEM_UNSUPPORTED" : "ERR_?";
}

// VMemArena flags (docs/MEMORY.md section 8.2). Registry membership flags (ARENA_HASHED /
// ARENA_SNAPSHOT / ARENA_GROWS_AT_BARRIER) are a different field on ArenaEntry - arena_registry.h.
enum : u32 {
    ARENA_POISON       = 1u << 0,  // debug: memset 0xDD above the mark on reset (scratch arenas)
    ARENA_ZERO_ON_PUSH = 1u << 1,  // re-zero re-used (dirty) bytes on push (registered arenas)
};

enum : u32 { COMMIT_GRANULE = 64 * 1024 };  // commit step; docs/MEMORY.md section 8.2

// docs/MEMORY.md section 8.2, field for field. [used, high_water) is dirty; [high_water,
// committed) is OS-zero; [committed, reserved) is uncommitted address space.
struct VMemArena {
    u8*  base;        // reserved range start, page aligned; stable for the arena's lifetime
    u64  reserved;    // bytes reserved
    u64  committed;   // bytes committed, multiple of COMMIT_GRANULE
    u64  used;        // bump pointer
    u64  high_water;  // max(used) ever - the dirty/clean boundary for the zero-on-push rule
    u32  page;        // OS page size (from VMemApi.page_size)
    u32  flags;       // ARENA_POISON | ARENA_ZERO_ON_PUSH
    NameHash id;
    const VMemApi* os;
};
static_assert(sizeof(VMemArena) == 64, "docs/MEMORY.md section 8.2 layout");
static_assert(__is_trivially_copyable(VMemArena), "");

// Reserves reserve_bytes (rounded up to a page) of address space; commits nothing. Returns
// ERR_MEM_BAD_ARG on a null/zero argument, ERR_MEM_OOM if the OS refuses the reservation.
ErrCode vmem_arena_init(VMemArena* a, NameHash id, u64 reserve_bytes, u32 flags, const VMemApi* os);

// Bump-allocates `bytes` at `align` (power of two, <= page). Commits pages as needed; TL_FATAL
// over reserve (budget violation). Fresh memory is zero; re-used memory is zero only under
// ARENA_ZERO_ON_PUSH. Legal inside a tick only for scratch / GROWS_AT_BARRIER windows
// (docs/MEMORY.md section 2). Never returns null.
void* arena_push(VMemArena* a, u64 bytes, u32 align);

// The current bump mark; pass to arena_reset_to / arena_decommit_above. Pure, any time.
inline u64 arena_mark(const VMemArena* a) { return a->used; }

// Rolls `used` back to `mark` (mark <= used, TL_ASSERT). Debug + ARENA_POISON: fills the freed
// range with 0xDD so a stale read shows as garbage. Pages stay committed.
void arena_reset_to(VMemArena* a, u64 mark);

// Returns whole COMMIT_GRANULE units above `mark` to the OS and rolls `used` back to
// min(used, mark). A later push re-commits and reads zeros (docs/PLATFORM.md section 9.3).
// The streaming path for Alloy's per-chunk terrain arena (docs/ALLOY.md section 13).
void arena_decommit_above(VMemArena* a, u64 mark);
