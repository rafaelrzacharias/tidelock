#pragma once
// ---------------------------------------------------------------------------------------------
// net_limits.h - the one C++ home for MAX_PEERS (docs/CANON.md: value 8, owned by NETCODE there).
//
// Spec: docs/NETCODE.md §20 preamble (the constants decided there); docs/CANON.md "MAX_PEERS".
// Purpose: `core` (input.h/PeerSlots) and `net` (wire.h) both need this constant, but the module
//   DAG (tools/audit/includes.py MODULE_DAG) has net depend on core, never the reverse, so
//   neither module's own header can be the other's source without an upward edge. `foundation`
//   sits below both, so it is the shared home (CLAUDE.md "one fact, one home" - two independent
//   `constexpr u32 MAX_PEERS = 8u;` declarations collided the moment a TU included both
//   core/input.h and net/wire.h; TODO.md RR-24).
// Determinism: a compile-time constant; nothing here executes.
// Threading: n/a.
// Includes: foundation/tl_types.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

constexpr u32 MAX_PEERS = 8u;
static_assert(MAX_PEERS <= 8u, "slot_mask/live_mask/hold bitmaps are one byte wide (docs/NETCODE.md section 20.2.2)");
