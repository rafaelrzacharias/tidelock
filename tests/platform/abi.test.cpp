// abi.test.cpp - docs/PLATFORM.md §9.6 abi_and_layout. The layout half (sizeof(RawEvent) == 32,
// sizeof(DrawVertex) == 20, ...) is enforced by static_assert in platform.h at every build; this
// is the runtime half - a live headless PlatformApi has abi_version == 1 and every fn-ptr filled.
#include "runner/tl_test.h"
#include "platform_test_util.h"
#include "platform/entropy.h"

TL_TEST(abi_and_layout, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);

    TL_EXPECT_EQ(api->abi_version, PLATFORM_ABI_VERSION);
    TL_EXPECT_TRUE(api->is_headless != 0u);

    TL_EXPECT_NOT_NULL(api->window.size);
    TL_EXPECT_NOT_NULL(api->window.drawable_size);
    TL_EXPECT_NOT_NULL(api->window.set_fullscreen);
    TL_EXPECT_NOT_NULL(api->window.set_vsync);
    TL_EXPECT_NOT_NULL(api->window.set_title);
    TL_EXPECT_NOT_NULL(api->window.has_focus);

    TL_EXPECT_NOT_NULL(api->events.pump);
    TL_EXPECT_NOT_NULL(api->events.dropped_total);

    TL_EXPECT_NOT_NULL(api->draw.texture_create);
    TL_EXPECT_NOT_NULL(api->draw.texture_upload);
    TL_EXPECT_NOT_NULL(api->draw.texture_lock);
    TL_EXPECT_NOT_NULL(api->draw.texture_unlock);
    TL_EXPECT_NOT_NULL(api->draw.texture_size);
    TL_EXPECT_NOT_NULL(api->draw.texture_destroy);
    TL_EXPECT_NOT_NULL(api->draw.set_target);
    TL_EXPECT_NOT_NULL(api->draw.set_clip);
    TL_EXPECT_NOT_NULL(api->draw.clear);
    TL_EXPECT_NOT_NULL(api->draw.draw_geometry);
    TL_EXPECT_NOT_NULL(api->draw.present);
    TL_EXPECT_NOT_NULL(api->draw.live_textures);

    TL_EXPECT_NOT_NULL(api->file.read_all);
    TL_EXPECT_NOT_NULL(api->file.write_all);
    TL_EXPECT_NOT_NULL(api->file.write_atomic);
    TL_EXPECT_NOT_NULL(api->file.append);
    TL_EXPECT_NOT_NULL(api->file.exists);
    TL_EXPECT_NOT_NULL(api->file.enumerate);
    TL_EXPECT_NOT_NULL(api->file.base_path);
    TL_EXPECT_NOT_NULL(api->file.pref_path);
    TL_EXPECT_NOT_NULL(api->file.watch);
    TL_EXPECT_NOT_NULL(api->file.unwatch);

    TL_EXPECT_NOT_NULL(api->clock.ticks);
    TL_EXPECT_NOT_NULL(api->clock.frequency);
    TL_EXPECT_NOT_NULL(api->clock.wall_unix_ms);

    TL_EXPECT_NOT_NULL(api->vmem.reserve);
    TL_EXPECT_NOT_NULL(api->vmem.commit);
    TL_EXPECT_NOT_NULL(api->vmem.decommit);
    TL_EXPECT_NOT_NULL(api->vmem.release);

    TL_ASSERT_NOT_NULL(api->entropy);
    TL_EXPECT_NOT_NULL(api->entropy->fill);

    TL_EXPECT_NOT_NULL(api->thread.create);
    TL_EXPECT_NOT_NULL(api->thread.join);
    TL_EXPECT_NOT_NULL(api->thread.sem_create);
    TL_EXPECT_NOT_NULL(api->thread.sem_wait);
    TL_EXPECT_NOT_NULL(api->thread.sem_try_wait);
    TL_EXPECT_NOT_NULL(api->thread.sem_post);
    TL_EXPECT_NOT_NULL(api->thread.sem_destroy);
    TL_EXPECT_NOT_NULL(api->thread.mutex_create);
    TL_EXPECT_NOT_NULL(api->thread.mutex_lock);
    TL_EXPECT_NOT_NULL(api->thread.mutex_unlock);
    TL_EXPECT_NOT_NULL(api->thread.mutex_destroy);
    TL_EXPECT_NOT_NULL(api->thread.yield);
    TL_EXPECT_NOT_NULL(api->thread.sleep_ms);
    TL_EXPECT_NOT_NULL(api->thread.core_count);
    TL_EXPECT_NOT_NULL(api->thread.is_main);

    TL_EXPECT_NOT_NULL(api->crash.install);
    TL_EXPECT_NOT_NULL(api->crash.raise_fatal);

    platform_test_shutdown(api);
}
