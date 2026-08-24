// thread.test.cpp - docs/PLATFORM.md §9.6 thread_primitives. Uses raw __atomic builtins directly
// (not foundation/atomic.h - the jobs lane owns that header and has not landed; this is a test's
// own counter, not a src/ deliverable, so borrowing the compiler intrinsic directly is in scope).
#include "runner/tl_test.h"
#include "platform_test_util.h"

namespace {

struct AddCtx { const ThreadApi* api; u32* counter; u32 iters; };

void add_worker(void* raw) {
    AddCtx* c = (AddCtx*)raw;
    for (u32 i = 0; i < c->iters; ++i) {
        __atomic_fetch_add(c->counter, 1u, __ATOMIC_SEQ_CST);
    }
}

struct PingPongCtx { const ThreadApi* api; SemHandle ping; SemHandle pong; u32 rounds; u32* tally; };

void pong_worker(void* raw) {
    PingPongCtx* c = (PingPongCtx*)raw;
    for (u32 i = 0; i < c->rounds; ++i) {
        c->api->sem_wait(c->api->ctx, c->ping);
        ++*c->tally;
        c->api->sem_post(c->api->ctx, c->pong);
    }
}

struct MutexCtx { const ThreadApi* api; MutexHandle m; u32* counter; u32 iters; };

void mutex_worker(void* raw) {
    MutexCtx* c = (MutexCtx*)raw;
    for (u32 i = 0; i < c->iters; ++i) {
        c->api->mutex_lock(c->api->ctx, c->m);
        ++*c->counter;
        c->api->mutex_unlock(c->api->ctx, c->m);
    }
}

void noop_worker(void*) {}

}  // namespace

TL_TEST(thread_primitives, "platform,slow") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const ThreadApi& th = api->thread;

    TL_EXPECT_GE(th.core_count(th.ctx), 1u);
    TL_EXPECT_TRUE(th.is_main(th.ctx) != 0u);

    // 8 threads x 100k atomic_add32 == 800k
    {
        enum { NT = 8, ITERS = 100000 };
        u32 counter = 0u;
        AddCtx ctxs[NT];
        ThreadHandle handles[NT];
        for (u32 i = 0; i < NT; ++i) {
            ctxs[i] = AddCtx{ &th, &counter, ITERS };
            Result<ThreadHandle> r = th.create(th.ctx, add_worker, &ctxs[i], sv("adder"), 0u);
            TL_ASSERT_EQ(r.err, ERR_OK);
            handles[i] = r.value;
        }
        for (u32 i = 0; i < NT; ++i) { th.join(th.ctx, handles[i]); }
        TL_EXPECT_EQ(counter, (u32)(NT * ITERS));
    }

    // semaphore ping-pong, 10k rounds
    {
        Result<SemHandle> ping = th.sem_create(th.ctx, 0u);
        Result<SemHandle> pong = th.sem_create(th.ctx, 0u);
        TL_ASSERT_EQ(ping.err, ERR_OK);
        TL_ASSERT_EQ(pong.err, ERR_OK);
        u32 tally = 0u;
        PingPongCtx ctx{ &th, ping.value, pong.value, 10000u, &tally };
        Result<ThreadHandle> worker = th.create(th.ctx, pong_worker, &ctx, sv("ponger"), 0u);
        TL_ASSERT_EQ(worker.err, ERR_OK);
        for (u32 i = 0; i < ctx.rounds; ++i) {
            th.sem_post(th.ctx, ping.value);
            th.sem_wait(th.ctx, pong.value);
        }
        th.join(th.ctx, worker.value);
        TL_EXPECT_EQ(tally, ctx.rounds);
        th.sem_destroy(th.ctx, ping.value);
        th.sem_destroy(th.ctx, pong.value);
    }

    // mutex-guarded counter, exact
    {
        Result<MutexHandle> m = th.mutex_create(th.ctx);
        TL_ASSERT_EQ(m.err, ERR_OK);
        enum { NT = 4, ITERS = 20000 };
        u32 counter = 0u;
        MutexCtx ctxs[NT];
        ThreadHandle handles[NT];
        for (u32 i = 0; i < NT; ++i) {
            ctxs[i] = MutexCtx{ &th, m.value, &counter, ITERS };
            Result<ThreadHandle> r = th.create(th.ctx, mutex_worker, &ctxs[i], sv("locker"), 0u);
            TL_ASSERT_EQ(r.err, ERR_OK);
            handles[i] = r.value;
        }
        for (u32 i = 0; i < NT; ++i) { th.join(th.ctx, handles[i]); }
        TL_EXPECT_EQ(counter, (u32)(NT * ITERS));
        th.mutex_destroy(th.ctx, m.value);
    }

    // join returns after the fn: a trivial thread must complete before join returns (implied by
    // every case above not hanging); explicit as its own check for a single quick thread.
    {
        Result<ThreadHandle> r = th.create(th.ctx, noop_worker, nullptr, sv("noop"), 0u);
        TL_ASSERT_EQ(r.err, ERR_OK);
        th.join(th.ctx, r.value);
        TL_EXPECT_TRUE(true);   // reaching here without hanging is the assertion
    }

    // 65th thread -> THREAD_LIMIT (HEADLESS_MAX_THREADS == 64)
    {
        ThreadHandle handles[64];
        u32 created = 0u;
        for (; created < 64u; ++created) {
            Result<ThreadHandle> r = th.create(th.ctx, noop_worker, nullptr, sv("filler"), 0u);
            if (r.err != ERR_OK) { break; }
            handles[created] = r.value;
        }
        TL_ASSERT_EQ(created, 64u);
        Result<ThreadHandle> overflow = th.create(th.ctx, noop_worker, nullptr, sv("overflow"), 0u);
        TL_EXPECT_EQ(overflow.err, (ErrCode)ERR_PLATFORM_THREAD_LIMIT);
        for (u32 i = 0; i < created; ++i) { th.join(th.ctx, handles[i]); }
    }

    TL_EXPECT_TRUE(th.is_main(th.ctx) != 0u);   // still the main thread at the end

    platform_test_shutdown(api);
}
