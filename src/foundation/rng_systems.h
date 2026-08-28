#pragma once
// ---------------------------------------------------------------------------------------------
// rng_systems.h - the closed system_id enum rng_for keys on.
//
// Spec: docs/DETERMINISM.md §3 (system_id is a closed enum; adding one is a registration).
// Purpose: every rng_for() draw is keyed by a system_id naming the sim system/pass/rule family
//   that drew it, so two different systems can never collide on the same (tick, carrier, draw)
//   key. This header is the ONE closed list; nothing else declares a system_id.
// Invariants: `RNG_SYS_LUAU_BASE` reserves a 256-wide block (.. +255) for Luau-registered
//   systems, assigned by registration ordinal (docs/LUAU-LAYER.md §10.6) so a script's draws are
//   keyed without editing this header. Engine systems register BELOW the block, starting at 1.
//   **0 is reserved, and that is a precondition rather than a convention**: rng_for asserts
//   `system_id != 0`, so a default-initialised or forgotten system_id traps in dev instead of
//   silently keying its draws as whatever registration put first (ruled 2026-08-24,
//   docs/DETERMINISM.md §3 - this header stated the invariant and nothing enforced it, while the
//   lane's own tests drew with 0). Alloy's SYS_BASIN..SYS_PROMOTE (docs/ALLOY.md §14.5, values
//   1..10) are the first consumer and are registered BELOW, not in a parallel file - docs/CANON.md
//   "one fact, one home" for the enum's storage. Registered here by the steward ahead of the
//   alloy-substrate lane (RR-46, ruled 2026-08-28): docs/ALLOY.md §14.1 had specified a second
//   `sim/rng_systems.h` holding its own `enum RngSystem`, which this header forbade by name. A sim
//   TU may include foundation, so two such enums are a redefinition in one TU; where they do not
//   collide they key ONE rng_for keyspace from two homes, and two systems can silently draw the
//   same stream. ALLOY.md was amended to match this header rather than the reverse.
// Determinism: the enum is part of build_id by being source (docs/DETERMINISM.md §3) - a value
//   changing meaning between builds is exactly the bug this closes.
// Threading: none - compile-time constants only.
// Includes: foundation/tl_types.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

enum RngSystem : u32 {
    // Engine systems register below the Luau block, starting at 1. Alloy (docs/ALLOY.md §14.5).
    SYS_BASIN    = 1,
    SYS_GROWTH   = 2,
    SYS_CHEM     = 3,
    SYS_EMBER    = 4,
    SYS_FRACTURE = 5,
    SYS_DECAY    = 6,
    SYS_WEATHER  = 7,
    SYS_CONDENSE = 8,
    SYS_DEBRIS   = 9,
    SYS_PROMOTE  = 10,

    RNG_SYS_LUAU_BASE = 256,   // .. +255; ordinal-assigned to Luau-registered systems
};

static_assert(RNG_SYS_LUAU_BASE > 0, "0 is reserved: never a valid system_id");
static_assert(u64(RNG_SYS_LUAU_BASE) + 255 < 0xffffffffull, "the Luau block fits in u32");
// The engine block stays strictly below the Luau block. Stated in prose above since rev 1 and
// checked by nothing until now; a raw value in a comment is a claim until a static_assert has
// seen it (LESSONS.md).
static_assert(SYS_BASIN == 1u, "0 is reserved: engine systems start at 1");
static_assert(SYS_PROMOTE < RNG_SYS_LUAU_BASE, "engine system_ids must not reach the Luau block");
