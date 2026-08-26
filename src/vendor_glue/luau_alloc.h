#pragma once
// ---------------------------------------------------------------------------------------------
// luau_alloc.h - tl_luau_alloc: Luau's lua_Alloc hook over a mem_pool.
//
// Spec: docs/MEMORY.md §8.6 (the adaptor list; vendor_glue/ is the one folder outside
//   mem_pool.cpp that may call pool_*), docs/LUAU-LAYER.md §10.2 step 2 (the four cases and the
//   over-budget contract).
// Purpose: every byte a lua_State allocates comes from ONE budgeted pool per VM, so the Luau
//   heap is bounded, measured (the profiler reads pool_stats) and outside the registered arena
//   set - it is not authoritative state and is never hashed or snapshotted (docs/MEMORY.md §5).
// Invariants: `ud` is the MemPool* handed to lua_newstate; one pool serves exactly one
//   lua_State. Over budget returns null rather than trapping - Luau raises LUA_ERRMEM
//   ("not enough memory") and unwinds to the nearest protected call, which is always ours
//   (docs/LUAU-LAYER.md §10.2 step 2), so the failure takes the module's normal error path.
// Determinism: the pool's addresses are not state and never reach a hash; an allocation failure
//   is a function of the budget, which IS fingerprinted (docs/MEMORY.md §7 R-2), so every peer
//   fails at the same call.
// Threading: a pool has one owning lua_State and no internal locking; the sim VM is called only
//   from the tick thread, the UI VM only from the frame thread.
// Includes: foundation/tl_types.h, foundation/mem_pool.h, <stddef.h> (size_t is Luau's ABI).
//   The .cpp additionally reaches <stdlib.h> for free() - see tl_luau_compile_free.
// ---------------------------------------------------------------------------------------------
#include <stddef.h>

#include "foundation/mem_pool.h"
#include "foundation/tl_types.h"

// Luau's lua_Alloc: `ud` is the VM's MemPool*. nsize == 0 frees `ptr` and returns null; a null
// `ptr` allocates nsize bytes; otherwise the block is resized (osize is accepted for ABI
// compatibility and ignored - the pool recovers the old size from the block's page header,
// docs/MEMORY.md §8.6). Returns null when the pool's budget refuses the request, leaving `ptr`
// untouched; it never traps. Signature-compatible with lua_Alloc by static_assert in the .cpp,
// which is the only TU that may see a Luau header.
extern "C" void* tl_luau_alloc(void* ud, void* ptr, size_t osize, size_t nsize);

// Frees the bytecode buffer luau_compile returns. That buffer is malloc'd by upstream contract
// and is the ONE Luau allocation that does not go through tl_luau_alloc: the Compiler has no
// allocator hook at all (TODO.md RR-18 carries the measurement and the consequences). The free
// lives here rather than in src/script so the <stdlib.h> grant - and with it malloc - stays
// confined to the folder whose job is vendor heap plumbing. Null is a no-op.
extern "C" void tl_luau_compile_free(void* bytecode);
