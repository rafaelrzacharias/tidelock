#pragma once
// shadow.h - the FLOAT-SHADOW side-by-side run (docs/GATE0-BENCH.md §1, §4 "Shadow CSV").
// Dev tiers only: the double binding of the solver exists only there (solver_shadow.cpp).
#include "gate0/scene.h"
#include "gate0/solver_fx.h"

#include <stdio.h>

namespace g0shadow {

// Runs the fx world and the double world on `scene` in lockstep for `ticks` ticks, pass by
// pass, and writes one row per (tick, substep, pass, constraint_kind) with the max |fx - double|
// position error in pos_t raw units (docs/GATE0-BENCH.md §4). Returns the max error over the
// run. Never authoritative; its numbers are compared for drift, never for equality.
i64 shadow_run(const g0scene::Scene* scene, const g0::Consts* k, u32 ticks, FILE* csv, Scratch* scratch, const VMemApi* os, u8 dump, u32 watch);

}  // namespace g0shadow
