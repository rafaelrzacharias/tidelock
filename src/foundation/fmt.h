#pragma once
// ---------------------------------------------------------------------------------------------
// fmt.h - fmt_buf: locale-free formatted write into a caller buffer, over vendored stb_sprintf.
//
// Spec: docs/CONTAINERS.md §5 (design), §8.6/§8.6b (this header; §8.6b now records the
//   implementation, not the stub).
// Purpose: logs, CSV, editor text - the one formatter in the runtime; no heap, no locale.
// Invariants: never overflows `out` - truncates and returns the length that WOULD have been
//   written (stb_sprintf's own contract), so a caller can detect truncation by comparing the
//   return value against out.count.
// Status: implemented over vendored `stb_sprintf` (`fmt.cpp`), landed under a narrow, additive
//   cone-discipline exception (steward-relayed ruling, Rafael, 2026-08-27, PR #16 - named again
//   in `fmt.cpp`'s own header) once `vendor/stb/stb_sprintf.h` arrived; `docs/CONTAINERS.md`
//   §8.6b updated in the same commit as the design home.
// Determinism: **tooling-side only - never call this from a sim/det TU.** `fmt_buf`'s varargs
//   accept `%f`/`%g`-class specifiers, which promote a passed `float` to `double` at the call
//   site - a float on the call boundary is exactly what `docs/CPP-SUBSET.md` §9 bars from
//   `src/sim/` and the det half of `src/foundation/` (this file itself contains no float token,
//   the invariant `tl_prof.h`/`tl_probe.h`'s headers already document, but a CALLER can still
//   pass one). Not on any sim path by construction today (logs/CSV/editor text only,
//   `docs/CANON.md`'s Luau VM removal list keeps `string.format %p`-class holes out of sim
//   scope); this note exists so that stays true on purpose, not by accident.
// Threading: none - a pure buffer-write function, no shared state.
// Includes: foundation/tl_types.h, foundation/array.h (for Span<char>).
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/array.h"

// Formats into `out` (stb_sprintf semantics: truncates, never overflows, returns the length that
// would have been written) - tooling-side only, see this header's Determinism note.
u32 fmt_buf(Span<char> out, const char* fmt, ...);
