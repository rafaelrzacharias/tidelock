// backend_sdl.test.cpp - docs/RENDER2D.md §9.6 present_descriptor (numbers corrected against the
// real §9.4 algorithm in the same commit that fixed the doc row - TODO.md).
#include "runner/tl_test.h"
#include "render_test_util.h"
#include "render/render_internal.h"
#include "platform/impl_headless/headless_test_api.h"

TL_TEST(present_descriptor, "render") {
    static RenderTestFixture f;
    TL_ASSERT_EQ(render_test_init(&f, 64, 64), ERR_OK);   // WORLD gets an internal target; UI/DEBUG draw to the window
    World* w = &f.world;

    const Rect_u16 uv{ 0, 0, 16, 16 };
    const u32 d0 = render_push_data(w, 10.0f, 10.0f, 0.0f, 4.0f, 4.0f, uv, 0xFFFFFFFFu, TexHandle{}, DRAWFLAG_SCREEN_SPACE);
    render_submit(w, DrawCommand{ key_pack(LAYER_WORLD, 0, DEPTH_SPRITE_BASE, 0, 0), d0, 0, 0 });
    const u32 d1 = render_push_data(w, 20.0f, 20.0f, 0.0f, 4.0f, 4.0f, uv, 0xFFFFFFFFu, TexHandle{}, DRAWFLAG_SCREEN_SPACE);
    render_submit(w, DrawCommand{ key_pack(LAYER_UI, 0, 0, 0, 0), d1, 0, 0 });
    const u32 d2 = render_push_data(w, 30.0f, 30.0f, 0.0f, 4.0f, 4.0f, uv, 0xFFFFFFFFu, TexHandle{}, DRAWFLAG_SCREEN_SPACE);
    render_submit(w, DrawCommand{ key_pack(LAYER_DEBUG, 0, 0, 0, 0), d2, 0, 0 });

    // render_build_frame is everything up to (not including) present() - so the log is
    // inspectable before present()'s documented side effect clears it (platform/draw.test.cpp).
    render_build_frame(w);

    const Span<const DrawCall> log = headless_draw_log(f.platform);
    u32 n_set_target = 0, n_clear = 0, n_draw_geometry = 0;
    for (u32 i = 0; i < log.count; ++i) {
        if (log.data[i].verb == (u8)DRAW_VERB_SET_TARGET) { ++n_set_target; }
        else if (log.data[i].verb == (u8)DRAW_VERB_CLEAR) { ++n_clear; }
        else if (log.data[i].verb == (u8)DRAW_VERB_DRAW_GEOMETRY) {
            ++n_draw_geometry;
            TL_EXPECT_EQ(log.data[i].n, 4u);   // every draw_geometry call (batches and the blit alike) is one quad
        }
    }
    // top-level window + WORLD's own target + the WORLD blit's own window call + UI's window +
    // DEBUG's window (docs/RENDER2D.md §9.6, corrected).
    TL_EXPECT_EQ(n_set_target, 5u);
    // the top-level window clear + WORLD's own (UI/DEBUG have a null target, so §9.4's
    // `if target != null` guards their clear out).
    TL_EXPECT_EQ(n_clear, 2u);
    // 3 batches + 1 blit quad.
    TL_EXPECT_EQ(n_draw_geometry, 4u);

    // draw_geometry count == batch count (docs/RENDER2D.md §9.6) is this invariant, not the raw
    // log total - the blit is deliberately not one of "the batches".
    TL_EXPECT_EQ(w->render->stats_draw_calls, 3u);
    TL_EXPECT_EQ(w->render->stats_batches, 3u);
    TL_EXPECT_EQ(w->render->batches.count, 3u);

    // one present: its only observable effect is clearing the log (platform/draw.test.cpp
    // headless_draw_log_records_verbs_in_order). Called directly (not through render_present) so
    // this does not re-run render_build_frame a second, redundant time over the same data.
    f.platform->draw.present(f.platform->draw.ctx);
    const Span<const DrawCall> after_present = headless_draw_log(f.platform);
    TL_EXPECT_EQ(after_present.count, 0u);

    // render_present's step-7 reset: one fresh submission, one full call, queue state clears.
    const u32 d3 = render_push_data(w, 5.0f, 5.0f, 0.0f, 2.0f, 2.0f, uv, 0xFFFFFFFFu, TexHandle{}, DRAWFLAG_SCREEN_SPACE);
    render_submit(w, DrawCommand{ key_pack(LAYER_UI, 0, 0, 0, 0), d3, 0, 0 });
    render_present(w);
    TL_EXPECT_EQ(w->render->keys.count, 0u);
    TL_EXPECT_EQ(w->render->verts.count, 0u);
    TL_EXPECT_EQ(w->render->batches.count, 0u);
    TL_EXPECT_EQ(w->render->stats_submitted, 0u);
    TL_EXPECT_EQ(w->render->stats_rejected, 0u);

    render_test_shutdown(&f);
}
