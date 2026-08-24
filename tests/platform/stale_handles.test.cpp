// stale_handles.test.cpp - the void-returning verbs fail loudly on a dangling handle.
//
// Not a docs/PLATFORM.md §9.6 row. §9.6 checks the Result-returning verbs' error codes
// (TEX_STALE, TEX_USAGE) and stops there, so the verbs that CANNOT return a code went out
// swallowing: sem_wait/sem_post/sem_try_wait/mutex_lock/mutex_unlock all returned quietly when
// the handle did not resolve, and texture_unlock validated nothing at all.
//
// Quiet is the worst answer available for these. A mutex_lock that resolves to nothing leaves the
// caller believing it holds exclusion it does not, and the corruption surfaces in a system that
// did nothing wrong; a swallowed sem_post turns a rendezvous into a hang on the other side. Both
// land arbitrarily far from the dangling handle that caused them. CLAUDE.md: fail loudly and
// explicitly, no silent fallbacks.
//
// Each case is its own expect-fatal child process (the runner's TL_TEST_EXPECT_FATAL), because a
// TL_FATAL ends the process by design.
#include "runner/tl_test.h"
#include "platform_test_util.h"

TL_TEST_EXPECT_FATAL(stale_mutex_lock_is_fatal, "platform,slow") {
    (void)t;
    const PlatformApi* api = platform_test_init();
    const ThreadApi& th = api->thread;
    Result<MutexHandle> m = th.mutex_create(th.ctx);
    th.mutex_destroy(th.ctx, m.value);
    th.mutex_lock(th.ctx, m.value);   // destroyed: the lock cannot be held, so this must not return
}

TL_TEST_EXPECT_FATAL(null_mutex_lock_is_fatal, "platform,slow") {
    (void)t;
    const PlatformApi* api = platform_test_init();
    const ThreadApi& th = api->thread;
    th.mutex_lock(th.ctx, MutexHandle{});   // there is no such thing as locking nothing
}

TL_TEST_EXPECT_FATAL(stale_sem_post_is_fatal, "platform,slow") {
    (void)t;
    const PlatformApi* api = platform_test_init();
    const ThreadApi& th = api->thread;
    Result<SemHandle> s = th.sem_create(th.ctx, 0u);
    th.sem_destroy(th.ctx, s.value);
    th.sem_post(th.ctx, s.value);   // a swallowed post is a hang somewhere else
}

TL_TEST_EXPECT_FATAL(stale_texture_unlock_is_fatal, "platform,slow") {
    (void)t;
    const PlatformApi* api = platform_test_init();
    const DrawApi& d = api->draw;
    Result<TexHandle> tex = d.texture_create(d.ctx, 2u, 2u, PIXFMT_RGBA8, TEX_STREAMING);
    d.texture_destroy(d.ctx, tex.value);
    d.texture_unlock(d.ctx, tex.value);
}

TL_TEST_EXPECT_FATAL(texture_unlock_on_non_streaming_is_fatal, "platform,slow") {
    (void)t;
    const PlatformApi* api = platform_test_init();
    const DrawApi& d = api->draw;
    Result<TexHandle> tex = d.texture_create(d.ctx, 2u, 2u, PIXFMT_RGBA8, TEX_STATIC);
    d.texture_unlock(d.ctx, tex.value);   // lock already refuses this with TEX_USAGE; so must unlock
}

// The tolerant half of the same contract, so "fail loudly" does not quietly become "fail on
// everything": destroy and join stay no-ops on a null or already-released handle, because
// double-release on a shutdown path is ordinary and DrawApi::texture_destroy documents it.
TL_TEST(destroy_and_join_tolerate_null_and_stale, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const ThreadApi& th = api->thread;
    const DrawApi& d = api->draw;

    th.join(th.ctx, ThreadHandle{});
    th.sem_destroy(th.ctx, SemHandle{});
    th.mutex_destroy(th.ctx, MutexHandle{});
    d.texture_destroy(d.ctx, TexHandle{});

    Result<MutexHandle> m = th.mutex_create(th.ctx);
    TL_ASSERT_EQ(m.err, ERR_OK);
    th.mutex_destroy(th.ctx, m.value);
    th.mutex_destroy(th.ctx, m.value);   // twice

    Result<TexHandle> tex = d.texture_create(d.ctx, 2u, 2u, PIXFMT_RGBA8, TEX_STATIC);
    TL_ASSERT_EQ(tex.err, ERR_OK);
    d.texture_destroy(d.ctx, tex.value);
    d.texture_destroy(d.ctx, tex.value);
    TL_EXPECT_EQ(d.live_textures(d.ctx), 0u);   // the second destroy did not double-decrement

    // texture_size is the ASK verb: 0x0 on a stale handle is an answer, not a swallowed error.
    u16 w = 99u, h = 99u;
    d.texture_size(d.ctx, tex.value, &w, &h);
    TL_EXPECT_EQ(w, 0u);
    TL_EXPECT_EQ(h, 0u);

    platform_test_shutdown(api);
}
