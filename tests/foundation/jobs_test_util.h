#pragma once
// jobs_test_util.h - a fresh headless platform, per-worker scratch, and a Jobs per test
// (docs/TESTING.md §7 rubric #1), plus the one seeded workload every jobs test shares.
//
// The workload is deliberately chunk-KEYED and item-COUNTED: `partial[chunk]` is the only output
// slot a chunk may write, and `visit[i]` counts how many times each item was touched. A sum alone
// cannot see a scheduler bug - integer addition is order-free, so a chunk landing in the wrong
// slot still totals correctly (LESSONS.md). The buffer is compared byte for byte, and every
// visit must be exactly 1, which is what catches a permutation that runs a chunk twice.
#include "platform/platform.h"
#include "foundation/jobs.h"
#include "foundation/atomic.h"

#include <string.h>

struct JobsFix {
    const PlatformApi* api;
    Scratch  scratch[JOBS_MAX_WORKERS + 1];
    Scratch* ptr[JOBS_MAX_WORKERS + 1];
    VMemArena out;
    Jobs jobs;
    u32 workers;
    u32 _pad0;
};

// Brings up headless + `workers` scratch arenas + the pool. `out_bytes` reserves the arena the
// test's own output buffers come from (never a scratch: those belong to the workers).
inline ErrCode jobs_fix_init(JobsFix* f, u32 workers, u64 out_bytes) {
    memset(f, 0, sizeof(JobsFix));
    f->workers = workers;
    PlatformConfig cfg{};
    cfg.title = sv("tl_tests");
    cfg.org = sv("tidelock");
    cfg.app = sv("tests");
    f->api = platform_headless_init(&cfg);
    if (f->api == nullptr) { return ERR_JOBS_THREAD; }
    for (u32 i = 0u; i <= workers; ++i) {
        const ErrCode e = scratch_init(&f->scratch[i], (NameHash)(0x5000u + i), 1u << 20,
                                       &f->api->vmem);
        if (e != ERR_OK) { return e; }
        f->ptr[i] = &f->scratch[i];
    }
    const ErrCode ae = vmem_arena_init(&f->out, (NameHash)0x6000u, out_bytes, 0u, &f->api->vmem);
    if (ae != ERR_OK) { return ae; }
    JobsConfig jc{};
    jc.thread = &f->api->thread;
    jc.scratch = f->ptr;
    jc.scratch_count = workers + 1u;
    jc.worker_count = workers;
    jc.stack_bytes = 0u;
    jc._pad0 = 0u;
    return jobs_init(&f->jobs, &jc);
}

// Reverses jobs_fix_init exactly: the pool first (it joins its threads), then the arenas, then
// the platform. Safe on a partially-built fixture, which is what a failing init leaves.
inline void jobs_fix_shutdown(JobsFix* f) {
    jobs_shutdown(&f->jobs);
    if (f->api == nullptr) { return; }
    const VMemApi* v = &f->api->vmem;
    if (f->out.base != nullptr) { v->release(v->ctx, f->out.base, f->out.reserved); }
    for (u32 i = 0u; i <= f->workers; ++i) {
        if (f->scratch[i].a.base != nullptr) {
            v->release(v->ctx, f->scratch[i].a.base, f->scratch[i].a.reserved);
        }
    }
    platform_headless_shutdown(f->api);
    f->api = nullptr;
}

// A pure function of (i, seed) - no clock, no address, no worker. The value an item contributes.
inline u64 jobs_item_value(u32 i, u64 seed) {
    u64 x = seed + (u64)i * 0x9e3779b97f4a7c15ull;
    x ^= x >> 29; x *= 0xbf58476d1ce4e5b9ull; x ^= x >> 32;
    return x;
}

struct SumWork {
    u64* partial;      // [chunks] - the ONLY output slot, keyed by chunk id
    u8*  visit;        // [n] - times each item was touched; every entry must end at exactly 1
    u32* trace;        // [chunks] or null: trace[claim position] = chunk id
    u64  seed;
    u32  claim_index;  // atomic, test-owned
    u32  n;
    u32  chunks;
    u32  _pad0;
};

// The shared ChunkFn. Allocates from the scratch it was handed (inside a scope, so successive
// chunks on one worker do not grow it), writes ONLY partial[chunk], and never reads any state
// keyed by which worker it happens to be on.
inline void jobs_sum_chunk(void* ctx, u32 chunk, u32 begin, u32 end, Scratch* s) {
    SumWork* w = (SumWork*)ctx;
    TL_SCRATCH_SCOPE_BEGIN(s);
    u64* acc = (u64*)scratch_push(s, 64u, 16u);
    acc[0] = 0u;
    for (u32 i = begin; i < end; ++i) {
        acc[0] += jobs_item_value(i, w->seed);
        w->visit[i] = (u8)(w->visit[i] + 1u);   // chunks own disjoint ranges: no race, by design
    }
    w->partial[chunk] = acc[0];
    // ACQ_REL, previous value: the test's own claim-order recorder, not the pool's. Always
    // counted, so `claim_index == chunks` afterwards is a direct "every chunk ran exactly once".
    const u32 pos = atomic_add32(&w->claim_index, 1u);
    if (w->trace != nullptr) { w->trace[pos] = chunk; }
    TL_SCRATCH_SCOPE_END(s);
}

// The oracle: the same chunking, computed serially with no pool at all, consuming the same
// jobs_chunk_count. Fills `out[0..chunks)` and bumps `visit`.
inline void jobs_sum_reference(u64* out, u8* visit, u32 n, u32 grain, u64 seed) {
    const u32 c = jobs_chunk_count(n, grain);
    for (u32 k = 0u; k < c; ++k) {
        const u64 b = (u64)k * (u64)grain;
        const u64 e = (b + (u64)grain) < (u64)n ? (b + (u64)grain) : (u64)n;
        u64 sum = 0u;
        for (u64 i = b; i < e; ++i) {
            sum += jobs_item_value((u32)i, seed);
            visit[i] = (u8)(visit[i] + 1u);
        }
        out[k] = sum;
    }
}

// A second, independent oracle that knows nothing about chunk boundaries: a flat sum over
// [0, n). If the chunking itself drops or doubles a range, this disagrees and the per-chunk
// comparison alone would not have.
inline u64 jobs_flat_total(u32 n, u64 seed) {
    u64 sum = 0u;
    for (u32 i = 0u; i < n; ++i) { sum += jobs_item_value(i, seed); }
    return sum;
}
