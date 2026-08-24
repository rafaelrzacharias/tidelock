#pragma once
// ---------------------------------------------------------------------------------------------
// alloc_shim.h - the global-allocator tripwire: operator new is fatal, CRT mallocs are counted.
//
// Spec: docs/MEMORY.md §2 ("global allocator shim") and §8.1 (alloc_shim.cpp);
//   docs/CPP-SUBSET.md §1 (the new/malloc ban) and §4 (the symbol audit covers the sim libs;
//   this shim covers the rest). Header added over the §8.1 file list so the guard can name the
//   counter - recorded in TODO.md (W1 mem notes).
// Purpose: in dev/netcode tiers a `new` from src/ code dies loudly, and every CRT allocation is
//   counted so the arena-offset guard can prove a tick allocated nothing outside the arenas
//   (guard_tick_end: a nonzero per-tick delta is TL_FATAL - docs/MEMORY.md section 8.4).
// Invariants: the counter is cumulative and monotonic; consumers compare deltas. Vendor libs
//   allocate through mem_pool hooks, so in steady state the delta is zero.
// Determinism: never part of sim state; dev-tier tooling only. Non-det half of foundation.
// Threading: the counter is a relaxed atomic increment; reads are advisory (guard runs at tick
//   boundaries on the main thread).
// Includes: foundation/tl_types.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

// Installs CRT allocation counting. Call once at boot in dev/netcode tiers; returns ERR_OK
// only when counting is live, ERR_MEM_UNSUPPORTED where it cannot be (currently: everywhere -
// the counter needs one word of writable static storage, which docs/CPP-SUBSET.md section 1's
// link gate bans in every src/ lib; ruling request in TODO.md, W1 mem notes). A caller that
// wants to TRUST a zero CRT delta must check this return first.
extern "C" ErrCode tl_alloc_shim_install(void);

// Cumulative count of CRT/global-new allocations observed since install; 0 before install and
// wherever install reported ERR_MEM_UNSUPPORTED. Compare deltas across a tick
// (guard_tick_begin/end).
extern "C" u64 tl_crt_alloc_count(void);
