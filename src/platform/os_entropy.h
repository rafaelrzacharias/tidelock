#pragma once
// ---------------------------------------------------------------------------------------------
// os_entropy.h - internal seam between the shared os_entropy.cpp TU and each impl's entropy.cpp.
//
// Spec: docs/PLATFORM.md §5, §9.1 ("os_entropy.cpp | BCryptGenRandom / getrandom(2) | both").
// Purpose: one OS-backed EntropyApi table, built once, pointed at by both impl_sdl3/entropy.cpp
//   and impl_headless/entropy.cpp ("thin over the shared os_*", PLATFORM.md §9.4).
// Invariants: unlike vmem, this ONE file (not a per-OS pair) implements both targets behind
//   `#ifdef _WIN32` - docs/PLATFORM.md §9.1 names it singular ("both").
// Determinism: not part of the public contract - platform.h never includes this, and neither may
//   any module outside platform/net/app (docs/PLATFORM.md §5, enforced by
//   tools/audit/includes.py's ENTROPY_HEADER check).
// Threading: `fill` may be called from any thread; the OS calls behind it are thread-safe.
// Includes: platform/entropy.h only.
// ---------------------------------------------------------------------------------------------
#include "platform/entropy.h"

// Fills `*out` with the real OS-backed EntropyApi. `out->ctx` is left null.
void os_entropy_fill_table(EntropyApi* out);
