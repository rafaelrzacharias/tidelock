#pragma once
// ---------------------------------------------------------------------------------------------
// freetype_glue.h - the call counter that proves FreeType's own seam fired.
//
// Spec: docs/MEMORY.md §8.6, docs/PLATFORM.md §9.5. Filed after review round 3, N3: a test that
//   only checks pool_vendor's live_bytes delta cannot tell FreeType's contribution apart from
//   SDL3's (SDL_ttf's own 4 SDL_malloc sites also move pool_vendor once sdl3_glue is installed
//   ANYWHERE in the process - SDL_SetMemoryFunctions is process-wide and permanent), so such a
//   test is a witness only when run --isolate, one process per test. This counter is
//   attributable to FreeType alone, in any invocation.
// Purpose: exposes how many times FreeType's ftsystem.c seam has called into
//   tl_freetype_alloc/realloc/free combined, since process start - a test-only instrument
//   (docs/TESTING.md §7 "measure, don't assert"), not something production code reads.
// Determinism: none - this counter is never registered, hashed or snapshotted, same as
//   pool_vendor itself (docs/MEMORY.md §1.5).
// Threading: no locking; FreeType's own single-threaded-per-library contract is the only caller.
// Includes: foundation/tl_types.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

// The number of tl_freetype_alloc/realloc/free calls so far this process. Monotonic - it is
// never reset, so callers compare a before/after delta (the same shape as pool_stats()).
u64 tl_freetype_call_count(void);
