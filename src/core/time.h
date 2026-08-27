#pragma once
// ---------------------------------------------------------------------------------------------
// time.h - Clock: the one wall-clock read in the engine.
//
// Spec: docs/FRAME-LOOP.md §1 (design), §8.1 (this header). CANON.md "TICK_HZ"/"FIXED_DT_SECONDS"/
//   "MAX_STEPS" are cited from core/loop.h, not restated here (one fact, one home).
// Purpose: wraps the platform's hires timer (docs/PLATFORM.md §3 ClockApi) into a delta-seconds
//   read: `clock_tick` is the ONLY wall-clock read anywhere in the engine (docs/FRAME-LOOP.md §0)
//   - every other consumer (sim, render) sees ticks or alpha, never a clock.
// Invariants: `clock_tick` clamps its return to [0, 0.25] seconds (docs/FRAME-LOOP.md §1: "a
//   debugger pause must not spiral") - the clamp lives here so every caller gets it for free.
//   `clock_init` must run before the first `clock_tick` (TL_ASSERT).
// Determinism: f64 is legal here - core/ is not a sim TU (docs/CPP-SUBSET.md §1 bans floats only
//   in src/sim/ and the det half of src/foundation/) - but the VALUE never reaches sim state: the
//   loop (core/loop.cpp) turns it into whole tick counts via the fixed-step accumulator before
//   anything sim-side sees it. Clock itself is never registered, hashed or snapshotted.
// Threading: single-threaded; one Clock per Engine, read only from engine_frame.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, platform/platform.h (ClockApi only).
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "platform/platform.h"

// Wall-clock read + baseline. Never registered/hashed/snapshotted (docs/FRAME-LOOP.md §1).
struct Clock {
    const ClockApi* api;
    u64 last_ticks;
    u64 freq;
    u8  primed;
    u8  _pad0[7];
};
static_assert(__is_trivially_copyable(Clock), "docs/CPP-SUBSET.md section 1: POD");

// Wires c to api and takes the first baseline sample. TL_ASSERT(api != nullptr): the platform
// contract guarantees frequency() != 0 (docs/PLATFORM.md §3), so this does not re-check it.
void clock_init(Clock* c, const ClockApi* api);

// Seconds elapsed since the previous call (clock_init counts as the first "previous call"),
// clamped to [0, 0.25]. The ONLY wall-clock read in the engine (docs/FRAME-LOOP.md §0).
// TL_ASSERT(c->primed) - clock_init must run first.
f64 clock_tick(Clock* c);
