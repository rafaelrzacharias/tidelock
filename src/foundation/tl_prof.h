#pragma once
// ---------------------------------------------------------------------------------------------
// tl_prof.h - TL_PROF_{BEGIN,END,SCOPE,SCOPE_W,COUNTER_SET,COUNTER_ADD}.
//
// Spec: docs/TOOLING.md §5, §9.1 (macro text, verbatim), §9.3.1 (the runtime algorithm);
//   docs/CPP-SUBSET.md §7b (tier table), §9 R-4 (RR-7 - the tooling-plane exemption `prof.cpp`
//   will rely on once it lands).
// Purpose: `prof.cpp` (w3-editor lane, docs/TOOLING.md §9.6 build order item 3) implements
//   `ProfState`: per-worker node buffers, a 60-frame ring, counters. `TL_PROF_SCOPE_W`'s
//   `(scr)->worker` still cannot compile: `Scratch` (`foundation/scratch.h`) carries no `worker`
//   field by design (docs/ARCHITECTURE.md's "pulled in by a real consumer, never pushed on spec"
//   ruling, 2026-08-26) - that stays a documented gap, not this lane's to close, until a real
//   `parallel_for` caller lands (`JOBS.md`) and files for the field. The ECS scheduler auto-
//   scoping every system (docs/TOOLING.md §9.3.1) is `core/schedule.cpp`'s integration, a
//   follow-up this lane does not make either (cone discipline, docs/ROADMAP.md §0 rule 2 - ecs
//   is merged and closed); `TL_PROF_SCOPE`/`BEGIN`/`END`/`COUNTER_*` (worker 0 only) are fully
//   usable today.
// Invariants: outside `TL_DEV` every macro compiles to `((void)0)` (`TL_PROF_SCOPE` still opens a
//   plain, empty `for`-block, so a `return` inside one is a review error under `TL_DEV`, silently
//   fine outside it - the reason `TOOLING.md` §9.1 calls that shape out explicitly). `ticks()`
//   (below) is a monotonically-increasing placeholder counter, not a real wall clock: `ClockApi`
//   (`platform/platform.h`) is not wired into `foundation` yet (the same "not wired" shape
//   `tl_log.h`'s `tick` and `tl_probe.h`'s tick-throttle document) - `prof_zero_alloc_and_tree`
//   asserts structure (`t_end >= t_begin`, parent/depth, overflow counting), never a real
//   duration; real wall time is a `TODO.md` item for whoever wires `ClockApi` through.
// Determinism: never hashed, never snapshotted (docs/CPP-SUBSET.md §9 R-4) - a profiler scope
//   records timing, never sim state.
// Threading: `TL_PROF_SCOPE` is worker 0 (main thread) by contract; `TL_PROF_SCOPE_W` takes the
//   job system's worker id explicitly (`JOBS.md` §1) - neither macro is safe to nest across an
//   actual thread hand-off, only within one worker's call stack.
// Includes: foundation/tl_types.h, foundation/hash.h (NameHash, for ProfNode::key/ProfCounter::key).
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/hash.h"

// TL_PROF_SCOPE's for-loop variable is unique per EXPANSION SITE (`__COUNTER__`, evaluated once
// per macro invocation via the two-layer indirect-paste idiom below), not a fixed `_tl_ps` -
// nesting is the profiler's whole reason to exist (docs/TOOLING.md §1 "one substrate"'s
// hierarchical scopes), and a fixed name shadows itself the moment two TL_PROF_SCOPE calls
// nest, tripping `-Wshadow -Werror` (found the first time this macro was ever instantiated -
// tests/foundation/tl_prof.test.cpp's `prof_scope_macro_matches_manual_begin_end` - matching
// docs/LESSONS.md's "a template with no call site has never been compiled").
#define TL_PROF_CONCAT_(a, b) a##b
#define TL_PROF_CONCAT(a, b) TL_PROF_CONCAT_(a, b)

#if TL_DEV
#  define TL_PROF_BEGIN(lit)            tl_prof_begin(0, lit##_id, lit, 0xFFFFFFFFu)
#  define TL_PROF_END()                 tl_prof_end(0)
#  define TL_PROF_SCOPE_IMPL(lit, uniq) for (u32 TL_PROF_CONCAT(_tl_ps, uniq) = (TL_PROF_BEGIN(lit), 0u); TL_PROF_CONCAT(_tl_ps, uniq) == 0u; TL_PROF_END(), TL_PROF_CONCAT(_tl_ps, uniq) = 1u)
#  define TL_PROF_SCOPE(lit)            TL_PROF_SCOPE_IMPL(lit, __COUNTER__)
#  define TL_PROF_SCOPE_W_IMPL(scr, lit, job, uniq) for (u32 TL_PROF_CONCAT(_tl_ps, uniq) = (tl_prof_begin((scr)->worker, lit##_id, lit, (job)), 0u); TL_PROF_CONCAT(_tl_ps, uniq) == 0u; tl_prof_end((scr)->worker), TL_PROF_CONCAT(_tl_ps, uniq) = 1u)
#  define TL_PROF_SCOPE_W(scr, lit, job) TL_PROF_SCOPE_W_IMPL(scr, lit, job, __COUNTER__)
#  define TL_PROF_COUNTER_SET(lit, v)   tl_prof_counter(lit##_id, lit, (i64)(v), 0)
#  define TL_PROF_COUNTER_ADD(lit, v)   tl_prof_counter(lit##_id, lit, (i64)(v), 1)
#else
#  define TL_PROF_BEGIN(lit)            ((void)0)
#  define TL_PROF_END()                 ((void)0)
#  define TL_PROF_SCOPE_IMPL(lit, uniq) for (u32 TL_PROF_CONCAT(_tl_ps, uniq) = 0u; TL_PROF_CONCAT(_tl_ps, uniq) == 0u; TL_PROF_CONCAT(_tl_ps, uniq) = 1u)
#  define TL_PROF_SCOPE(lit)            TL_PROF_SCOPE_IMPL(lit, __COUNTER__)
#  define TL_PROF_SCOPE_W_IMPL(scr, lit, job, uniq) for (u32 TL_PROF_CONCAT(_tl_ps, uniq) = 0u; TL_PROF_CONCAT(_tl_ps, uniq) == 0u; TL_PROF_CONCAT(_tl_ps, uniq) = 1u)
#  define TL_PROF_SCOPE_W(scr, lit, job) TL_PROF_SCOPE_W_IMPL(scr, lit, job, __COUNTER__)
#  define TL_PROF_COUNTER_SET(lit, v)   ((void)0)
#  define TL_PROF_COUNTER_ADD(lit, v)   ((void)0)
#endif

#if TL_DEV

// docs/TOOLING.md §9.2, verbatim (48 B).
struct ProfNode {
    u64      t_begin, t_end;
    NameHash key;
    const char* name;
    u32      parent;   // index into the SAME worker's `nodes`, or PROF_NODE_NONE at depth 0
    u32      job_id;    // 0xFFFFFFFF = not a job-tagged scope (docs/TOOLING.md §9.1's TL_PROF_BEGIN)
    u16      depth;
    u8       worker;
    u8       _pad0;
    u32      _pad1;
};
static_assert(sizeof(ProfNode) == 48, "docs/TOOLING.md section 9.2");

enum : u32 { PROF_NODE_NONE = 0xFFFFFFFFu, PROF_MAX_WORKERS = 16, PROF_RING_FRAMES = 60 };
enum : u32 { PROF_WORKER_NODES_CAP = 8192, PROF_STACK_CAP = 64, PROF_FRAME_NODES_CAP = 16384, PROF_COUNTERS_CAP = 256 };

// docs/TOOLING.md §9.2: per-worker scratch the current frame accumulates into before the
// frame-end merge. 8192 nodes * 48 B + a 64-deep begin/end stack + count/depth/overflow.
struct ProfWorker {
    ProfNode nodes[PROF_WORKER_NODES_CAP];
    u32      count;
    u32      stack[PROF_STACK_CAP];
    u32      depth;
    u32      overflow;   // scopes dropped past PROF_WORKER_NODES_CAP or PROF_STACK_CAP
};

// docs/TOOLING.md §9.2: one ring slot - every worker's nodes merged in worker order at frame end.
struct ProfFrame {
    u64      frame, tick, t_start, t_end;
    u32      node_count;
    u32      dropped;    // nodes past PROF_FRAME_NODES_CAP, counted not stored
    i64      counters[PROF_COUNTERS_CAP];
    ProfNode nodes[PROF_FRAME_NODES_CAP];
};
static_assert(sizeof(ProfFrame) == 788520, "docs/TOOLING.md section 9.2");

// docs/TOOLING.md §9.2 (24 B).
struct ProfCounter { NameHash key; const char* name; i64 value; };
static_assert(sizeof(ProfCounter) == 24, "docs/TOOLING.md section 9.2");

// docs/TOOLING.md §9.3.1. `worker` is 0 for TL_PROF_SCOPE (main thread by contract); job_id
// 0xFFFFFFFF for a non-job-tagged scope. Never allocates; a full worker or stack increments
// `overflow` and pushes PROF_NODE_NONE as a sentinel (tl_prof_end recognizes it and skips the
// t_end write) rather than corrupting the stack.
void tl_prof_begin(u8 worker, NameHash key, const char* name, u32 job_id);
// Pops `worker`'s stack; writes t_end unless the popped entry is the overflow sentinel.
void tl_prof_end(u8 worker);
// `add` false = COUNTER_SET (value replaces), true = COUNTER_ADD (value accumulates). A new key
// inserts (fatal past PROF_COUNTERS_CAP, matching tl_probe_log's "insert: fatal" precedent).
void tl_prof_counter(NameHash key, const char* name, i64 value, u8 add);

// Merges every worker's current-frame nodes into `ring[head]` in worker order (`parent` rebased
// by each worker's node-count offset within the merged frame), samples counters, advances
// `head`/`count`, resets every worker's count/depth/overflow to 0. `TL_ASSERT(workers[0].depth
// == 0)` - a TL_PROF_SCOPE that returned out of its block instead of falling through left the
// stack unbalanced (docs/TOOLING.md §9.1's "a `return` inside one is a review error").
// `tick`/`t_start` are the caller's own bookkeeping values (FRAME-LOOP.md's tick counter and this
// header's placeholder `ticks()` - not read internally).
void tl_prof_frame_end(u64 tick);

// Test-only introspection (guarded by TL_DEV, matching tl_log.h/tl_probe.h's precedent).
// The number of complete, ready-to-read ring frames (capped at PROF_RING_FRAMES once wrapped).
u32 tl_prof_test_ring_count(void);
// The ring frame `slots_back` frames before the most recently completed one (0 = the latest).
// Fatal if slots_back >= tl_prof_test_ring_count().
const ProfFrame* tl_prof_test_ring_at(u32 slots_back);
// The registered counter count (registration order).
u32 tl_prof_test_counter_count(void);
// The counter at `slot` (registration order). Fatal if slot >= tl_prof_test_counter_count().
const ProfCounter* tl_prof_test_counter_at(u32 slot);
// Clears every worker, the ring, and every counter to their zero-initialised state.
void tl_prof_test_reset(void);

#endif  // TL_DEV
