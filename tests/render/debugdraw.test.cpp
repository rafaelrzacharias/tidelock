// debugdraw.test.cpp - docs/RENDER2D.md §9.3.8: immediate lines/rects/circles submit through
// render_draw_quad (no DrawApi call anywhere in debugdraw.cpp), and the persistent ring.
#include "runner/tl_test.h"
#include "render/debugdraw.h"
#include "render/text.h"
#include "foundation/vmem_test_api.h"
#include <string.h>
#include <math.h>

struct DebugDrawFixture {
    VMemApi api;
    VMemArena render_arena;
    World w;
    RenderQueue rq;
};

// A caller-owned static (docs/LESSONS.md - the World-sized-fixture stack crash).
static DebugDrawFixture* dd_fixture() { static DebugDrawFixture f; return &f; }

static ErrCode dd_init(DebugDrawFixture* f) {
    f->api = test_vmem_api();
    const ErrCode e = vmem_arena_init(&f->render_arena, "dd.render_arena"_id, 4u * 1024u * 1024u, 0, &f->api);
    if (e != ERR_OK) { return e; }
    memset(&f->w, 0, sizeof(f->w));
    memset(&f->rq, 0, sizeof(f->rq));
    // > DBG_PERSIST_RING_CAP (4096) so a full-ring-wrap replay test can submit every live entry
    // in one render_present-less draw-command batch (review round 2 N11).
    const u32 cap = 8192u;
    array_init_fixed(&f->rq.keys, &f->render_arena, cap);
    array_init_fixed(&f->rq.data_index, &f->render_arena, cap);
    array_init_fixed(&f->rq.clip_id, &f->render_arena, cap);
    array_init_fixed(&f->rq.order, &f->render_arena, cap);
    array_init_fixed(&f->rq.batches, &f->render_arena, cap);
    f->rq.data.x = (f32*)arena_push(&f->render_arena, cap * sizeof(f32), alignof(f32));
    f->rq.data.y = (f32*)arena_push(&f->render_arena, cap * sizeof(f32), alignof(f32));
    f->rq.data.rot_turns = (f32*)arena_push(&f->render_arena, cap * sizeof(f32), alignof(f32));
    f->rq.data.sx = (f32*)arena_push(&f->render_arena, cap * sizeof(f32), alignof(f32));
    f->rq.data.sy = (f32*)arena_push(&f->render_arena, cap * sizeof(f32), alignof(f32));
    f->rq.data.uv = (Rect_u16*)arena_push(&f->render_arena, cap * sizeof(Rect_u16), alignof(Rect_u16));
    f->rq.data.rgba = (u32*)arena_push(&f->render_arena, cap * sizeof(u32), alignof(u32));
    f->rq.data.tex = (u16*)arena_push(&f->render_arena, cap * sizeof(u16), alignof(u16));
    f->rq.data.flags = (u8*)arena_push(&f->render_arena, cap * sizeof(u8), alignof(u8));
    f->rq.data.cap = cap;
    f->rq.clips.count = 1;
    f->rq.layout.viewport = Rect_i32{ 0, 0, 1000, 1000 };   // the RECT_SPACE_SCREEN fallback rect_visible reads (no clip pushed)
    f->w.render = &f->rq;
    static WorldTickState tick_state;   // w->state (world.h): the registered singleton, stubbed here
    tick_state = WorldTickState{};
    f->w.state = &tick_state;
    debugdraw_init(&f->w, &f->render_arena);
    return ERR_OK;
}

TL_TEST(dbg_line_submits_one_command, "render") {
    DebugDrawFixture* f = dd_fixture();
    TL_ASSERT_EQ(dd_init(f), ERR_OK);
    dbg_line(&f->w, 10.0f, 10.0f, 50.0f, 10.0f, 2.0f, 0xFFFFFFFFu, RECT_SPACE_SCREEN);
    TL_EXPECT_EQ(f->rq.keys.count, 1u);
    TL_EXPECT_EQ(key_layer(f->rq.keys.data[0]), (u8)LAYER_DEBUG);
    TL_EXPECT_EQ(f->rq.data.x[0], 30.0f);   // midpoint of (10,10)-(50,10)
    TL_EXPECT_EQ(f->rq.data.y[0], 10.0f);
    TL_EXPECT_EQ(f->rq.data.sx[0], 40.0f);   // length
    TL_EXPECT_EQ(f->rq.data.sy[0], 2.0f);    // width_px

    // a degenerate (zero-length) segment draws nothing.
    dbg_line(&f->w, 5.0f, 5.0f, 5.0f, 5.0f, 2.0f, 0xFFFFFFFFu, RECT_SPACE_SCREEN);
    TL_EXPECT_EQ(f->rq.keys.count, 1u);
}

// Review round 2 N4: §9.3.8's width/radius are ALWAYS target px, even for a WORLD-space command -
// a WORLD-space quad's sx/sy otherwise share the command's own space (world m), so an unconverted
// width_px comes out ppu_eff times too wide at emit and changes with zoom, which a debug overlay's
// stroke weight must never do. Every debugdraw test before this round used RECT_SPACE_SCREEN only
// (this file's own comment used to note it as untested).
TL_TEST(dbg_line_world_space_width_is_target_px, "render") {
    DebugDrawFixture* f = dd_fixture();
    TL_ASSERT_EQ(dd_init(f), ERR_OK);
    f->rq.view_world[0] = Rect_f32{ -1000.0f, -1000.0f, 2000.0f, 2000.0f };
    Layout L{}; L.internal_w = 320; L.internal_h = 180;
    f->rq.view_mat[0] = view_matrix(Camera2D{ 0.0f, 0.0f, 1.0f, 0.0f, 16.0f, 0, { 0, 0, 0 } }, L);   // ppu_eff = 16

    dbg_line(&f->w, 0.0f, 0.0f, 10.0f, 0.0f, 2.0f, 0xFFFFFFFFu, RECT_SPACE_WORLD);   // width_px = 2
    TL_ASSERT_EQ(f->rq.keys.count, 1u);
    TL_EXPECT_TRUE(fabsf(f->rq.data.sx[0] - 10.0f) < 1e-5f);           // length: unconverted, already in world m
    TL_EXPECT_TRUE(fabsf(f->rq.data.sy[0] - (2.0f / 16.0f)) < 1e-5f);  // width: 2 px / ppu_eff(16) = 0.125 world m
}

TL_TEST(dbg_rect_submits_four_lines, "render") {
    DebugDrawFixture* f = dd_fixture();
    TL_ASSERT_EQ(dd_init(f), ERR_OK);
    dbg_rect(&f->w, 0.0f, 0.0f, 100.0f, 50.0f, 1.0f, 0xFF0000FFu, RECT_SPACE_SCREEN);
    TL_EXPECT_EQ(f->rq.keys.count, 4u);
}

TL_TEST(dbg_circle_segment_count_clamped, "render") {
    DebugDrawFixture* f = dd_fixture();
    TL_ASSERT_EQ(dd_init(f), ERR_OK);

    // Centred well inside the {0,0,1000,1000} viewport (docs/RENDER2D.md §9.3.4's overlap test
    // is non-strict but still real - a circle centred at the viewport's own corner would clip
    // half its segments' AABBs outside it, which is not what this row means to exercise).
    // N = clamp(ceil(r_px / 2), 8, 64): r_px = 4 -> ceil(2) = 2 -> clamped to 8.
    dbg_circle(&f->w, 500.0f, 500.0f, 4.0f, 1.0f, 0xFFFFFFFFu, RECT_SPACE_SCREEN);
    TL_EXPECT_EQ(f->rq.keys.count, 8u);

    // r_px = 300 -> ceil(150) = 150 -> clamped to 64.
    f->rq.keys.count = 0; f->rq.data.count = 0; f->rq.data_index.count = 0; f->rq.clip_id.count = 0;
    dbg_circle(&f->w, 500.0f, 500.0f, 300.0f, 1.0f, 0xFFFFFFFFu, RECT_SPACE_SCREEN);
    TL_EXPECT_EQ(f->rq.keys.count, 64u);
}

// Review round 2 N11: debugdraw_replay_persistent used to walk the ring in SLOT order, which only
// matches insertion order before the first wrap - docs/RENDER2D.md §9.3.8 states "depth =
// submission", and each replay's depth24 is assigned fresh (q->stats_submitted at THIS call),
// so the replay LOOP's own visitation order is what "submission order" means here. Pushes
// DBG_PERSIST_RING_CAP + 3 distinguishable entries (rgba = insertion index) so the last 3 wrap
// over ring slots 0-2 (debugdraw.h's own overflow contract) and checks the replay reproduces
// the surviving insertion order (3..DBG_PERSIST_RING_CAP+2), oldest first - not ring-slot order.
TL_TEST(persistent_ring_replays_in_insertion_order_after_wrap, "render") {
    DebugDrawFixture* f = dd_fixture();
    TL_ASSERT_EQ(dd_init(f), ERR_OK);
    f->w.state->tick = 0;

    for (u32 i = 0; i < (u32)DBG_PERSIST_RING_CAP + 3u; ++i) {
        dbg_line_persist(&f->w, 10.0f, 10.0f, 20.0f, 10.0f, 1.0f, i, RECT_SPACE_SCREEN, 1000000u);
    }

    f->rq.keys.count = 0; f->rq.data.count = 0; f->rq.data_index.count = 0; f->rq.clip_id.count = 0;
    debugdraw_replay_persistent(&f->w);
    TL_ASSERT_EQ(f->rq.keys.count, (u32)DBG_PERSIST_RING_CAP);

    for (u32 k = 0; k < f->rq.keys.count; ++k) {
        const u32 di = f->rq.data_index.data[k];
        TL_EXPECT_EQ(f->rq.data.rgba[di], k + 3u);
    }
}

TL_TEST(persistent_ring_replays_until_expiry, "render") {
    DebugDrawFixture* f = dd_fixture();
    TL_ASSERT_EQ(dd_init(f), ERR_OK);
    f->w.state->tick = 10;

    dbg_line_persist(&f->w, 0.0f, 0.0f, 10.0f, 0.0f, 1.0f, 0xFFFFFFFFu, RECT_SPACE_SCREEN, 2u);   // until_tick = 12
    TL_ASSERT_EQ(f->rq.keys.count, 1u);   // drawn immediately

    f->rq.keys.count = 0; f->rq.data.count = 0; f->rq.data_index.count = 0; f->rq.clip_id.count = 0;
    f->w.state->tick = 11;
    debugdraw_replay_persistent(&f->w);
    TL_EXPECT_EQ(f->rq.keys.count, 1u);   // 12 > 11: still alive

    f->rq.keys.count = 0; f->rq.data.count = 0; f->rq.data_index.count = 0; f->rq.clip_id.count = 0;
    f->w.state->tick = 12;
    debugdraw_replay_persistent(&f->w);
    TL_EXPECT_EQ(f->rq.keys.count, 0u);   // 12 > 12 is false: expired
}

TL_TEST(text_layout_is_unsupported, "render") {
    u32 count = 99u;
    const ErrCode e = text_layout(nullptr, StrView{}, nullptr, 0u, &count);
    TL_EXPECT_EQ(e, (ErrCode)ERR_RENDER_UNSUPPORTED);
    TL_EXPECT_EQ(count, 0u);
}
