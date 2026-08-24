// solver_shadow.cpp - the FLOAT-SHADOW build of solver.cpp (docs/GATE0-BENCH.md §1, §8.1):
// the same source compiled over double typedefs into namespace g0s. Dev tiers only (the
// tests/CMakeLists.txt target block adds this TU under TL_DEV); never authoritative.
#define GATE0_SHADOW 1
#include "gate0/solver.cpp"
