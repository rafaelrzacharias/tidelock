#pragma once
// ---------------------------------------------------------------------------------------------
// fmt.h - fmt_buf: locale-free formatted write into a caller buffer, over vendored stb_sprintf.
//
// Spec: docs/CONTAINERS.md §5 (design), §8.6 (this header).
// Purpose: logs, CSV, editor text - the one formatter in the runtime; no heap, no locale.
// Invariants: never overflows `out` - truncates and returns the length that WOULD have been
//   written (stb_sprintf's own contract), so a caller can detect truncation by comparing the
//   return value against out.count.
// Status: **STUB - TL_FATAL("unimplemented")**. `docs/BUILD.md` §5 lists `stb_sprintf` as a
//   vendored library but `vendor/CMakeLists.txt` assigns its arrival to the W1 platform lane
//   ("SDL3 + stb arrive with the W1 platform lane") - not this lane, and not yet landed as of this
//   commit (checked: no `vendor/*stb*` tree exists on `main` or `w1-platform`). Per
//   docs/ROADMAP.md §0 rule 1 ("header first... stubs that TL_FATAL('unimplemented')"), the
//   contract ships now so dependents compile against it; the body is filled in the day
//   `vendor/stb_sprintf/` lands - implementing a hand-rolled formatter here instead would violate
//   the doc's explicit "over stb_sprintf" and duplicate a vendoring decision that belongs to the
//   platform lane (TODO.md carries this as a note, not a ruling - the owner and the mechanism are
//   already decided, only the landing is pending).
// Determinism: not on any sim path (logs/CSV/editor only, docs/CANON.md's Luau VM removal list
//   keeps `string.format %p`-class holes out of sim scope); N/A until implemented.
// Threading: none - a pure buffer-write function once implemented.
// Includes: foundation/tl_types.h, foundation/array.h (for Span<char>).
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/array.h"

// Formats into `out` (stb_sprintf semantics: truncates, never overflows, returns the length that
// would have been written). STUB - see the contract block above.
u32 fmt_buf(Span<char> out, const char* fmt, ...);
