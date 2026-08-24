#pragma once
// ---------------------------------------------------------------------------------------------
// tl_prof.h - TL_PROF_{BEGIN,END,SCOPE,SCOPE_W,COUNTER_SET,COUNTER_ADD}.
//
// Spec: docs/TOOLING.md §5, §9.1 (macro text, verbatim), §9.3.1 (the runtime algorithm);
//   docs/CPP-SUBSET.md §7b (tier table), §9 R-4 (RR-7 - the tooling-plane exemption `prof.cpp`
//   will rely on once it lands).
// Purpose: macros only, today. The runtime behind them (`prof.cpp`: `ProfState`, the per-worker
//   node buffers, the 60-frame ring, Chrome trace export) needs `NameHash` (`foundation/hash.h`,
//   not yet built - Foundation-week's job) and `Scratch` (`foundation/scratch.h`, `MEMORY.md`
//   §1.3, not yet built) for `TL_PROF_SCOPE_W`'s `(scr)->worker`, and its first real consumer is
//   the ECS scheduler auto-scoping every system (`ECS.md`, not yet built) - so per
//   `docs/TOOLING.md` §9.6 build order item 3, this ships as the stable macro interface only
//   (`ARCHITECTURE.md` "large subsystem = stable interface + ONE impl now, pulled in by a real
//   consumer"); the runtime is a `TODO.md` item for whichever lane needs the first real scope.
// Invariants: outside `TL_DEV` every macro compiles to `((void)0)` (`TL_PROF_SCOPE` still opens a
//   plain, empty `for`-block, so a `return` inside one is a review error under `TL_DEV`, silently
//   fine outside it - the reason `TOOLING.md` §9.1 calls that shape out explicitly).
// Determinism: never hashed, never snapshotted (docs/CPP-SUBSET.md §9 R-4) - a profiler scope
//   records timing, never sim state.
// Threading: `TL_PROF_SCOPE` is worker 0 (main thread) by contract; `TL_PROF_SCOPE_W` takes the
//   job system's worker id explicitly (`JOBS.md` §1) - neither macro is safe to nest across an
//   actual thread hand-off, only within one worker's call stack.
// Includes: foundation/tl_types.h only.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

#if TL_DEV
#  define TL_PROF_BEGIN(lit)            tl_prof_begin(0, lit##_id, lit, 0xFFFFFFFFu)
#  define TL_PROF_END()                 tl_prof_end(0)
#  define TL_PROF_SCOPE(lit)            for (u32 _tl_ps = (TL_PROF_BEGIN(lit), 0u); _tl_ps == 0u; TL_PROF_END(), _tl_ps = 1u)
#  define TL_PROF_SCOPE_W(scr, lit, job) for (u32 _tl_ps = (tl_prof_begin((scr)->worker, lit##_id, lit, (job)), 0u); _tl_ps == 0u; tl_prof_end((scr)->worker), _tl_ps = 1u)
#  define TL_PROF_COUNTER_SET(lit, v)   tl_prof_counter(lit##_id, lit, (i64)(v), 0)
#  define TL_PROF_COUNTER_ADD(lit, v)   tl_prof_counter(lit##_id, lit, (i64)(v), 1)
#else
#  define TL_PROF_BEGIN(lit)            ((void)0)
#  define TL_PROF_END()                 ((void)0)
#  define TL_PROF_SCOPE(lit)            for (u32 _tl_ps = 0u; _tl_ps == 0u; _tl_ps = 1u)
#  define TL_PROF_SCOPE_W(scr, lit, job) for (u32 _tl_ps = 0u; _tl_ps == 0u; _tl_ps = 1u)
#  define TL_PROF_COUNTER_SET(lit, v)   ((void)0)
#  define TL_PROF_COUNTER_ADD(lit, v)   ((void)0)
#endif
