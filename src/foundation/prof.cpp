// prof.cpp - the hierarchical per-frame profiler behind TL_PROF_* (docs/TOOLING.md §9.2, §9.3.1).
// Tooling plane (RR-7, docs/CPP-SUBSET.md §9 R-4): writable namespace-scope state is sanctioned
// here, matching probe.cpp/log.cpp's precedent (ProfState is ~53 MB - the ring alone is
// sizeof(ProfFrame)*60 - which is fine as zero-initialised .bss: pages commit lazily as scopes
// are actually recorded, and this stem is exempt from the writable-static ban regardless of size).
#include "foundation/tl_prof.h"

#include "foundation/tl_assert.h"

#include <string.h>

#if TL_DEV

namespace {

struct ProfState {
    ProfFrame ring[PROF_RING_FRAMES];
    u32 head;
    u32 count;
    ProfWorker workers[PROF_MAX_WORKERS];
    ProfCounter counters[PROF_COUNTERS_CAP];
    u32 counter_count;
    u8 paused;
};
ProfState g_prof = {};

// Placeholder monotonic counter (this header's contract block: ClockApi is not wired into
// foundation yet) - strictly increasing so t_end >= t_begin always holds structurally.
u64 g_clock = 0;
u64 now(void) { return ++g_clock; }

ProfCounter* find_counter(NameHash key) {
    for (u32 i = 0; i < g_prof.counter_count; ++i) {
        if (g_prof.counters[i].key == key) { return &g_prof.counters[i]; }
    }
    return nullptr;
}

}  // namespace

void tl_prof_begin(u8 worker, NameHash key, const char* name, u32 job_id) {
    TL_CHECK(worker < PROF_MAX_WORKERS);
    ProfWorker* w = &g_prof.workers[worker];
    TL_CHECK(w->depth < PROF_STACK_CAP);   // a genuinely unbalanced nest, not a soft overflow

    if (w->count == PROF_WORKER_NODES_CAP) {
        w->overflow += 1u;
        w->stack[w->depth++] = PROF_NODE_NONE;   // sentinel: tl_prof_end skips the t_end write
        return;
    }

    const u32 n = w->count++;
    ProfNode* node = &w->nodes[n];
    node->t_begin = now();
    node->t_end = 0;
    node->key = key;
    node->name = name;
    node->parent = (w->depth > 0u) ? w->stack[w->depth - 1u] : PROF_NODE_NONE;
    node->job_id = job_id;
    node->depth = (u16)w->depth;
    node->worker = worker;
    node->_pad0 = 0;
    node->_pad1 = 0;
    w->stack[w->depth++] = n;
}

void tl_prof_end(u8 worker) {
    TL_CHECK(worker < PROF_MAX_WORKERS);
    ProfWorker* w = &g_prof.workers[worker];
    TL_CHECK(w->depth > 0u);
    const u32 n = w->stack[--w->depth];
    if (n != PROF_NODE_NONE) { w->nodes[n].t_end = now(); }
}

void tl_prof_counter(NameHash key, const char* name, i64 value, u8 add) {
    ProfCounter* c = find_counter(key);
    if (c == nullptr) {
        TL_CHECK(g_prof.counter_count < PROF_COUNTERS_CAP);
        c = &g_prof.counters[g_prof.counter_count++];
        c->key = key;
        c->name = name;
        c->value = 0;
    }
    c->value = add ? (c->value + value) : value;
}

void tl_prof_frame_end(u64 tick) {
    ProfFrame* f = &g_prof.ring[g_prof.head];
    f->frame = g_prof.count;
    f->tick = tick;
    f->t_start = 0;
    f->t_end = now();
    f->node_count = 0;
    f->dropped = 0;

    for (u32 wi = 0; wi < PROF_MAX_WORKERS; ++wi) {
        ProfWorker* w = &g_prof.workers[wi];
        TL_ASSERT(wi != 0u || w->depth == 0u);   // "TL_ASSERT(workers[0].depth == 0)" (§9.3.1)
        const u32 base = f->node_count;   // this worker's nodes rebase `parent` by this offset
        for (u32 i = 0; i < w->count; ++i) {
            if (f->node_count >= PROF_FRAME_NODES_CAP) { f->dropped += 1u; continue; }
            ProfNode dst = w->nodes[i];
            if (dst.parent != PROF_NODE_NONE) { dst.parent += base; }
            f->nodes[f->node_count++] = dst;
        }
        w->count = 0;
        w->depth = 0;
        w->overflow = 0;
    }

    memset(f->counters, 0, sizeof(f->counters));
    for (u32 i = 0; i < g_prof.counter_count && i < PROF_COUNTERS_CAP; ++i) {
        f->counters[i] = g_prof.counters[i].value;
    }

    g_prof.head = (g_prof.head + 1u) % PROF_RING_FRAMES;
    if (g_prof.count < PROF_RING_FRAMES) { g_prof.count += 1u; }
}

u32 tl_prof_ring_count(void) { return g_prof.count; }

const ProfFrame* tl_prof_ring_at(u32 slots_back) {
    TL_CHECK(slots_back < g_prof.count);
    // head points at the NEXT write slot; the most recently completed frame is head-1.
    const u32 latest = (g_prof.head + PROF_RING_FRAMES - 1u) % PROF_RING_FRAMES;
    const u32 idx = (latest + PROF_RING_FRAMES - slots_back) % PROF_RING_FRAMES;
    return &g_prof.ring[idx];
}

u32 tl_prof_counter_count(void) { return g_prof.counter_count; }

const ProfCounter* tl_prof_counter_at(u32 slot) {
    TL_CHECK(slot < g_prof.counter_count);
    return &g_prof.counters[slot];
}

void tl_prof_test_reset(void) {
    // NOT `g_prof = ProfState{}` - that value-initializes a ~53 MB TEMPORARY ProfState (ring[60]
    // of ProfFrame alone is 60 * 788520 B) that an unoptimized (debug tier, -O0) build actually
    // materializes on the stack before assigning it - an instant stack overflow regardless of
    // platform (found via a real segfault on the debug tier CI leg, docs/LESSONS.md's "a
    // World-sized fixture on the stack" class, this time at 200x the size). memset zeroes the
    // existing static IN PLACE, no temporary.
    memset(&g_prof, 0, sizeof(g_prof));
    g_clock = 0;
}

#endif  // TL_DEV
