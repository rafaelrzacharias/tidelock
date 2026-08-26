// alloc_shim.cpp - the link ANCHOR only. The tripwire operators themselves live in
// alloc_shim_ops.cpp, in their own archive member, so that a program which must host a
// vendored C++ library with no allocator hook can supply its own operator new WITHOUT a
// duplicate-symbol error: ordinary archive semantics then leave this member's operators
// unpulled (RR-18, ruled 2026-08-26; docs/MEMORY.md §2).
// Spec: docs/MEMORY.md §2, §8.1.
// STATELESS by ruling (2026-08-26): the specced CRT-malloc counter was DROPPED - it needed a
// word of writable static storage the link gate (docs/CPP-SUBSET.md §1) bans, and the actual
// mechanism is these TL_FATAL tripwires + the symbol audit + vendor pool hooks. The tripwires
// take effect in a binary only if this object is pulled in; the guard calls
// tl_alloc_shim_anchor so every guard user links it.
#include "foundation/alloc_shim.h"
#include "foundation/tl_assert.h"

extern "C" void tl_alloc_shim_anchor(void) {}
