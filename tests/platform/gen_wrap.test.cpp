// gen_wrap.test.cpp - the four hand-rolled headless slot tables survive generation wrap.
//
// Not a docs/PLATFORM.md §9.6 row: §9.6 never reuses a slot more than a handful of times, so the
// wrap was invisible to the whole suite. TexHandle/ThreadHandle/SemHandle/MutexHandle are all
// Handle<Tag,12,4> - FOUR generation bits, GEN_MAX 15 - and each table's release path used a bare
// `++gen`. The 16th reuse of ONE slot therefore left gen == 16: handle_make TL_ASSERTs
// `gen <= GEN_MAX` in a dev tier, and with asserts compiled out the bit falls off the u16 rep, so
// handle_gen reads back 0 - the freshly created resource is instantly stale, and on slot 0 the
// handle IS the null handle. thread_primitives already reached gen 6 on thread slot 0, so this
// was six cycles of headroom, not a theoretical edge.
//
// Every loop below runs > 2 x GEN_MAX iterations against the FIRST free slot (nothing else is
// alive, so create always picks index 0), which is what forces the wrap.
#include "runner/tl_test.h"
#include "platform_test_util.h"
#include "foundation/handle.h"

namespace {

enum : u32 { CYCLES = 40u };   // > 2 x GEN_MAX (15), so the wrap happens at least twice

void noop_worker(void*) {}

}  // namespace

TL_TEST(gen_wrap_texture_slot_survives_reuse, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const DrawApi& d = api->draw;

    for (u32 i = 0; i < CYCLES; ++i) {
        Result<TexHandle> r = d.texture_create(d.ctx, 4u, 4u, PIXFMT_RGBA8, TEX_TARGET);
        TL_ASSERT_EQ(r.err, ERR_OK);
        TL_ASSERT_FALSE(handle_is_null(r.value));            // never the null handle
        TL_ASSERT_GE(handle_gen(r.value), 1u);               // generation 0 is never issued
        TL_ASSERT_LE(handle_gen(r.value), TexHandle::GEN_MAX);
        // the handle a wrapped slot issues must actually resolve, not read back stale
        TL_ASSERT_EQ(d.set_target(d.ctx, r.value), ERR_OK);
        TL_ASSERT_EQ(d.set_target(d.ctx, TexHandle{}), ERR_OK);
        d.texture_destroy(d.ctx, r.value);
        TL_ASSERT_EQ(d.live_textures(d.ctx), 0u);
        // and the destroyed handle is stale again immediately
        TL_ASSERT_EQ(d.set_target(d.ctx, r.value), (ErrCode)ERR_PLATFORM_TEX_STALE);
    }

    platform_test_shutdown(api);
}

TL_TEST(gen_wrap_mutex_and_sem_slots_survive_reuse, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const ThreadApi& th = api->thread;

    for (u32 i = 0; i < CYCLES; ++i) {
        Result<MutexHandle> m = th.mutex_create(th.ctx);
        TL_ASSERT_EQ(m.err, ERR_OK);
        TL_ASSERT_FALSE(handle_is_null(m.value));
        TL_ASSERT_IN_RANGE(handle_gen(m.value), 1u, MutexHandle::GEN_MAX);
        th.mutex_lock(th.ctx, m.value);     // resolves to a live OS mutex, or this deadlocks/no-ops
        th.mutex_unlock(th.ctx, m.value);
        th.mutex_destroy(th.ctx, m.value);

        Result<SemHandle> s = th.sem_create(th.ctx, 1u);
        TL_ASSERT_EQ(s.err, ERR_OK);
        TL_ASSERT_FALSE(handle_is_null(s.value));
        TL_ASSERT_IN_RANGE(handle_gen(s.value), 1u, SemHandle::GEN_MAX);
        // initial count 1, so try_wait must succeed - a stale handle would return 0 instead
        TL_ASSERT_EQ(th.sem_try_wait(th.ctx, s.value), 1u);
        th.sem_destroy(th.ctx, s.value);
    }

    platform_test_shutdown(api);
}

TL_TEST(gen_wrap_thread_slot_survives_reuse, "platform,slow") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const ThreadApi& th = api->thread;

    for (u32 i = 0; i < CYCLES; ++i) {
        Result<ThreadHandle> r = th.create(th.ctx, noop_worker, nullptr, sv("wrap"), 0u);
        TL_ASSERT_EQ(r.err, ERR_OK);
        TL_ASSERT_FALSE(handle_is_null(r.value));
        TL_ASSERT_IN_RANGE(handle_gen(r.value), 1u, ThreadHandle::GEN_MAX);
        th.join(th.ctx, r.value);   // a stale handle makes join a silent no-op and leaks the slot
    }
    // 40 create/join cycles must all have gone through the SAME slot: if join had ever failed to
    // resolve, the slot would still be alive and the 41st create would sit at index 40, not 0.
    Result<ThreadHandle> last = th.create(th.ctx, noop_worker, nullptr, sv("wrap"), 0u);
    TL_ASSERT_EQ(last.err, ERR_OK);
    TL_EXPECT_EQ(handle_index(last.value), 0u);
    th.join(th.ctx, last.value);

    platform_test_shutdown(api);
}
