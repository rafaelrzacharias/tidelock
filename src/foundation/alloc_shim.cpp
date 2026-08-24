// alloc_shim.cpp - the global-allocator tripwire. Spec: docs/MEMORY.md §2, §8.1.
// SHIPPED STATELESS, deliberately: the specced CRT-malloc COUNTER needs one word of writable
// static storage, and the writable-static link gate (docs/CPP-SUBSET.md §1) bans .data/.bss in
// every src/ lib with no exemption mechanism - a genuine cross-spec contradiction, filed as a
// ruling request in TODO.md (W1 mem notes; it also hits the tooling-rt lane's log/prof state).
// Until ruled: operator new/delete are TL_FATAL tripwires in dev/netcode tiers (they need no
// state), tl_alloc_shim_install reports ERR_MEM_UNSUPPORTED, and tl_crt_alloc_count reads 0 -
// the guard's CRT check is vacuous but HONEST (install's return value says counting is off).
// The tripwire takes effect in a binary only if this object is pulled in; referencing
// tl_crt_alloc_count from the guard does that for every guard user.
#include "foundation/alloc_shim.h"
#include "foundation/vmem_arena.h"
#include "foundation/tl_assert.h"

#include <stddef.h>  // size_t - operator new's parameter type is not ours to choose

extern "C" ErrCode tl_alloc_shim_install(void) {
    return ERR_MEM_UNSUPPORTED;  // counting is off until the writable-static ruling (TODO.md)
}

extern "C" u64 tl_crt_alloc_count(void) {
    return 0u;
}

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
