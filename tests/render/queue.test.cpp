// queue.test.cpp - docs/RENDER2D.md §9.6 key_pack_unpack, clip_stack, reject.
#include "runner/tl_test.h"
#include "render_test_util.h"

static u32 lcg_next(u32* state) { *state = *state * 1664525u + 1013904223u; return *state; }

TL_TEST(key_pack_unpack, "render") {
    // 0
    {
        const u64 k = key_pack(0, 0, 0, 0, 0);
        TL_EXPECT_EQ(key_layer(k), (u8)0); TL_EXPECT_EQ(key_blend(k), (u8)0);
        TL_EXPECT_EQ(key_depth(k), 0u); TL_EXPECT_EQ(key_material(k), (u16)0); TL_EXPECT_EQ(key_tiebreak(k), (u16)0);
    }
    // max
    {
        const u64 k = key_pack(0xFF, 1, 0xFFFFFFu, 0xFFFFu, 0x7FFFu);
        TL_EXPECT_EQ(key_layer(k), (u8)0xFF); TL_EXPECT_EQ(key_blend(k), (u8)1);
        TL_EXPECT_EQ(key_depth(k), 0xFFFFFFu); TL_EXPECT_EQ(key_material(k), (u16)0xFFFF); TL_EXPECT_EQ(key_tiebreak(k), (u16)0x7FFF);
    }
    // random 10k sample - field masks do not overlap (the static_asserts in queue.h are the
    // compile-time half; this is the runtime half over a broad sample).
    u32 seed = 0xB16B00B5u;
    u32 tested = 0;
    for (u32 i = 0; i < 10000u; ++i) {
        const u8  layer    = (u8)(lcg_next(&seed) & 0xFFu);
        const u8  blend    = (u8)(lcg_next(&seed) & 1u);
        const u32 depth    = lcg_next(&seed) & 0xFFFFFFu;
        const u16 material = (u16)(lcg_next(&seed) & 0xFFFFu);
        const u16 tiebreak = (u16)(lcg_next(&seed) & 0x7FFFu);
        const u64 k = key_pack(layer, blend, depth, material, tiebreak);
        TL_EXPECT_EQ(key_layer(k), layer);
        TL_EXPECT_EQ(key_blend(k), blend);
        TL_EXPECT_EQ(key_depth(k), depth);
        TL_EXPECT_EQ(key_material(k), material);
        TL_EXPECT_EQ(key_tiebreak(k), tiebreak);
        ++tested;
    }
    TL_EXPECT_EQ(tested, 10000u);
}

TL_TEST(clip_stack, "render") {
    static RenderTestFixture f;
    TL_ASSERT_EQ(render_test_init(&f, 0, 0), ERR_OK);
    World* w = &f.world;

    TL_EXPECT_EQ(w->render->clips.depth, (u8)0);
    const u16 id1 = render_clip_push(w, Rect_i32{ 0, 0, 100, 100 });
    TL_EXPECT_EQ(id1, (u16)1);   // id 0 is reserved for "no clip" (docs/RENDER2D.md §9.2)
    const u16 id2 = render_clip_push(w, Rect_i32{ 10, 10, 50, 50 });
    TL_EXPECT_EQ(id2, (u16)2);
    TL_EXPECT_EQ(w->render->clips.depth, (u8)2);

    // submit stamps the TOP of the stack.
    render_submit(w, DrawCommand{ key_pack(0, 0, 0, 0, 0), 0, 0, 0 });
    TL_ASSERT_EQ(w->render->clip_id.count, 1u);
    TL_EXPECT_EQ(w->render->clip_id.data[0], id2);

    render_clip_pop(w);
    TL_EXPECT_EQ(w->render->clips.depth, (u8)1);
    render_submit(w, DrawCommand{ key_pack(0, 0, 0, 0, 0), 0, 0, 0 });
    TL_EXPECT_EQ(w->render->clip_id.data[1], id1);

    render_clip_pop(w);
    TL_EXPECT_EQ(w->render->clips.depth, (u8)0);
    render_submit(w, DrawCommand{ key_pack(0, 0, 0, 0, 0), 0, 0, 0 });
    TL_EXPECT_EQ(w->render->clip_id.data[2], (u16)0);   // id 0 when the stack is empty

    render_test_shutdown(&f);
}

TL_TEST_EXPECT_FATAL(clip_stack_depth_overflow, "render,fatal") {
    (void)t;
    static RenderTestFixture f;
    TL_ASSERT_EQ(render_test_init(&f, 0, 0), ERR_OK);
    World* w = &f.world;
    for (u32 i = 0; i < 32u; ++i) { render_clip_push(w, Rect_i32{ 0, 0, 10, 10 }); }
    render_clip_push(w, Rect_i32{ 0, 0, 10, 10 });   // depth 32 -> TL_FATAL
    render_test_shutdown(&f);
}

TL_TEST(reject, "render") {
    static RenderTestFixture f;
    TL_ASSERT_EQ(render_test_init(&f, 0, 0), ERR_OK);
    World* w = &f.world;
    w->render->view_world[0] = Rect_f32{ 0.0f, 0.0f, 100.0f, 100.0f };

    // fully outside the visible rect: rejected, no command added.
    render_draw_quad(w, LAYER_WORLD, DEPTH_SPRITE_BASE, 200.0f, 200.0f, 0.0f, 10.0f, 10.0f,
                     Rect_u16{ 0, 0, 16, 16 }, 0xFFFFFFFFu, TexHandle{}, 0);
    TL_EXPECT_EQ(w->render->stats_rejected, 1u);
    TL_EXPECT_EQ(w->render->stats_submitted, 0u);
    TL_EXPECT_EQ(w->render->keys.count, 0u);

    // touching the edge exactly (a zero-size quad centred ON the visible rect's right edge) is
    // kept - rect_visible is non-strict (docs/RENDER2D.md §9.3.4).
    render_draw_quad(w, LAYER_WORLD, DEPTH_SPRITE_BASE, 100.0f, 50.0f, 0.0f, 0.0f, 0.0f,
                     Rect_u16{ 0, 0, 16, 16 }, 0xFFFFFFFFu, TexHandle{}, 0);
    TL_EXPECT_EQ(w->render->stats_submitted, 1u);
    TL_EXPECT_EQ(w->render->stats_rejected, 1u);   // unchanged
    TL_EXPECT_EQ(w->render->keys.count, 1u);

    render_test_shutdown(&f);
}
