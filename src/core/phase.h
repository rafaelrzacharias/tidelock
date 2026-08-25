#pragma once
// ---------------------------------------------------------------------------------------------
// phase.h - the closed phase enum and its display names.
//
// Spec: docs/FRAME-LOOP.md §2 (the phase table; position-named, not role-named), §8.1 (this
//   header); docs/ECS.md §3 (SystemDesc.phase); docs/CANON.md "Phases and the barrier".
//   FRAME-LOOP.md §8.1 assigns this file to the loop lane (W3 loop+input); it landed from the
//   W2 ecs lane header-first (docs/ROADMAP.md §0 rule 1) because schedule.h cannot exist without
//   the enum - the loop lane owns loop.h/time.h/interp.cpp and inherits this header as-is.
// Purpose: one closed value set for "when does this system run". Position names (FIRST, LAST)
//   rather than role names (physics, collision) - role names bake a pipeline assumption
//   (docs/ECS.md §3, the soft Layr mistake).
// Invariants: sim phases are PHASE_FIRST..PHASE_LAST (every tick); render phases are
//   PHASE_PRE_RENDER..PHASE_RENDER (per frame). The set is closed and APPEND-ONLY: the reserved
//   future names (INIT, FRAME_END - docs/FRAME-LOOP.md §2) are appended after PHASE_RENDER when
//   a consumer exists, never inserted, so stored phase values and the schedule's phase_begin
//   layout stay stable.
// Determinism: values only; the per-phase barrier and ordering rules live in docs/ECS.md §3/§4.
// Threading: none.
// Includes: foundation/tl_types.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

// The closed set, in execution order (docs/FRAME-LOOP.md §2).
enum Phase : u8 {
    PHASE_FIRST = 0,
    PHASE_PRE_UPDATE,
    PHASE_UPDATE,
    PHASE_POST_UPDATE,
    PHASE_LAST,
    PHASE_PRE_RENDER,
    PHASE_RENDER,
    PHASE_COUNT,
};

// The sim half of the pipeline is [PHASE_FIRST, PHASE_SIM_LAST]; the render half follows.
constexpr Phase PHASE_SIM_LAST = PHASE_LAST;

// Display names for the log/profiler/editor, indexed by Phase.
constexpr const char* PHASE_NAMES[PHASE_COUNT] = {
    "FIRST", "PRE_UPDATE", "UPDATE", "POST_UPDATE", "LAST", "PRE_RENDER", "RENDER",
};

static_assert(PHASE_COUNT == 7, "append-only: new phases go after PHASE_RENDER (docs/FRAME-LOOP.md section 2)");
