// alloc_shim_ops.cpp - the global operator new/delete tripwires, ALONE in this archive member.
//
// Spec: docs/MEMORY.md §2 (the shim), §1.5 (vendor heaps), and the RR-18 ruling of 2026-08-26.
// Why its own TU: `operator new` is a replaceable function, and a program that links a vendored
// C++ library with no allocator hook (Luau's Compiler is the first - measured 32 global
// operator new calls per luau_compile, against zero from the Luau VM, which IS hooked) must be
// able to supply a pool-backed replacement. While these operators shared a member with
// tl_alloc_shim_anchor - which the arena guard force-pulls - any replacement was a
// duplicate-symbol error. Split out, ordinary archive semantics apply: a program that defines
// its own operator new never pulls this member, and one that does not gets the tripwire.
// The replacement lives in src/vendor_glue/vendor_new.cpp; docs/MEMORY.md §1.5 names the rule.
#include "foundation/alloc_shim.h"
#include "foundation/tl_assert.h"

#include <stddef.h>  // size_t - operator new's parameter type is not ours to choose

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
