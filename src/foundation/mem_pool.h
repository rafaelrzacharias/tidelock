#pragma once
// ---------------------------------------------------------------------------------------------
// mem_pool.h - the vendor-heap pool: a power-of-two size-class freelist over one VMemArena.
//
// Spec: docs/MEMORY.md §1.5 (the ruling + alternatives), §8.6 (this header).
// Purpose: Luau, ImGui, SDL, ENet and stb need realloc/free semantics; this is the ONLY general
//   allocator in the binary, budgeted and bounded. ENGINE AND SIM CODE NEVER CALL IT - the CI
//   grep allows pool_alloc only in mem_pool.cpp and vendor_glue/ (docs/MEMORY.md section 8.6).
//   Luau gets one pool per VM; ImGui and SDL share one; ENet its own.
// Invariants: 13 classes 16..64K; class pages are 64 KB, 64 KB-ALIGNED, carved from the pool's
//   arena and never returned; blocks carry no header (the class lives in the page header, first
//   16 bytes, blocks start at offset 64). Allocations > 64K take a dedicated 64 KB-aligned
//   arena_push with a 64-byte header and are freed by decommit of exactly that range. Reused
//   blocks are NOT zero (docs/MEMORY.md section 1.1) - vendor allocators do not expect zero.
//   Budget exceeded at page carve -> null (Luau raises its own memory error; ImGui/SDL assert).
// Determinism: none of these heaps is authoritative; never registered, never hashed, never
//   snapshotted. This TU is in the NON-det half of foundation (docs/BUILD.md section 10.2).
// Threading: one pool has one owning subsystem; no internal locking (SDL/ImGui/Luau call from
//   their own threads only - the vendor lane's adaptors state each contract).
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/vmem_arena.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/vmem_arena.h"

enum : u32 { POOL_CLASS_COUNT = 13 };            // 16, 32, ... 64K (docs/MEMORY.md section 8.6)
enum : u32 { POOL_MAX_SMALL   = 64 * 1024 };     // largest freelist class; above -> large path

// Read by the profiler (docs/TOOLING.md section 5); mutated only by the pool's own calls.
struct MemPoolStats {
    u64 live_bytes;                        // sum of live block class sizes + live large sizes
    u64 peak_bytes;                        // max(live_bytes) ever
    u32 live_count[POOL_CLASS_COUNT];      // live blocks per class
    u32 large_count;                       // live large allocations
};
static_assert(__is_trivially_copyable(MemPoolStats), "");

struct MemPool {
    VMemArena arena;                       // the pool's one reserve; pages carved, never returned
    u64 budget_bytes;                      // checked at page carve / large push
    u64 carved_bytes;                      // bytes taken from the arena so far (pages + large)
    void* free_head[POOL_CLASS_COUNT];     // per-class freelists (intrusive, first 8 B of block)
    MemPoolStats stats;
};

// Reserves the pool's address space and zeroes the freelists. budget_bytes caps carved pages +
// large allocations (0 = uncapped is not sanctioned: TL_ASSERT). Errors as vmem_arena_init.
ErrCode pool_init(MemPool* p, NameHash id, u64 reserve_bytes, u64 budget_bytes, const VMemApi* os);

// Allocates `size` bytes, 16-byte aligned, from the class freelist (<= 64K) or the large path.
// Returns null when the budget is exhausted - never TL_FATAL (the vendor lib owns the failure).
// size 0 returns null. Contents are garbage (reused) or zero (fresh pages) - callers assume
// neither.
void* pool_alloc(MemPool* p, u64 size);

// Frees a pool_alloc/pool_realloc block: class blocks return to their freelist, large blocks
// are decommitted (address space is not reused). null is a no-op. Double-free is undetected in
// release; TL_ASSERT catches a non-pool pointer in dev where cheap.
void pool_free(MemPool* p, void* q);

// Grows/shrinks `q` to new_size: same class -> same pointer; else alloc + memcpy(min) + free.
// null q behaves as pool_alloc; new_size 0 behaves as pool_free returning null. Returns null
// (q untouched) when the budget refuses the new block.
void* pool_realloc(MemPool* p, void* q, u64 new_size);

// The pool's live/peak/per-class counters; valid until the next pool call.
const MemPoolStats* pool_stats(const MemPool* p);
