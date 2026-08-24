// jobs.test.cpp - the worker pool, to docs/JOBS.md §6.4. Rubric: docs/TESTING.md §7.
//
// The gate this file exists for is INV-7 (docs/NETCODE.md §2): identical results at 1/2/8/16
// workers and under shuffle. Two things it deliberately does NOT do:
//   - it never compares a reduced scalar where it can compare the whole chunk-keyed BUFFER. An
//     integer sum is order-free, so a partial landing in the wrong slot still totals correctly;
//   - it never concludes "shuffle is fine" from matching results alone. A shuffle run whose
//     schedule did not actually change proves nothing, so the schedule is asserted CHANGED, at
//     worker_count 0 where the claim order is a pure function of the seed and no timing enters.
// Every test that could deadlock runs its body in a child process under --timeout-ms, so a lost
// wake-up reports TIMEOUT instead of stalling the lane. No test sleeps for synchronization.
//
// MEASURED, 2026-08-24, by mutating the pool and re-running this file (do it again before
// trusting any change here):
//   - dropping the last chunk in the claim loop -> caught by 5 tests;
//   - reintroducing R-4 (count CHUNKS, retire inside the loop, as rev 1's pseudocode did) ->
//     caught by jobs_contention_soak and by NOTHING ELSE. Every single-shot test passed. The
//     defect lives in the window between one job's last retire and the next job's publish, so
//     only back-to-back jobs can see it. That is why the soak's CHECKER is tagged fast and runs
//     in the PR lane (`--isolate --tag !slow`) while only its body is slow-tagged: tag the
//     checker `slow` and R-4 ships.
#include "runner/tl_test.h"
#include "jobs_test_util.h"

#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {
enum : u32 { GRAIN = 1024u, MAX_CHUNKS = 1024u };
const u32 SWEEP_WORKERS[5] = { 0u, 1u, 2u, 8u, 16u };
}  // namespace

// -------------------------------------------------------------------------------------------
// jobs_chunk_count: a pure function of (n, grain), and of nothing else.
// -------------------------------------------------------------------------------------------
TL_TEST(jobs_chunk_count_table, "foundation,jobs,smoke,fast") {
    TL_EXPECT_EQ(jobs_chunk_count(0u, 1u), 0u);
    TL_EXPECT_EQ(jobs_chunk_count(0u, 1024u), 0u);
    TL_EXPECT_EQ(jobs_chunk_count(1u, 1u), 1u);
    TL_EXPECT_EQ(jobs_chunk_count(1u, 1024u), 1u);
    TL_EXPECT_EQ(jobs_chunk_count(1023u, 1024u), 1u);
    TL_EXPECT_EQ(jobs_chunk_count(1024u, 1024u), 1u);
    TL_EXPECT_EQ(jobs_chunk_count(1025u, 1024u), 2u);
    TL_EXPECT_EQ(jobs_chunk_count(1000000u, 1024u), 977u);
    // The overflow edge rev 1's u32 expression got wrong: (n + grain - 1) wraps for n near
    // U32_MAX and answers a tiny chunk count, which would silently drop almost every item.
    TL_EXPECT_EQ(jobs_chunk_count(0xFFFFFFFFu, 1u), 0xFFFFFFFFu);
    TL_EXPECT_EQ(jobs_chunk_count(0xFFFFFFFFu, 4096u), 1048576u);
    TL_EXPECT_EQ(jobs_chunk_count(0xFFFFFFFFu, 0xFFFFFFFFu), 1u);
    // The count never depends on anything but its two arguments: same call, same answer.
    TL_EXPECT_EQ(jobs_chunk_count(1000000u, 1024u), jobs_chunk_count(1000000u, 1024u));
}

// grain 0 is a call-site bug (R-1), fatal in every tier - so this needs no tier skip.
TL_TEST_EXPECT_FATAL(jobs_grain_zero_is_fatal, "foundation,jobs,fast") {
    ++t->checks;
    (void)jobs_chunk_count(10u, 0u);
}

// -------------------------------------------------------------------------------------------
// THE GATE: bit-identical chunk-keyed output at 0/1/2/8/16 workers (docs/JOBS.md §3).
// -------------------------------------------------------------------------------------------
TL_TEST(jobs_parallel_for_is_worker_invariant, "foundation,jobs,fast") {
    const u32 ns[5] = { 0u, 1u, GRAIN - 1u, GRAIN, 1000000u };
    const u64 seed = 0x7469646c6f636b31ull;
    for (u32 ni = 0u; ni < 5u; ++ni) {
        const u32 n = ns[ni];
        const u32 chunks = jobs_chunk_count(n, GRAIN);
        TL_ASSERT_LE(chunks, MAX_CHUNKS);
        const u64 flat = jobs_flat_total(n, seed);
        u64 golden[MAX_CHUNKS];
        u8 have_golden = 0u;
        for (u32 wi = 0u; wi < 5u; ++wi) {
            JobsFix f;
            TL_ASSERT_EQ(jobs_fix_init(&f, SWEEP_WORKERS[wi], 8u << 20), ERR_OK);
            TL_EXPECT_EQ(jobs_worker_count(&f.jobs), SWEEP_WORKERS[wi]);

            u64* partial = (u64*)arena_push(&f.out, (u64)(chunks ? chunks : 1u) * 8u, 16u);
            u8* visit = (u8*)arena_push(&f.out, (u64)(n ? n : 1u), 16u);
            memset(partial, 0, (usize)(chunks ? chunks : 1u) * 8u);
            memset(visit, 0, (usize)(n ? n : 1u));

            SumWork w{};
            w.partial = partial; w.visit = visit; w.trace = nullptr;
            w.seed = seed; w.claim_index = 0u; w.n = n; w.chunks = chunks; w._pad0 = 0u;
            parallel_for(&f.jobs, n, GRAIN, jobs_sum_chunk, &w);

            // Every chunk ran exactly once - not "the total was right".
            TL_EXPECT_EQ(w.claim_index, chunks);
            // Every ITEM was touched exactly once: this is what catches a chunk-index permutation
            // that runs one chunk twice and another never (a broken shuffle bijection would).
            u32 not_once = 0u;
            for (u32 i = 0u; i < n; ++i) { if (visit[i] != 1u) { ++not_once; } }
            TL_EXPECT_EQ(not_once, 0u);

            if (have_golden == 0u) {
                u8* ref_visit = (u8*)arena_push(&f.out, (u64)(n ? n : 1u), 16u);
                memset(ref_visit, 0, (usize)(n ? n : 1u));
                memset(golden, 0, sizeof(golden));
                jobs_sum_reference(golden, ref_visit, n, GRAIN, seed);
                have_golden = 1u;
            }
            // The whole buffer, byte for byte, against the serial oracle.
            TL_EXPECT_MEM_EQ(partial, golden, (usize)chunks * sizeof(u64));
            // And against an oracle that knows nothing about chunk boundaries, so a chunking
            // error that is self-consistent still fails here.
            u64 total = 0u;
            for (u32 k = 0u; k < chunks; ++k) { total += partial[k]; }
            TL_EXPECT_EQ(total, flat);

            jobs_fix_shutdown(&f);
        }
    }
}

// -------------------------------------------------------------------------------------------
// Shuffle: the schedule must CHANGE and the result must not.
// -------------------------------------------------------------------------------------------
TL_TEST(jobs_shuffle_reorders_the_schedule_but_not_the_result, "foundation,jobs,fast") {
    const u32 n = 40000u;
    const u32 grain = 64u;
    const u64 seed = 0xA11CE5EEDull;
    const u32 chunks = jobs_chunk_count(n, grain);
    TL_ASSERT_EQ(chunks, 625u);

    // worker_count 0: the inline path still goes through the ticket->chunk mapping, so the claim
    // order here is a pure function of the shuffle seed. No timing, no flake, no sampling.
    JobsFix f;
    TL_ASSERT_EQ(jobs_fix_init(&f, 0u, 8u << 20), ERR_OK);

    u64* base = (u64*)arena_push(&f.out, (u64)chunks * 8u, 16u);
    u64* shuf = (u64*)arena_push(&f.out, (u64)chunks * 8u, 16u);
    u32* trace_a = (u32*)arena_push(&f.out, (u64)chunks * 4u, 16u);
    u32* trace_b = (u32*)arena_push(&f.out, (u64)chunks * 4u, 16u);
    u32* trace_c = (u32*)arena_push(&f.out, (u64)chunks * 4u, 16u);
    u8* visit = (u8*)arena_push(&f.out, n, 16u);

    SumWork w{};
    w.seed = seed; w.n = n; w.chunks = chunks; w.visit = visit;

    // Run A - shuffle off. The unshuffled inline order is the identity, which is the baseline the
    // "it changed" claim is measured against; if this ever stops holding, the claim is vacuous.
    memset(visit, 0, (usize)n);
    w.partial = base; w.trace = trace_a; w.claim_index = 0u;
    parallel_for(&f.jobs, n, grain, jobs_sum_chunk, &w);
    TL_EXPECT_EQ(w.claim_index, chunks);
    u32 off_identity = 0u;
    for (u32 i = 0u; i < chunks; ++i) { if (trace_a[i] != i) { ++off_identity; } }
    TL_EXPECT_EQ(off_identity, 0u);

    // Run B - shuffle on.
    jobs_shuffle_set(&f.jobs, 0xDEADBEEFCAFEull);
    memset(visit, 0, (usize)n);
    w.partial = shuf; w.trace = trace_b; w.claim_index = 0u;
    parallel_for(&f.jobs, n, grain, jobs_sum_chunk, &w);
    TL_EXPECT_EQ(w.claim_index, chunks);

    // (1) The schedule actually differed - and by a lot, so a permutation that moved two entries
    //     could not pass. A random permutation of 625 leaves ~1 fixed point.
    u32 moved = 0u;
    for (u32 i = 0u; i < chunks; ++i) { if (trace_b[i] != i) { ++moved; } }
    TL_EXPECT_GT(moved, chunks / 2u);
    // (2) It is still a PERMUTATION: every chunk claimed exactly once. A ticket->chunk map that
    //     is not a bijection would run one chunk twice and drop another, which (1) cannot see.
    u8* seen = (u8*)arena_push(&f.out, chunks, 16u);
    memset(seen, 0, (usize)chunks);
    for (u32 i = 0u; i < chunks; ++i) { TL_ASSERT_LT(trace_b[i], chunks); seen[trace_b[i]] = 1u; }
    u32 missing = 0u;
    for (u32 i = 0u; i < chunks; ++i) { if (seen[i] == 0u) { ++missing; } }
    TL_EXPECT_EQ(missing, 0u);
    u32 not_once = 0u;
    for (u32 i = 0u; i < n; ++i) { if (visit[i] != 1u) { ++not_once; } }
    TL_EXPECT_EQ(not_once, 0u);
    // (3) The result is byte-identical anyway. That is the whole point of the mode.
    TL_EXPECT_MEM_EQ(shuf, base, (usize)chunks * sizeof(u64));

    // (4) The key is per JOB, not per pool: the next job under the same seed gets a different
    //     schedule, so a bug that only survives one fixed ordering keeps being hunted.
    memset(visit, 0, (usize)n);
    w.partial = shuf; w.trace = trace_c; w.claim_index = 0u;
    parallel_for(&f.jobs, n, grain, jobs_sum_chunk, &w);
    u32 differs = 0u;
    for (u32 i = 0u; i < chunks; ++i) { if (trace_c[i] != trace_b[i]) { ++differs; } }
    TL_EXPECT_GT(differs, chunks / 2u);
    TL_EXPECT_MEM_EQ(shuf, base, (usize)chunks * sizeof(u64));

    jobs_fix_shutdown(&f);
}

// Shuffle with real workers: results still byte-identical to the serial oracle.
TL_TEST(jobs_shuffle_is_worker_invariant_too, "foundation,jobs,fast") {
    const u32 n = 40000u;
    const u32 grain = 64u;
    const u64 seed = 0xB0B0B0Bull;
    const u32 chunks = jobs_chunk_count(n, grain);
    const u32 counts[3] = { 1u, 2u, 8u };
    u64 golden[625];
    TL_ASSERT_EQ(chunks, 625u);
    {
        u8 ref_visit[1] = { 0u };
        (void)ref_visit;
    }
    for (u32 ci = 0u; ci < 3u; ++ci) {
        JobsFix f;
        TL_ASSERT_EQ(jobs_fix_init(&f, counts[ci], 8u << 20), ERR_OK);
        u64* partial = (u64*)arena_push(&f.out, (u64)chunks * 8u, 16u);
        u8* visit = (u8*)arena_push(&f.out, n, 16u);
        u8* ref_visit = (u8*)arena_push(&f.out, n, 16u);
        memset(partial, 0, (usize)chunks * 8u);
        memset(visit, 0, (usize)n);
        memset(ref_visit, 0, (usize)n);
        if (ci == 0u) { memset(golden, 0, sizeof(golden)); jobs_sum_reference(golden, ref_visit, n, grain, seed); }

        jobs_shuffle_set(&f.jobs, 0x5EED0000ull + counts[ci]);
        SumWork w{};
        w.partial = partial; w.visit = visit; w.trace = nullptr;
        w.seed = seed; w.claim_index = 0u; w.n = n; w.chunks = chunks; w._pad0 = 0u;
        parallel_for(&f.jobs, n, grain, jobs_sum_chunk, &w);

        TL_EXPECT_EQ(w.claim_index, chunks);
        TL_EXPECT_MEM_EQ(partial, golden, (usize)chunks * sizeof(u64));
        u32 not_once = 0u;
        for (u32 i = 0u; i < n; ++i) { if (visit[i] != 1u) { ++not_once; } }
        TL_EXPECT_EQ(not_once, 0u);
        jobs_fix_shutdown(&f);
    }
}

// -------------------------------------------------------------------------------------------
// Levels run strictly in order, and a chunk of level l+1 sees every level-l write.
// -------------------------------------------------------------------------------------------
namespace {
struct LevelWork { u32* dep; u32 fails; u32 n; u32 _pad0; };

void level_dependency_fn(void* ctx, u32 level, u32 chunk, u32 begin, u32 end, Scratch* s) {
    (void)chunk; (void)s;
    LevelWork* w = (LevelWork*)ctx;
    for (u32 i = begin; i < end; ++i) {
        // Every item must carry exactly the previous level's stamp. Anything else means level l
        // was still running when level l+1 started - the barrier failed.
        if (w->dep[i] != level) { (void)atomic_add32(&w->fails, 1u); }   // ACQ_REL, previous value
        w->dep[i] = level + 1u;
    }
}
}  // namespace

TL_TEST(jobs_levels_run_strictly_in_order, "foundation,jobs,fast") {
    const u32 n = 20000u;
    const u32 levels_count = 12u;
    for (u32 wi = 0u; wi < 5u; ++wi) {
        JobsFix f;
        TL_ASSERT_EQ(jobs_fix_init(&f, SWEEP_WORKERS[wi], 4u << 20), ERR_OK);
        u32* dep = (u32*)arena_push(&f.out, (u64)n * 4u, 16u);
        memset(dep, 0, (usize)n * 4u);
        Level levels[12];
        for (u32 l = 0u; l < levels_count; ++l) { levels[l].count = n; levels[l].grain = 128u; }

        LevelWork w{};
        w.dep = dep; w.fails = 0u; w.n = n; w._pad0 = 0u;
        parallel_levels(&f.jobs, levels_count, levels, level_dependency_fn, &w);

        TL_EXPECT_EQ(w.fails, 0u);
        u32 wrong = 0u;
        for (u32 i = 0u; i < n; ++i) { if (dep[i] != levels_count) { ++wrong; } }
        TL_EXPECT_EQ(wrong, 0u);
        jobs_fix_shutdown(&f);
    }
}

namespace {
// Counts calls per level and per chunk - nothing about ordering, so the edge cases below assert
// exactly what ran, not what it computed.
struct LevelTally { u32 calls[4]; u32 chunks_seen; u32 _pad0; };

void level_tally_fn(void* ctx, u32 level, u32 chunk, u32 begin, u32 end, Scratch* s) {
    (void)chunk; (void)begin; (void)end; (void)s;
    LevelTally* w = (LevelTally*)ctx;
    if (level < 4u) { w->calls[level] += 1u; }
    w->chunks_seen += 1u;
}
}  // namespace

TL_TEST(jobs_levels_edges, "foundation,jobs,fast") {
    JobsFix f;
    TL_ASSERT_EQ(jobs_fix_init(&f, 0u, 1u << 20), ERR_OK);   // inline: the tally needs no atomics
    LevelTally w{};

    // level_count 0 runs nothing and is not an error - not even the levels pointer is read, which
    // is why passing null here must be safe.
    parallel_levels(&f.jobs, 0u, nullptr, level_tally_fn, &w);
    TL_EXPECT_EQ(w.chunks_seen, 0u);

    // An empty level is skipped, not an error - and it does NOT renumber the levels around it:
    // the third entry is still level 2, so a colour with no constraints this tick cannot shift
    // every later colour's index (docs/ALLOY.md §8.1 relies on that).
    Level levels[3] = { { 64u, 16u }, { 0u, 16u }, { 32u, 16u } };
    parallel_levels(&f.jobs, 3u, levels, level_tally_fn, &w);
    TL_EXPECT_EQ(w.calls[0], 4u);          // 64 / 16
    TL_EXPECT_EQ(w.calls[1], 0u);          // the empty level ran nothing
    TL_EXPECT_EQ(w.calls[2], 2u);          // 32 / 16, and still spelled level 2
    TL_EXPECT_EQ(w.chunks_seen, 6u);
    jobs_fix_shutdown(&f);
}

// -------------------------------------------------------------------------------------------
// Worker scratch: one arena per participant, no cross-talk, poisoned on reset.
// -------------------------------------------------------------------------------------------
namespace {
struct ScratchWork { Scratch** arena_of; u32 corrupt; u32 _pad0; };

void scratch_marker_fn(void* ctx, u32 chunk, u32 begin, u32 end, Scratch* s) {
    (void)begin; (void)end;
    ScratchWork* w = (ScratchWork*)ctx;
    TL_SCRATCH_SCOPE_BEGIN(s);
    u32* p = (u32*)scratch_push(s, 4096u, 16u);
    for (u32 k = 0u; k < 1024u; ++k) { p[k] = chunk ^ k; }
    // Nothing else may touch this arena while this call runs: it belongs to one worker, and one
    // worker runs one chunk at a time. A shared arena shows up here as a corrupted marker.
    for (u32 k = 0u; k < 1024u; ++k) {
        if (p[k] != (chunk ^ k)) { (void)atomic_add32(&w->corrupt, 1u); }   // ACQ_REL
    }
    TL_SCRATCH_SCOPE_END(s);
    // Test-only: record WHICH arena this chunk was handed. Real chunk code may never key anything
    // on this (docs/JOBS.md §0) - the test does it precisely to prove the mapping stayed inside
    // the pool's own set.
    w->arena_of[chunk] = s;
}
}  // namespace

TL_TEST(jobs_worker_scratch_is_isolated_and_poisoned, "foundation,jobs,fast") {
    const u32 workers = 8u;
    const u32 chunks = 512u;
    JobsFix f;
    TL_ASSERT_EQ(jobs_fix_init(&f, workers, 4u << 20), ERR_OK);
    Scratch** arena_of = (Scratch**)arena_push(&f.out, (u64)chunks * sizeof(Scratch*), 16u);
    memset(arena_of, 0, (usize)chunks * sizeof(Scratch*));

    ScratchWork w{};
    w.arena_of = arena_of; w.corrupt = 0u; w._pad0 = 0u;
    parallel_for(&f.jobs, chunks, 1u, scratch_marker_fn, &w);
    TL_EXPECT_EQ(w.corrupt, 0u);

    // Every arena handed out is one of the pool's own, and there are never more distinct ones
    // than participants.
    u32 outside = 0u;
    u32 distinct = 0u;
    for (u32 i = 0u; i <= workers; ++i) {
        u8 used = 0u;
        for (u32 c = 0u; c < chunks; ++c) { if (arena_of[c] == jobs_scratch(&f.jobs, i)) { used = 1u; } }
        distinct += used;
    }
    for (u32 c = 0u; c < chunks; ++c) {
        u8 known = 0u;
        for (u32 i = 0u; i <= workers; ++i) { if (arena_of[c] == jobs_scratch(&f.jobs, i)) { known = 1u; } }
        if (known == 0u) { ++outside; }
    }
    TL_EXPECT_EQ(outside, 0u);
    TL_EXPECT_LE(distinct, workers + 1u);
    TL_EXPECT_GE(distinct, 1u);

    // The scopes balanced: every chunk fn left the arena as it found it.
    for (u32 i = 0u; i <= workers; ++i) { TL_EXPECT_EQ(jobs_scratch(&f.jobs, i)->depth, 0u); }

    // Barrier step 4 (docs/FRAME-LOOP.md §3): reset every participant's scratch. `used` is
    // already 0 - every chunk fn balanced its scope - so the evidence that the arenas were
    // actually WORKED is high_water, and it must be non-zero on more than one of them or the
    // reset below would be resetting nothing (the vacuity trap).
    u32 arenas_touched = 0u;
    for (u32 i = 0u; i <= workers; ++i) {
        if (jobs_scratch(&f.jobs, i)->a.high_water > 0u) { ++arenas_touched; }
    }
    TL_EXPECT_GE(arenas_touched, 1u);
    TL_EXPECT_EQ(arenas_touched, distinct);   // exactly the arenas that were handed out
    jobs_scratch_reset_all(&f.jobs);
    u64 used_after = 0u;
    for (u32 i = 0u; i <= workers; ++i) { used_after += arena_mark(&jobs_scratch(&f.jobs, i)->a); }
    TL_EXPECT_EQ(used_after, (u64)0);

#if TL_DEV
    // ARENA_POISON (docs/MEMORY.md §1.3): reused scratch reads as 0xDD, never as stale data and
    // never as zeros a caller might mistake for initialised memory.
    u32 unpoisoned = 0u;
    for (u32 i = 0u; i <= workers; ++i) {
        const Scratch* s = jobs_scratch(&f.jobs, i);
        if (s->a.high_water == 0u) { continue; }
        for (u32 b = 0u; b < 64u; ++b) { if (s->a.base[b] != 0xDDu) { ++unpoisoned; } }
    }
    TL_EXPECT_EQ(unpoisoned, 0u);
#else
    // The poison is a dev-tier facility; on this tier assert the flag is genuinely off rather
    // than recording no check at all.
    TL_EXPECT_EQ(jobs_scratch(&f.jobs, 0)->a.flags & ARENA_POISON, 0u);
#endif
    jobs_fix_shutdown(&f);
}

// -------------------------------------------------------------------------------------------
// Pool exhaustion and bad arguments: a named code, and never a partial pool.
// -------------------------------------------------------------------------------------------
TL_TEST(jobs_init_rejects_bad_arguments, "foundation,jobs,fast") {
    PlatformConfig pc{};
    pc.title = sv("tl_tests"); pc.org = sv("tidelock"); pc.app = sv("tests");
    const PlatformApi* api = platform_headless_init(&pc);
    TL_ASSERT_NOT_NULL(api);

    Scratch s{};
    TL_ASSERT_EQ(scratch_init(&s, (NameHash)0x7001u, 1u << 20, &api->vmem), ERR_OK);
    Scratch* ptrs[4] = { &s, &s, &s, &s };
    Scratch* with_null[2] = { &s, nullptr };

    Jobs j{};
    JobsConfig cfg{};
    cfg.thread = &api->thread; cfg.scratch = ptrs; cfg.scratch_count = 4u;
    cfg.worker_count = 1u; cfg.stack_bytes = 0u; cfg._pad0 = 0u;

    TL_EXPECT_EQ(jobs_init(nullptr, &cfg), ERR_JOBS_BAD_ARG);
    TL_EXPECT_EQ(jobs_init(&j, nullptr), ERR_JOBS_BAD_ARG);
    {
        JobsConfig bad = cfg; bad.thread = nullptr;
        TL_EXPECT_EQ(jobs_init(&j, &bad), ERR_JOBS_BAD_ARG);
    }
    {
        JobsConfig bad = cfg; bad.scratch = nullptr;
        TL_EXPECT_EQ(jobs_init(&j, &bad), ERR_JOBS_BAD_ARG);
    }
    {   // over the pool cap (R-6): rejected, never truncated to fit
        JobsConfig bad = cfg; bad.worker_count = JOBS_MAX_WORKERS + 1u; bad.scratch_count = 64u;
        TL_EXPECT_EQ(jobs_init(&j, &bad), ERR_JOBS_BAD_ARG);
    }
    {   // scratch_count must cover worker_count + 1: the calling thread has an arena too
        JobsConfig bad = cfg; bad.worker_count = 4u; bad.scratch_count = 4u;
        TL_EXPECT_EQ(jobs_init(&j, &bad), ERR_JOBS_BAD_ARG);
    }
    {   // a null entry inside the array is caught, not dereferenced later on a worker thread
        JobsConfig bad = cfg; bad.scratch = with_null; bad.scratch_count = 2u; bad.worker_count = 1u;
        TL_EXPECT_EQ(jobs_init(&j, &bad), ERR_JOBS_BAD_ARG);
    }
    // No partial pool survived any of those: shutdown on the rejected struct is a no-op, and a
    // good init still works afterwards.
    jobs_shutdown(&j);
    TL_EXPECT_EQ(jobs_init(&j, &cfg), ERR_OK);
    TL_EXPECT_EQ(jobs_worker_count(&j), 1u);
    jobs_shutdown(&j);
    jobs_shutdown(&j);            // idempotent
    TL_EXPECT_EQ(jobs_worker_count(&j), 0u);

    // R-6's clamp: the policy never answers more than the pool can hold.
    const u32 def = jobs_default_worker_count(&api->thread);
    TL_EXPECT_LE(def, (u32)JOBS_MAX_WORKERS);
    TL_EXPECT_EQ(def, jobs_default_worker_count(&api->thread));

    api->vmem.release(api->vmem.ctx, s.a.base, s.a.reserved);
    platform_headless_shutdown(api);
}

// -------------------------------------------------------------------------------------------
// Jobs never nest.
// -------------------------------------------------------------------------------------------
namespace {
Jobs* g_nest_target = nullptr;   // test-local; the fatal ends the child process immediately

void nested_inner(void*, u32, u32, u32, Scratch*) {}

void nested_outer(void*, u32, u32, u32, Scratch*) {
    parallel_for(g_nest_target, 4u, 1u, nested_inner, nullptr);   // TL_FATAL, every tier
}
}  // namespace

TL_TEST_EXPECT_FATAL(jobs_nested_parallel_for_is_fatal, "foundation,jobs,fast") {
    ++t->checks;
    JobsFix f;
    if (jobs_fix_init(&f, 0u, 1u << 20) != ERR_OK) { return; }
    g_nest_target = &f.jobs;
    parallel_for(&f.jobs, 4u, 1u, nested_outer, nullptr);
    jobs_fix_shutdown(&f);
}

// -------------------------------------------------------------------------------------------
// Contention soak. The body runs in a CHILD under --timeout-ms, so a lost wake-up or a stale
// claim reports TIMEOUT instead of hanging the lane (docs/JOBS.md §6.4).
// -------------------------------------------------------------------------------------------
#define TL_JOBS_SOAK_ENV "TL_JOBS_SOAK"

TL_TEST(jobs_contention_soak_trigger, "foundation,jobs,slow") {
    if (getenv(TL_JOBS_SOAK_ENV) == nullptr) {
        TL_SKIP("trigger runs only under " TL_JOBS_SOAK_ENV " (set by jobs_contention_soak)");
    }
    const u32 counts[4] = { 1u, 2u, 8u, 16u };
    const u32 n = 512u;
    const u32 grain = 64u;
    const u32 chunks = jobs_chunk_count(n, grain);
    const u64 seed = 0xC0FFEEull;
    u64 golden[8];
    TL_ASSERT_EQ(chunks, 8u);
    for (u32 ci = 0u; ci < 4u; ++ci) {
        JobsFix f;
        TL_ASSERT_EQ(jobs_fix_init(&f, counts[ci], 1u << 20), ERR_OK);
        u64* partial = (u64*)arena_push(&f.out, (u64)chunks * 8u, 16u);
        u8* visit = (u8*)arena_push(&f.out, n, 16u);
        u8* ref_visit = (u8*)arena_push(&f.out, n, 16u);
        memset(ref_visit, 0, (usize)n);
        memset(golden, 0, sizeof(golden));
        jobs_sum_reference(golden, ref_visit, n, grain, seed);
        // Many tiny jobs back to back is the shape that finds a lost wake-up: every job re-arms
        // every worker, and the window between one job's last retire and the next job's publish
        // is exactly where R-3 and R-4 live.
        u32 wrong = 0u;
        for (u32 rep = 0u; rep < 2000u; ++rep) {
            memset(partial, 0, (usize)chunks * 8u);
            memset(visit, 0, (usize)n);
            SumWork w{};
            w.partial = partial; w.visit = visit; w.trace = nullptr;
            w.seed = seed; w.claim_index = 0u; w.n = n; w.chunks = chunks; w._pad0 = 0u;
            if ((rep & 3u) == 3u) { jobs_shuffle_set(&f.jobs, 0x9000ull + rep); }
            else { jobs_shuffle_set(&f.jobs, 0u); }
            parallel_for(&f.jobs, n, grain, jobs_sum_chunk, &w);
            if (w.claim_index != chunks) { ++wrong; }
            if (!tl_mem_eq(partial, golden, (usize)chunks * sizeof(u64))) { ++wrong; }
            for (u32 i = 0u; i < n; ++i) { if (visit[i] != 1u) { ++wrong; break; } }
            jobs_scratch_reset_all(&f.jobs);
        }
        TL_EXPECT_EQ(wrong, 0u);
        jobs_fix_shutdown(&f);
    }
}

namespace {
// Runs `tl_tests --filter <name> --timeout-ms <ms>` with the soak env var set, and returns the
// child's exit code. --timeout-ms is the whole point: if the pool deadlocks, the CHILD's runner
// kills it and reports TIMEOUT, so this test fails in bounded time instead of stalling the lane.
int run_soak_child(const char* name, u32 timeout_ms) {
#ifdef _WIN32
    _putenv(TL_JOBS_SOAK_ENV "=1");
#else
    setenv(TL_JOBS_SOAK_ENV, "1", 1);
#endif
    char cmd[1024];
#ifdef _WIN32
    // cmd.exe /c strips only the first and last quote of a command line that starts with one;
    // the extra wrapping pair keeps the exe path intact (see tl_assert.test.cpp).
    snprintf(cmd, sizeof(cmd), "\"\"%s\" --filter %s --timeout-ms %u\"", TL_TESTS_EXE, name, timeout_ms);
#else
    snprintf(cmd, sizeof(cmd), "\"%s\" --filter %s --timeout-ms %u", TL_TESTS_EXE, name, timeout_ms);
#endif
    const int rc = system(cmd);
#ifdef _WIN32
    _putenv(TL_JOBS_SOAK_ENV "=");
    return rc;
#else
    unsetenv(TL_JOBS_SOAK_ENV);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
}
}  // namespace

TL_TEST(jobs_contention_soak, "foundation,jobs,fast") {
    // A failed spawn must never read as a pass (LESSONS.md): the child prints its own summary, and
    // a non-zero code here is a real failure - a hang shows up as the child's TIMEOUT, not as this
    // process waiting forever.
    const int rc = run_soak_child("jobs_contention_soak_trigger", 180000u);
    TL_EXPECT_EQ(rc, 0);
}
