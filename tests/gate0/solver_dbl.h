#pragma once
// solver_dbl.h - the double (FLOAT-SHADOW) binding of the solver (namespace g0s). Dev tiers
// only (docs/GATE0-BENCH.md §1: "in a dev-only build"); solver_shadow.cpp compiles solver.cpp
// under it. Never authoritative, never hashed.
#define GATE0_SHADOW 1
#include "gate0/g0_ops.h"
#include "gate0/solver.h"
#undef GATE0_SHADOW
