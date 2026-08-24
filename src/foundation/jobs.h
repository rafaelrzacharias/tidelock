#pragma once
// ---------------------------------------------------------------------------------------------
// jobs.h - the worker pool: chunk-keyed parallel_for / parallel_levels. NON-DET stem; the
//   RESULTS it produces are bit-identical at every worker count, which is the whole point.
//
// Spec: docs/JOBS.md §0 (the one rule), §1 (API), §2 (option A: fixed pool + atomic chunk
//   counter), §5 R-1/R-2, §6 (this implementation spec). Consumers: docs/ALLOY.md §8.1 (colored
//   Gauss-Seidel on parallel_levels), docs/ECS.md §3 (reads/writes groups, chunk = system),
//   docs/FRAME-LOOP.md §3 step 4 (jobs_scratch_reset_all at the barrier), docs/NETCODE.md §2
//   (INV-7) and §19 S-02 (the 1/2/8/16 gate), docs/RENDER2D.md §4 (the chunk-parallel writer).
// Purpose: split [0, n) into chunks by a PURE FUNCTION of (n, grain) and run them on a fixed
//   pool. Worker identity never reaches a result: per-chunk outputs land in chunk-indexed slots
//   and folds run in chunk-index order, so work-stealing and arrival order are free (docs/JOBS.md
//   §0). Scheduling affects wall time only.
// Invariants: (1) grain is a per-call-site constant, never derived from worker count (R-1) -
//   deriving it makes chunk BOUNDARIES worker-dependent, which shuffle mode exists to catch;
//   (2) jobs never nest - a chunk fn calling parallel_for is TL_FATAL, all tiers; (3) parallel_for
//   is main-thread-only and returns only after every chunk has run (it is its own barrier);
//   (4) a Jobs is PINNED once initialised - the worker slots hold a back-pointer into it, so it
//   must not be memcpy'd, moved or reallocated while live; (5) worker scratch is reset at the
//   barrier by the caller, never inside a job (docs/JOBS.md §6.3).
// Determinism: outputs keyed by CHUNK id, never worker id (docs/DETERMINISM.md §2 rule 5).
//   Nothing here is hashed, registered or snapshotted. The atomics decide only WHICH worker runs
//   a chunk. Identical results at 1/2/8/16 workers and under shuffle is a blocking release gate
//   (docs/JOBS.md §3).
// Threading: this IS the threading module. jobs_init/jobs_shutdown/parallel_for/parallel_levels/
//   jobs_shuffle_set/jobs_scratch_reset_all are MAIN-THREAD-ONLY (they call the ThreadApi
//   create/destroy verbs, which are not thread-safe - docs/PLATFORM.md §9.2). A ChunkFn runs on
//   an arbitrary worker and may allocate only from the Scratch it is handed.
// Includes: foundation/{tl_types,tl_assert,thread_api,scratch}.h.
// ---------------------------------------------------------------------------------------------
#if defined(TL_SIM_TU)
#error "foundation/jobs.h is non-det (docs/JOBS.md section 6.1) and must not be reached from a sim or det TU: it owns threads and atomics. Sim code is HANDED a chunk (ctx, chunk, begin, end, scratch); it never drives the pool."
#endif

#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/thread_api.h"
#include "foundation/scratch.h"

// docs/JOBS.md §6.2 spells this MAX_WORKERS; in code it carries the module prefix, like
// SCRATCH_MAX_SCOPES and COMMIT_GRANULE - `MAX_WORKERS` is too ordinary a name for a header every
// module can reach. 31 workers + the calling thread = 32 participants.
enum : u32 { JOBS_MAX_WORKERS = 31 };

// The jobs module's ErrCode range is 0x02xx (mem holds 0x01xx, foundation/vmem_arena.h).
constexpr ErrCode ERR_JOBS_BAD_ARG = (ErrCode)0x0201;  // null table, too few scratch slots, worker_count > JOBS_MAX_WORKERS
constexpr ErrCode ERR_JOBS_THREAD  = (ErrCode)0x0202;  // the platform refused a thread or a semaphore

// Log-side name for a jobs ErrCode; returns "ERR_?" for codes outside the jobs range.
constexpr const char* err_jobs_name(ErrCode e) {
    return e == ERR_OK           ? "ERR_OK"
         : e == ERR_JOBS_BAD_ARG ? "ERR_JOBS_BAD_ARG"
         : e == ERR_JOBS_THREAD  ? "ERR_JOBS_THREAD"
         : "ERR_?";
}

// One chunk of a parallel_for. `chunk` is the chunk INDEX - the only key an output may be stored
// under. [begin, end) is its half-open slice of [0, n). `scratch` is the running worker's arena:
// allocate from it freely, never store a pointer into it past the call, never key anything on
// which arena you were handed (docs/JOBS.md §0, docs/MEMORY.md §1.3).
typedef void (*ChunkFn)(void* ctx, u32 chunk, u32 begin, u32 end, Scratch* scratch);

// As ChunkFn, plus the level index. docs/JOBS.md §6.3's pseudocode passes "(ctx, l)" through a
// ChunkFn that has no level parameter, which is not expressible - this typedef is that fix,
// folded back into §6.2 in the same commit. `chunk` is still the only output key; `level` says
// which sequential level (colour, phase) is running.
typedef void (*LevelFn)(void* ctx, u32 level, u32 chunk, u32 begin, u32 end, Scratch* scratch);

// One sequential level of parallel_levels: `count` items, chunked by `grain` (R-1: a constant of
// the call site, never of the worker count). A level with count == 0 runs nothing and is legal.
struct Level { u32 count; u32 grain; };
static_assert(__is_trivially_copyable(Level), "");

// What jobs_init needs. `scratch` is an array of `scratch_count` Scratch pointers, index 0 being
// the CALLING thread's (it participates as a worker - R-2); scratch_count must be at least
// worker_count + 1. `stack_bytes` 0 takes the platform default (1 MB).
struct JobsConfig {
    const ThreadApi* thread;
    Scratch* const*  scratch;
    u32 scratch_count;
    u32 worker_count;      // 0 is legal and tested: everything runs inline on the calling thread
    u32 stack_bytes;
    u32 _pad0;
};
static_assert(__is_trivially_copyable(JobsConfig), "");

// The job currently published, snapshotted by each participant under the epoch acquire before it
// claims anything (docs/JOBS.md §6.3). `shuffle_key` 0 means "claim in counter order"; non-zero
// is dev shuffle mode (§3) and permutes the claim order through an index bijection.
struct JobDesc {
    ChunkFn fn;
    void*   ctx;
    u64     shuffle_key;
    u32     n;
    u32     grain;
    u32     chunks;
    u32     _pad0;
};
static_assert(__is_trivially_copyable(JobDesc), "");

struct Jobs;

// A worker's identity, passed to its ThreadFn. Holds the back-pointer that pins a live Jobs
// (invariant 4). `index` is 1..worker_count; index 0 is the calling thread and owns no slot.
struct JobsWorker { Jobs* jobs; u32 index; u32 _pad0; };
static_assert(__is_trivially_copyable(JobsWorker), "");

// The pool. Caller-allocated (foundation holds no static mutable state - docs/CPP-SUBSET.md §1),
// pinned once initialised. The three hot atomics sit 64 bytes apart so a claim on one cannot
// invalidate another's cache line; the separation is structural, and the contention soak
// (tests/foundation/jobs.test.cpp) is what measures whether it earned its 180 bytes.
struct Jobs {
    const ThreadApi* thread;
    Scratch*     scratch[JOBS_MAX_WORKERS + 1];   // [0] = the calling thread's
    JobsWorker   slots[JOBS_MAX_WORKERS];
    ThreadHandle threads[JOBS_MAX_WORKERS];
    SemHandle    wake[JOBS_MAX_WORKERS];          // one PER WORKER: a single shared semaphore lets
    SemHandle    done;                            // one worker eat another's token and hang the pool
    u64          shuffle_seed;
    u32          worker_count;
    u32          _pad0;
    JobDesc      job;
    // epoch is u64, not u32 (W1 jobs review): the inline path advances it without waking a
    // worker, so a worker's `seen` can lag by any number of jobs - after exactly 2^32 inline
    // jobs between two pooled ones, a u32 epoch equals `seen` again and the worker parks on a
    // REAL token (the stale-post guard cannot tell a wrapped epoch from a stale one), which is a
    // permanent hang. 2^64 does not wrap in a machine's lifetime.
    u64 epoch;       u32 _line0[14];   // atomic: published RELEASE, acquired by every worker
    u32 next_chunk;  u32 _line1[15];   // atomic: the claim ticket
    u32 pending;     u32 _line2[15];   // atomic: participants still inside their claim loop
    u32 shutdown;                      // atomic
    u32 in_job;                        // atomic: the nested-parallel_for tripwire
};
static_assert(__is_trivially_copyable(Jobs), "");
static_assert(offsetof(Jobs, epoch) % 8 == 0, "atomics must be naturally aligned");
static_assert(offsetof(Jobs, next_chunk) - offsetof(Jobs, epoch) == 64, "epoch and next_chunk must not share a cache line");
static_assert(offsetof(Jobs, pending) - offsetof(Jobs, next_chunk) == 64, "next_chunk and pending must not share a cache line");

// The chunk count for (n, grain): a PURE function of its two arguments and of nothing else - not
// of worker count, timing or arrival (docs/JOBS.md §0, R-1). n == 0 is 0 chunks. Computed in 64
// bits because (n + grain - 1) overflows u32 for n near U32_MAX. TL_FATAL on grain == 0, all
// tiers: a zero grain is a call-site bug, not a recoverable condition. Callers size their
// per-chunk partials array with this and fold it 0..count-1, in index order.
inline u32 jobs_chunk_count(u32 n, u32 grain) {
    if (grain == 0u) { TL_FATAL("jobs: grain must be non-zero (docs/JOBS.md R-1)"); }
    if (n == 0u) { return 0u; }
    return (u32)(((u64)n + (u64)grain - 1u) / (u64)grain);
}

// R-2's pool policy, in one place: min(core_count - 1, JOBS_MAX_WORKERS). Call it BEFORE
// jobs_init to size the scratch array; there is no "auto" sentinel in JobsConfig, so the count a
// pool runs at is always a value the caller can see. The clamp is not in the rev-1 spec: without
// it a 32-core box writes past threads[30] on its first init (folded into docs/JOBS.md §6.2).
u32 jobs_default_worker_count(const ThreadApi* thread);

// Starts the pool: creates worker_count threads and their semaphores, all from the calling
// thread (the ThreadApi create verbs are not thread-safe). Returns ERR_JOBS_BAD_ARG for a null
// table, worker_count > JOBS_MAX_WORKERS, or scratch_count < worker_count + 1; ERR_JOBS_THREAD if
// the platform refuses a thread or semaphore (already-created ones are torn down first - there is
// no partial pool). On ERR_OK the Jobs is live and pinned. Never call inside a tick. PRECONDITION,
// not detected: `j` must not be a LIVE pool (jobs_shutdown first) - a caller-allocated struct is
// indistinguishable from garbage, so init memsets it and a live pool's threads would be orphaned.
ErrCode jobs_init(Jobs* j, const JobsConfig* cfg);

// Stops the pool: signals every worker, joins them, destroys the semaphores. Idempotent on a
// zeroed or already-shut-down Jobs. Main-thread-only, and never with a job in flight.
void jobs_shutdown(Jobs* j);

// Runs fn over [0, n) split into jobs_chunk_count(n, grain) chunks, and RETURNS ONLY WHEN EVERY
// CHUNK HAS COMPLETED - it is its own barrier, and every write a chunk made is visible to the
// caller afterwards. The calling thread participates. n == 0 runs nothing; worker_count == 0 or a
// single chunk runs inline (no wake, no barrier, and the job never reaches shared state) - but
// still through the ticket->chunk mapping, so shuffle mode reorders the inline path too. TL_FATAL
// if called from inside a chunk fn (jobs never nest) or with a null fn. Main-thread-only.
// Allocates nothing.
void parallel_for(Jobs* j, u32 n, u32 grain, ChunkFn fn, void* ctx);

// Runs `level_count` levels STRICTLY IN ORDER, each a parallel_for over levels[l] - so every
// write made in level l is visible to every chunk of level l+1. This is the colored Gauss-Seidel
// host (docs/ALLOY.md §8.1: colours become levels) and the ECS group host (docs/ECS.md §3).
// level_count == 0 runs nothing; a level with count == 0 is skipped, not an error. TL_FATAL on a
// null fn or a null levels pointer with level_count > 0. Main-thread-only. Allocates nothing.
void parallel_levels(Jobs* j, u32 level_count, const Level* levels, LevelFn fn, void* ctx);

// The pool's resolved worker count (excluding the calling thread). Pure, any thread.
u32 jobs_worker_count(const Jobs* j);

// Dev shuffle mode (docs/JOBS.md §3): a non-zero seed makes each job claim its chunks through a
// seeded index bijection instead of in counter order, and jitters completion order, so a result
// keyed by worker or by arrival CHANGES and a correct one cannot. Legally non-deterministic by
// design - its own PRNG, never a sim RNG, never keyed on a tick. seed 0 turns it off. The seed is
// the caller's so a failing shuffle run can be replayed exactly. Main-thread-only, no job in
// flight. Compiled into EVERY tier, not just dev: it costs one predictable branch per chunk claim
// and is inert at seed 0, whereas an #if would leave the mode untested in the tier that ships and
// would make its test tier-conditional (LESSONS.md: a test whose #if mirrors the header's cannot
// fail on the branch the current tier does not take).
void jobs_shuffle_set(Jobs* j, u64 seed);

// The Scratch of worker `w` (0 = the calling thread). For the BARRIER and the test harness only -
// worker-indexed access is exactly what a chunk fn may never do (docs/JOBS.md §0). TL_ASSERT on
// w > worker_count. Main-thread-only, no job in flight.
Scratch* jobs_scratch(Jobs* j, u32 w);

// Resets every participant's scratch: step 4 of the end-of-tick barrier (docs/FRAME-LOOP.md §3).
// Debug-poisons 0xDD (ARENA_POISON, docs/MEMORY.md §1.3). TL_ASSERT if any scratch has an open
// scope. Main-thread-only, no job in flight - never inside a job (docs/JOBS.md §6.3).
void jobs_scratch_reset_all(Jobs* j);
