#pragma once
// ---------------------------------------------------------------------------------------------
// enet_glue.h - hooks ENet's allocator to pool_enet.
//
// Spec: docs/MEMORY.md §8.6 (`tl_enet_malloc/free`, `ENetCallbacks`);
//   docs/PLATFORM.md §9.5 (ENet -> pool_enet, `enet_initialize_with_callbacks`).
// Purpose: the ONE call site of enet_initialize_with_callbacks in the tree, so net/ installs
//   ENet's allocator hookup and initializes ENet globally by calling this instead of reaching
//   into pool_enet.h itself.
// Invariants: must run before any other ENet call, and after pool_enet_init (TL_ASSERT:
//   pool_enet() already enforces this). ENet's own no_memory callback is wired to TL_FATAL - an
//   exhausted pool_enet budget is a fatal condition, not a recoverable one (docs/PLATFORM.md §9.5).
// Determinism: none - ENet's heap is never authoritative (docs/MEMORY.md §1.5).
// Threading: call once, before any other ENet API (ENet's own single-threaded contract).
// Includes: foundation/tl_types.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

// Installs tl_enet_malloc/free as ENet's allocator and calls enet_initialize_with_callbacks.
// Returns false if ENet's own initialization fails (its return code, not a pool_enet failure -
// the pool budget is checked lazily, at the first allocation, per docs/MEMORY.md §8.6).
bool vendor_glue_enet_install(void);
