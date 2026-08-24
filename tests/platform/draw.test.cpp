// draw.test.cpp - docs/PLATFORM.md §9.6 headless_draw_validates.
#include "runner/tl_test.h"
#include "platform_test_util.h"
#include "platform/impl_headless/headless_test_api.h"

TL_TEST(headless_draw_bad_args, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const DrawApi& d = api->draw;

    TL_EXPECT_EQ(d.texture_create(d.ctx, 0u, 16u, PIXFMT_RGBA8, TEX_STATIC).err, (ErrCode)ERR_PLATFORM_TEX_BAD_ARG);
    TL_EXPECT_EQ(d.texture_create(d.ctx, 16u, 0u, PIXFMT_RGBA8, TEX_STATIC).err, (ErrCode)ERR_PLATFORM_TEX_BAD_ARG);
    TL_EXPECT_EQ(d.texture_create(d.ctx, 8193u, 16u, PIXFMT_RGBA8, TEX_STATIC).err, (ErrCode)ERR_PLATFORM_TEX_BAD_ARG);

    Result<TexHandle> stat = d.texture_create(d.ctx, 4u, 4u, PIXFMT_RGBA8, TEX_STATIC);
    TL_ASSERT_EQ(stat.err, ERR_OK);
    TL_EXPECT_EQ(d.texture_lock(d.ctx, stat.value, nullptr).err, (ErrCode)ERR_PLATFORM_TEX_USAGE);   // lock on non-streaming

    Result<TexHandle> stream = d.texture_create(d.ctx, 4u, 4u, PIXFMT_RGBA8, TEX_STREAMING);
    TL_ASSERT_EQ(stream.err, ERR_OK);
    TL_EXPECT_EQ(d.texture_upload(d.ctx, stream.value, nullptr, 0u), (ErrCode)ERR_PLATFORM_TEX_USAGE);   // upload on non-static

    // a NULL handle means "the window" (§9.2), not stale - set_target(null) succeeds.
    TL_EXPECT_EQ(d.set_target(d.ctx, TexHandle{}), ERR_OK);

    d.texture_destroy(d.ctx, stat.value);
    TL_EXPECT_EQ(d.set_target(d.ctx, stat.value), (ErrCode)ERR_PLATFORM_TEX_STALE);   // destroyed handle -> TEX_STALE

    // m % 3 != 0
    TL_EXPECT_EQ(d.draw_geometry(d.ctx, TexHandle{}, nullptr, 0u, nullptr, 4u), (ErrCode)ERR_PLATFORM_TEX_BAD_ARG);
    // null vertex with n > 0
    TL_EXPECT_EQ(d.draw_geometry(d.ctx, TexHandle{}, nullptr, 3u, nullptr, 0u), (ErrCode)ERR_PLATFORM_TEX_BAD_ARG);

    platform_test_shutdown(api);
}

TL_TEST(headless_draw_streaming_lock_roundtrips_pixels, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const DrawApi& d = api->draw;

    Result<TexHandle> tex = d.texture_create(d.ctx, 2u, 2u, PIXFMT_RGBA8, TEX_STREAMING);
    TL_ASSERT_EQ(tex.err, ERR_OK);
    u32 pitch = 0u;
    Result<u8*> lock = d.texture_lock(d.ctx, tex.value, &pitch);
    TL_ASSERT_EQ(lock.err, ERR_OK);
    TL_ASSERT_NOT_NULL(lock.value);
    TL_EXPECT_EQ(pitch, 8u);   // 2 px * 4 B
    for (u32 i = 0; i < pitch * 2u; ++i) { lock.value[i] = (u8)(i + 1u); }
    d.texture_unlock(d.ctx, tex.value);

    // a second lock returns the SAME buffer (streaming textures own one CPU buffer, §9.4)
    Result<u8*> lock2 = d.texture_lock(d.ctx, tex.value, nullptr);
    TL_ASSERT_EQ(lock2.err, ERR_OK);
    TL_EXPECT_EQ(lock2.value[0], 1u);
    TL_EXPECT_EQ(lock2.value[pitch * 2u - 1u], (u8)(pitch * 2u));
    d.texture_unlock(d.ctx, tex.value);

    platform_test_shutdown(api);
}

TL_TEST(headless_draw_log_records_verbs_in_order, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const DrawApi& d = api->draw;

    d.clear(d.ctx, 0xFFu);
    Result<TexHandle> tex = d.texture_create(d.ctx, 4u, 4u, PIXFMT_RGBA8, TEX_STATIC);
    TL_ASSERT_EQ(tex.err, ERR_OK);
    TL_EXPECT_EQ(d.draw_geometry(d.ctx, TexHandle{}, nullptr, 0u, nullptr, 0u), ERR_OK);

    Span<const DrawCall> log = headless_draw_log(api);
    TL_ASSERT_EQ(log.count, 3u);
    TL_EXPECT_EQ(log.data[0].verb, (u8)DRAW_VERB_CLEAR);
    TL_EXPECT_EQ(log.data[1].verb, (u8)DRAW_VERB_TEX_CREATE);
    TL_EXPECT_EQ(log.data[2].verb, (u8)DRAW_VERB_DRAW_GEOMETRY);

    d.present(d.ctx);
    Span<const DrawCall> after_present = headless_draw_log(api);
    TL_EXPECT_EQ(after_present.count, 0u);   // "cleared by present" (§9.4)

    platform_test_shutdown(api);
}

TL_TEST(headless_draw_texture_limit_and_stale, "platform,slow") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const DrawApi& d = api->draw;

    TexHandle handles[4096];
    for (u32 i = 0; i < 4096u; ++i) {
        Result<TexHandle> r = d.texture_create(d.ctx, 1u, 1u, PIXFMT_RGBA8, TEX_STATIC);
        TL_ASSERT_EQ(r.err, ERR_OK);
        handles[i] = r.value;
    }
    TL_EXPECT_EQ(d.live_textures(d.ctx), 4096u);
    Result<TexHandle> overflow = d.texture_create(d.ctx, 1u, 1u, PIXFMT_RGBA8, TEX_STATIC);
    TL_EXPECT_EQ(overflow.err, (ErrCode)ERR_PLATFORM_TEX_LIMIT);

    d.texture_destroy(d.ctx, handles[0]);
    TL_EXPECT_EQ(d.live_textures(d.ctx), 4095u);
    u16 w = 99u, h = 99u;
    d.texture_size(d.ctx, handles[0], &w, &h);   // stale: writes 0,0
    TL_EXPECT_EQ(w, 0u);
    TL_EXPECT_EQ(h, 0u);

    for (u32 i = 1; i < 4096u; ++i) { d.texture_destroy(d.ctx, handles[i]); }
    platform_test_shutdown(api);
}
