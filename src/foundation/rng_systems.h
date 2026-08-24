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
//   keyed without editing this header. Engine systems register BELOW the block, starting at 1
//   (0 is reserved: an accidentally-default-initialised system_id must never alias a real
//   system). Rev 1 has no engine systems yet - Alloy's SYS_BASIN..SYS_PROMOTE (docs/ALLOY.md
//   §14.5, values 1..10) are the first consumer and fit comfortably under the 256 reserved here;
//   that lane adds its own names to this enum when it lands (one registration each), not a
//   parallel file - docs/CANON.md "one fact, one home" for the enum's storage.
// Determinism: the enum is part of build_id by being source (docs/DETERMINISM.md §3) - a value
//   changing meaning between builds is exactly the bug this closes.
// Threading: none - compile-time constants only.
// Includes: foundation/tl_types.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

enum RngSystem : u32 {
    RNG_SYS_LUAU_BASE = 256,   // .. +255; ordinal-assigned to Luau-registered systems
};

static_assert(RNG_SYS_LUAU_BASE > 0, "0 is reserved: never a valid system_id");
static_assert(u64(RNG_SYS_LUAU_BASE) + 255 < 0xffffffffull, "the Luau block fits in u32");
