// alloc_shim.cpp - the global-allocator tripwire. Spec: docs/MEMORY.md §2, §8.1.
// STATELESS by ruling (2026-08-26): the specced CRT-malloc counter was DROPPED - it needed a
// word of writable static storage the link gate (docs/CPP-SUBSET.md §1) bans, and the actual
// mechanism is these TL_FATAL tripwires + the symbol audit + vendor pool hooks. The tripwires
// take effect in a binary only if this object is pulled in; the guard calls
// tl_alloc_shim_anchor so every guard user links it.
#include "foundation/alloc_shim.h"
#include "foundation/vmem_arena.h"
#include "foundation/tl_assert.h"

#include <stddef.h>  // size_t - operator new's parameter type is not ours to choose

extern "C" void tl_alloc_shim_anchor(void) {}

#if TL_TIER_DEV || TL_TIER_NETCODE

// docs/MEMORY.md §2: a global `new` from src/ code is a link error for sim libs (the symbol
// audit) and a fatal tripwire for the rest. Vendor libs allocate through their mem_pool hooks
// (docs/MEMORY.md §1.5), so nothing legitimate ever lands here.
void* operator new(size_t) { TL_FATAL("global operator new - use an arena or a pool (docs/MEMORY.md section 2)"); }
void* operator new[](size_t) { TL_FATAL("global operator new[] - use an arena or a pool (docs/MEMORY.md section 2)"); }
void operator delete(void*) noexcept { TL_FATAL("global operator delete (docs/MEMORY.md section 2)"); }
void operator delete[](void*) noexcept { TL_FATAL("global operator delete[] (docs/MEMORY.md section 2)"); }
void operator delete(void*, size_t) noexcept { TL_FATAL("global sized operator delete (docs/MEMORY.md section 2)"); }
void operator delete[](void*, size_t) noexcept { TL_FATAL("global sized operator delete[] (docs/MEMORY.md section 2)"); }

#endif  // TL_TIER_DEV || TL_TIER_NETCODE
