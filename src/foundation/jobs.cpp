// jobs.cpp - the worker pool (docs/JOBS.md §6.3, and §5 R-3..R-6, which are corrections to §6.3's
// rev-1 pseudocode: per-worker wake semaphores, a barrier that counts PARTICIPANTS rather than
// chunks, LevelFn, and the JOBS_MAX_WORKERS clamp).
//
// Memory ordering is stated at every atomic call site, because this file's correctness is the
// ordering and nothing else. foundation/atomic.h fixes the order per function (ACQUIRE loads,
// RELEASE stores, ACQ_REL read-modify-write), so each comment names which pairing the call is
// half of, not which constant was passed - there is no constant to pass.
//
// The two edges the whole design rests on:
//   PUBLISH   main writes j->job (plain) -> atomic_store64(&j->epoch) RELEASE
//             worker atomic_load64(&j->epoch) ACQUIRE -> reads j->job (plain). The release/
//             acquire pair is what makes those plain reads race-free.
//   RETIRE    a participant finishes its claim loop -> atomic_sub32(&j->pending) ACQ_REL
//             main observes pending == 0 (its own ACQ_REL sub returning 1, or the `done`
//             semaphore) -> only THEN may main rewrite j->job / j->next_chunk. R-4: this is the
//             edge rev 1 did not have, because it counted chunks, and the worker that zeroes a
//             chunk count is still inside its loop and will touch next_chunk once more.
#include "foundation/jobs.h"
#include "foundation/atomic.h"

#include <string.h>   // memset - sanctioned (docs/CPP-SUBSET.md §1)

namespace {

// The splitmix64 finalizer, used here as a plain mixing function for the DEBUG shuffle schedule.
// This is NOT the sim RNG: it is never keyed on a tick, never reaches sim state, and its whole
// purpose is to be legally non-deterministic run to run (docs/JOBS.md §3). docs/DETERMINISM.md
// §3's rng_for is the sim's, and lives in the det half where this file cannot reach it.
u64 mix64(u64 x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

// Smallest EVEN b with 2^b >= chunks, capped at 32. Even, because the Feistel below is balanced;
// the slack between 2^b and chunks is absorbed by cycle-walking, never by a fallback.
u32 domain_bits(u32 chunks) {
    u32 b = 1u;
    while (b < 32u && ((u64)1u << b) < (u64)chunks) { ++b; }
    if ((b & 1u) != 0u) { ++b; }
    return b > 32u ? 32u : b;
}

// One balanced 4-round Feistel over [0, 2^(2*half)). A Feistel network is a bijection for ANY
// round function, which is the only property that matters here: every ticket must map to a
// distinct chunk, or a chunk runs twice and another never runs.
u32 feistel(u32 x, u32 half, u64 key) {
    const u32 mask = (u32)(((u64)1u << half) - 1u);
    u32 l = (x >> half) & mask;
    u32 r = x & mask;
    for (u32 round = 0u; round < 4u; ++round) {
        const u32 f = (u32)(mix64(key ^ ((u64)round << 40) ^ (u64)r) & (u64)mask);
        const u32 nl = r;
        r = l ^ f;
        l = nl;
    }
    return (u32)(l << half) | r;
}

// ticket -> chunk: a permutation of [0, chunks) with NO permutation array, by cycle-walking the
// Feistel above. The walk terminates because the Feistel is a bijection on [0, 2^bits) and
// [0, chunks) is non-empty, so the orbit of any point must re-enter it; 2^bits < 4*chunks, so
// each step lands in range with probability above 1/4 and the expected walk is under 4 steps.
// The 1024 cap is a "this is broken" tripwire, not a correctness bound - the chance a correct
// walk exceeds it is under (3/4)^1024. Falling back to the identity would silently break
// bijectivity, which is the one thing this function may not do.
u32 shuffle_permute(u32 ticket, u32 chunks, u64 key) {
    if (chunks <= 1u) { return ticket; }
    const u32 half = domain_bits(chunks) / 2u;
    u32 y = ticket;
    for (u32 guard = 0u; guard < 1024u; ++guard) {
        y = feistel(y, half, key);
        if (y < chunks) { return y; }
    }
    TL_FATAL("jobs: shuffle cycle-walk did not converge (docs/JOBS.md section 6.3)");
}

// Perturbs COMPLETION order, not just claim order: docs/JOBS.md §3 asks shuffle to randomize
// both. A yield is a scheduler hint with no timing promise, which is exactly right - a sleep
// would be synchronization by wall clock, and this file has none.
void shuffle_jitter(const ThreadApi* th, u64 key, u32 ticket) {
    if ((mix64(key ^ ((u64)ticket << 17)) & 3u) == 0u) { th->yield(th->ctx); }
}

// Runs the chunk a ticket maps to. The ticket -> chunk indirection is the ONLY place shuffle
// lives, which is why both the pooled and the inline path go through here: a chunk-order
// dependency (a chunk fn reading a neighbour's output) is a real bug at worker_count 0 too, and
// an inline path that always ran 0,1,2,... could never expose it. It is also what makes the
// shuffle test non-racy - at worker_count 0 the claim order is a pure function of the seed.
void jobs_run_one(const JobDesc* job, u32 ticket, Scratch* scratch) {
    const u32 k = job->shuffle_key != 0u ? shuffle_permute(ticket, job->chunks, job->shuffle_key)
                                         : ticket;
    // k < chunks = ceil(n/grain), so k*grain < n <= U32_MAX: the u64 intermediate cannot overflow
    // and the casts back are exact.
    const u64 begin = (u64)k * (u64)job->grain;
    const u64 end = begin + (u64)job->grain;
    const u64 clamped = end < (u64)job->n ? end : (u64)job->n;
    job->fn(job->ctx, k, (u32)begin, (u32)clamped, scratch);
}

}  // namespace

// Claims and runs chunks until the ticket counter passes the job's chunk count, then retires.
// `w` is the participant index: 0 is the calling (main) thread, 1..worker_count are the workers.
// Only a worker retires here - main retires inside parallel_for, so the post/wait pairing is
// visible in one place (R-4).
static void jobs_claim_loop(Jobs* j, u32 w) {
    // PLAIN read of the published job, race-free by the PUBLISH edge above: a worker reached here
    // only after its epoch ACQUIRE, and main reached here having written these fields itself.
    // The snapshot is DEFENSE IN DEPTH, not load-bearing (measured, W1 jobs review: removing it
    // survives the whole suite, and the derivation agrees) - with a participant-counted barrier,
    // every read of j->job is sequenced before this participant's retire, and main rewrites it
    // only after observing every retire, so no re-read can overlap a rewrite. Kept because it
    // makes that argument unnecessary at every use site below, for 40 bytes of stack.
    const JobDesc job = j->job;
    Scratch* scratch = j->scratch[w];
    for (;;) {
        // ACQ_REL, returns the PREVIOUS value: this is the claim. Two participants can never
        // receive the same ticket, which is the entire mutual exclusion in this file.
        const u32 ticket = atomic_add32(&j->next_chunk, 1u);
        if (ticket >= job.chunks) { break; }
        jobs_run_one(&job, ticket, scratch);
        if (job.shuffle_key != 0u) { shuffle_jitter(j->thread, job.shuffle_key, ticket); }
    }
    if (w != 0u) {
        // RETIRE, release half: every write this worker made above happens-before whoever
        // observes the decremented value. Returns the PREVIOUS value, so == 1 means "I took
        // pending to zero". Only a worker posts, and only then; main waits iff its own decrement
        // did not return 1. Exactly one post per job and exactly one wait - so no stale `done`
        // token can survive into the next job and let a later parallel_for return early.
        if (atomic_sub32(&j->pending, 1u) == 1u) {
            j->thread->sem_post(j->thread->ctx, j->done);
        }
    }
}

// A worker thread's whole life. Parks on ITS OWN semaphore (R-3): with one shared semaphore, a
// worker that loops round and eats a second token starves a peer that then never wakes, and the
// barrier waits forever.
static void jobs_worker_main(void* raw) {
    JobsWorker* slot = (JobsWorker*)raw;
    Jobs* j = slot->jobs;
    const ThreadApi* th = j->thread;
    u64 seen = 0u;
    for (;;) {
        th->sem_wait(th->ctx, j->wake[slot->index - 1u]);
        // ACQUIRE, pairing with the RELEASE store in jobs_shutdown. Checked FIRST: a shutdown
        // post must never be mistaken for a job.
        if (atomic_load32(&j->shutdown) != 0u) { return; }
        // PUBLISH edge, acquire half - what makes the plain read of j->job in the claim loop
        // race-free. 64-bit so the guard below cannot mistake a wrapped epoch for a stale post
        // (jobs.h, the epoch field's comment).
        const u64 e = atomic_load64(&j->epoch);
        // A stale post (one left over from an earlier epoch) must NOT retire: main accounted for
        // exactly one retire per worker per epoch, so retiring twice would take pending below
        // zero. Parking again is safe because main posts once per worker per job, so this
        // worker's real post for the current epoch is still coming.
        if (e == seen) { continue; }
        seen = e;
        jobs_claim_loop(j, slot->index);
    }
}

u32 jobs_default_worker_count(const ThreadApi* thread) {
    TL_ASSERT(thread != nullptr);
    const u32 cores = thread->core_count(thread->ctx);
    const u32 workers = cores > 1u ? cores - 1u : 0u;
    // R-6: the clamp rev 1 never stated. Without it a 32-core machine writes past threads[30].
    return workers > (u32)JOBS_MAX_WORKERS ? (u32)JOBS_MAX_WORKERS : workers;
}

ErrCode jobs_init(Jobs* j, const JobsConfig* cfg) {
    if (j == nullptr || cfg == nullptr || cfg->thread == nullptr || cfg->scratch == nullptr) {
        return ERR_JOBS_BAD_ARG;
    }
    if (cfg->worker_count > (u32)JOBS_MAX_WORKERS) { return ERR_JOBS_BAD_ARG; }
    if (cfg->scratch_count < cfg->worker_count + 1u) { return ERR_JOBS_BAD_ARG; }
    memset(j, 0, sizeof(Jobs));
    for (u32 i = 0u; i <= cfg->worker_count; ++i) {
        if (cfg->scratch[i] == nullptr) { memset(j, 0, sizeof(Jobs)); return ERR_JOBS_BAD_ARG; }
        j->scratch[i] = cfg->scratch[i];
    }
    j->thread = cfg->thread;
    j->worker_count = cfg->worker_count;
    if (cfg->worker_count == 0u) {
        return ERR_OK;   // inline pool: no threads, no semaphores, nothing to tear down
    }

    const ThreadApi* th = cfg->thread;
    // Semaphores first, threads second: a semaphore failure then needs no thread teardown, and a
    // worker can never observe a half-built pool because it does not exist yet.
    Result<SemHandle> d = th->sem_create(th->ctx, 0u);
    if (d.err != ERR_OK) { memset(j, 0, sizeof(Jobs)); return ERR_JOBS_THREAD; }
    j->done = d.value;
    for (u32 w = 0u; w < cfg->worker_count; ++w) {
        Result<SemHandle> s = th->sem_create(th->ctx, 0u);
        if (s.err != ERR_OK) {
            for (u32 k = 0u; k < w; ++k) { th->sem_destroy(th->ctx, j->wake[k]); }
            th->sem_destroy(th->ctx, j->done);
            memset(j, 0, sizeof(Jobs));
            return ERR_JOBS_THREAD;
        }
        j->wake[w] = s.value;
    }
    for (u32 w = 0u; w < cfg->worker_count; ++w) {
        j->slots[w].jobs = j;
        j->slots[w].index = w + 1u;
        j->slots[w]._pad0 = 0u;
        Result<ThreadHandle> t = th->create(th->ctx, jobs_worker_main, &j->slots[w],
                                            sv("tl_worker"), cfg->stack_bytes);
        if (t.err != ERR_OK) {
            // No partial pool survives a failed init. RELEASE, pairing with each worker's ACQUIRE
            // load of shutdown; set before the posts, so every started worker sees 1 and returns.
            atomic_store32(&j->shutdown, 1u);
            for (u32 k = 0u; k < w; ++k) { th->sem_post(th->ctx, j->wake[k]); }
            for (u32 k = 0u; k < w; ++k) { th->join(th->ctx, j->threads[k]); }
            for (u32 k = 0u; k < cfg->worker_count; ++k) { th->sem_destroy(th->ctx, j->wake[k]); }
            th->sem_destroy(th->ctx, j->done);
            memset(j, 0, sizeof(Jobs));
            return ERR_JOBS_THREAD;
        }
        j->threads[w] = t.value;
    }
    return ERR_OK;
}

void jobs_shutdown(Jobs* j) {
    if (j == nullptr || j->thread == nullptr) { return; }   // idempotent on a zeroed/shut pool
    const ThreadApi* th = j->thread;
    TL_ASSERT(atomic_load32(&j->in_job) == 0u);             // never with a job in flight
    // RELEASE, pairing with the ACQUIRE load at the top of jobs_worker_main. Set BEFORE the posts
    // so a worker cannot wake, read 0, and park again on a semaphore about to be destroyed.
    atomic_store32(&j->shutdown, 1u);
    for (u32 w = 0u; w < j->worker_count; ++w) { th->sem_post(th->ctx, j->wake[w]); }
    for (u32 w = 0u; w < j->worker_count; ++w) { th->join(th->ctx, j->threads[w]); }
    for (u32 w = 0u; w < j->worker_count; ++w) { th->sem_destroy(th->ctx, j->wake[w]); }
    th->sem_destroy(th->ctx, j->done);
    j->thread = nullptr;
    j->worker_count = 0u;
}

void parallel_for(Jobs* j, u32 n, u32 grain, ChunkFn fn, void* ctx) {
    TL_ASSERT(j != nullptr);
    if (fn == nullptr) { TL_FATAL("jobs: parallel_for with a null chunk fn"); }
    // ACQUIRE. Non-zero means a job is already in flight, and since parallel_for is
    // main-thread-only the only way to reach here with one in flight is from inside a chunk fn.
    // Checked before anything else so the nesting report is not pre-empted by a grain fatal.
    if (atomic_load32(&j->in_job) != 0u) {
        TL_FATAL("jobs: nested parallel_for - jobs never nest (docs/JOBS.md section 6.2)");
    }
    const u32 c = jobs_chunk_count(n, grain);   // TL_FATALs on grain == 0, whatever n is
    if (c == 0u) { return; }
    TL_ASSERT(j->thread == nullptr || j->thread->is_main(j->thread->ctx) != 0u);

    // RELEASE. Published before any chunk fn runs, so a nested call from inside one sees it.
    atomic_store32(&j->in_job, 1u);

    // ACQUIRE. Main is the only writer of epoch, but reading it plainly beside a worker's atomic
    // load would be a second story about the same word. The epoch advances on EVERY job, inline
    // or pooled, so it is the single source of the per-job shuffle key.
    const u64 next_epoch = atomic_load64(&j->epoch) + 1u;
    // The key is per JOB, so two jobs with the same chunk count get different schedules. The low
    // bit is forced because 0 is the "shuffle off" sentinel in JobDesc.
    const u64 key = j->shuffle_seed != 0u ? (mix64(j->shuffle_seed + next_epoch) | 1u) : 0u;

    if (j->worker_count == 0u || c == 1u) {
        // Inline: no wake, no barrier, nothing shared to synchronize with - the job never reaches
        // j->job, so no worker can observe it. The epoch store keeps the key derivation uniform;
        // a worker that later acquires this epoch finds no post waiting and never runs it.
        const JobDesc local = { fn, ctx, key, n, grain, c, 0u };
        atomic_store64(&j->epoch, next_epoch);   // RELEASE
        Scratch* scratch = j->scratch[0];
        for (u32 t = 0u; t < c; ++t) { jobs_run_one(&local, t, scratch); }
    } else {
        const ThreadApi* th = j->thread;
        // PLAIN writes, legal only because RETIRE proved every participant of the PREVIOUS job
        // left its claim loop before that parallel_for returned; the RELEASE store of epoch below
        // publishes all of them at once (R-4).
        j->job.fn = fn;
        j->job.ctx = ctx;
        j->job.n = n;
        j->job.grain = grain;
        j->job.chunks = c;
        j->job._pad0 = 0u;
        j->job.shuffle_key = key;
        // RELEASE. Reset before the publish, and safe to reset only because of RETIRE.
        atomic_store32(&j->next_chunk, 0u);
        // RELEASE. PARTICIPANTS, not chunks (R-4): worker_count workers plus this thread. Every
        // worker decrements exactly once per epoch, after its claim loop has exited - so
        // pending == 0 means "no thread can touch next_chunk or j->job any more", which is the
        // property the next job's publish actually needs.
        atomic_store32(&j->pending, j->worker_count + 1u);
        // RELEASE, the PUBLISH edge: everything above becomes visible to any worker that acquires
        // this value.
        atomic_store64(&j->epoch, next_epoch);
        for (u32 w = 0u; w < j->worker_count; ++w) { th->sem_post(th->ctx, j->wake[w]); }

        jobs_claim_loop(j, 0u);   // the calling thread participates (R-2)

        // RETIRE for this thread. ACQ_REL: the acquire half joins the release sequence of every
        // worker's decrement, so if this returns 1 (main was last) main has already seen every
        // worker's writes and must NOT wait - no post is coming. Otherwise exactly one worker
        // will post, and this waits for exactly that one.
        if (atomic_sub32(&j->pending, 1u) != 1u) {
            th->sem_wait(th->ctx, j->done);
            // ACQUIRE of the 0 the last retire released. This is what carries every worker's
            // chunk writes into this thread under atomic.h's stated orders ALONE: main's own
            // decrement above synchronised only with the retires that PRECEDED it, and the ones
            // after it would otherwise be ordered only by the semaphore. Every real semaphore
            // does order its post/wait pair - but the ThreadApi contract does not say so
            // (ruling filed, TODO.md W1 jobs review), and one acquire load per pooled job makes
            // the barrier's correctness self-contained either way.
            const u32 p = atomic_load32(&j->pending);
            TL_ASSERT(p == 0u);
            (void)p;
        }
    }

    // RELEASE.
    atomic_store32(&j->in_job, 0u);
}

namespace {
// The level index reaches a LevelFn through this, on parallel_levels' own stack frame - which
// outlives the level because each parallel_for is a barrier (R-5).
struct LevelCtx { LevelFn fn; void* user; u32 level; u32 _pad0; };

void level_trampoline(void* raw, u32 chunk, u32 begin, u32 end, Scratch* scratch) {
    const LevelCtx* lc = (const LevelCtx*)raw;
    lc->fn(lc->user, lc->level, chunk, begin, end, scratch);
}
}  // namespace

void parallel_levels(Jobs* j, u32 level_count, const Level* levels, LevelFn fn, void* ctx) {
    TL_ASSERT(j != nullptr);
    if (fn == nullptr) { TL_FATAL("jobs: parallel_levels with a null level fn"); }
    if (level_count == 0u) { return; }
    if (levels == nullptr) { TL_FATAL("jobs: parallel_levels with a null levels array"); }
    for (u32 l = 0u; l < level_count; ++l) {
        LevelCtx lc = { fn, ctx, l, 0u };
        // Sequential by construction: parallel_for is its own barrier, so every write of level l
        // happens-before every chunk of level l+1 - through RETIRE, then the next PUBLISH.
        // An empty level runs no chunks; a zero grain still fatals, empty or not.
        parallel_for(j, levels[l].count, levels[l].grain, level_trampoline, &lc);
    }
}

u32 jobs_worker_count(const Jobs* j) {
    TL_ASSERT(j != nullptr);
    return j->worker_count;
}

void jobs_shuffle_set(Jobs* j, u64 seed) {
    TL_ASSERT(j != nullptr);
    TL_ASSERT(atomic_load32(&j->in_job) == 0u);   // ACQUIRE; never mid-job
    j->shuffle_seed = seed;
}

Scratch* jobs_scratch(Jobs* j, u32 w) {
    TL_ASSERT(j != nullptr);
    TL_ASSERT(w <= j->worker_count);
    TL_ASSERT(atomic_load32(&j->in_job) == 0u);   // ACQUIRE; worker-indexed access is barrier-only
    return j->scratch[w];
}

void jobs_scratch_reset_all(Jobs* j) {
    TL_ASSERT(j != nullptr);
    TL_ASSERT(atomic_load32(&j->in_job) == 0u);   // ACQUIRE; never inside a job (docs/JOBS.md §6.3)
    for (u32 w = 0u; w <= j->worker_count; ++w) {
        if (j->scratch[w] != nullptr) { scratch_reset(j->scratch[w]); }
    }
}
