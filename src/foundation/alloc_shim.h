#pragma once
// ---------------------------------------------------------------------------------------------
// alloc_shim.h - the global-allocator tripwire: operator new/delete from src/ code is fatal.
//
// Spec: docs/MEMORY.md §2 ("global allocator shim") and §8.1 (alloc_shim.cpp);
//   docs/CPP-SUBSET.md §1 (the new/malloc ban) and §4 (the symbol audit covers the sim libs;
//   this shim covers the rest).
// Purpose: in dev/netcode tiers a `new` from src/ code dies loudly. The specced CRT-malloc
//   COUNTER was DROPPED by ruling (2026-08-26): it needed a word of writable static storage the
//   link gate bans, and the mechanism is the tripwires + the symbol audit + vendor pool hooks.
// Determinism: never part of sim state; dev-tier tooling only. Non-det half of foundation.
// Includes: foundation/tl_types.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

// Link anchor: the tripwire operators take effect in a binary only if this object is pulled
// in; the guard calls this no-op so every guard user links the shim (docs/MEMORY.md section 2).
extern "C" void tl_alloc_shim_anchor(void);
