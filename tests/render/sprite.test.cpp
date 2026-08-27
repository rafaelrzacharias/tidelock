// sprite.test.cpp - sys_sprite_render: an ordinary public function (docs/CPP-SUBSET.md §6 "every
// public function has tests"), even though docs/RENDER2D.md §9.6's table names it only through
// extract_snap_and_arc/reject's coverage of the pieces it calls. Exercises the real path:
// Transform+Sprite -> sys_extract -> sys_sprite_render -> render_submit, and the SPRITE_VISIBLE
// gate (docs/RENDER2D.md §3 "flags: ... bit2 visible").
#include "runner/tl_test.h"
#include "render/sprite.h"
#include "core/transform.h"
#include "foundation/vmem_test_api.h"
#include <string.h>

struct SpriteFixture {
    VMemApi api;
    ArenaRegistry reg;
    Scratch scratch;
    VMemArena render_arena;
    World w;
    RenderQueue rq;
};

// A caller-owned static, never the stack (docs/LESSONS.md - the World-sized-fixture crash).
static SpriteFixture* sp_fixture() { static SpriteFixture f; return &f; }

// Returns the first failing call's ErrCode (the jobs_test_util.h/world_test_util.h idiom - TODO.md,
// docs/LESSONS.md: TL_ASSERT wrapping a call expression directly compiles the call itself away at
// TL_DEV=0, not just the check, since the macro argument never appears in that tier's expansion).
static ErrCode sp_init(SpriteFixture* f) {
    f->api = test_vmem_api();
    memset(&f->reg, 0, sizeof(f->reg));
    ErrCode e = scratch_init(&f->scratch, "sp.scratch"_id, 16u * 1024u * 1024u, &f->api);
    if (e != ERR_OK) { return e; }
    WorldDesc d{};
    d.seed = 1;
    e = world_init(&f->w, &f->reg, &f->scratch, &f->api, &d);
    if (e != ERR_OK) { return e; }
    world_register_component(&f->w, &Transform_info);
    world_register_component(&f->w, &TransformPrev_info);
    world_register_component(&f->w, &Sprite_info);
    world_build_schedule(&f->w);

    e = vmem_arena_init(&f->render_arena, "sp.render_arena"_id, 4u * 1024u * 1024u, 0, &f->api);
    if (e != ERR_OK) { return e; }
    memset(&f->rq, 0, sizeof(f->rq));
    const u32 cap = 64u;
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
    f->rq.clips.count = 1;   // slot 0 reserved for "no clip"
    f->rq.layer_view[LAYER_WORLD] = 0;
    f->rq.view_world[0] = Rect_f32{ -1000.0f, -1000.0f, 2000.0f, 2000.0f };   // huge: nothing here gets rejected
    f->rq.layout.internal_w = 320; f->rq.layout.internal_h = 180;
    f->w.render = &f->rq;
    return ERR_OK;
}

TL_TEST(sprite_render_submits_visible_sprites, "render") {
    SpriteFixture* f = sp_fixture();
    TL_ASSERT_EQ(sp_init(f), ERR_OK);
    World* w = &f->w;

    Transform tr{}; tr.x = fx::fx_int<pos_t>(5); tr.y = fx::fx_int<pos_t>(0); tr.rot = fx::fx_int<angle_t>(0); tr.flags = 0;
    TransformPrev tp{}; tp.x = tr.x; tp.y = tr.y; tp.rot = tr.rot; tp.flags = 0;

    Sprite s{}; s.rgba = 0xFFFFFFFFu; s.src[0] = 0; s.src[1] = 0; s.src[2] = 16; s.src[3] = 16;
    s.tex = TexHandle{}; s.depth_bias = 3; s.layer = LAYER_WORLD; s.flags = SPRITE_VISIBLE; s._pad0 = 0;

    const Entity e_visible = world_spawn(w);
    world_add<Transform>(w, e_visible, tr);
    world_add<TransformPrev>(w, e_visible, tp);
    world_add<Sprite>(w, e_visible, s);

    // an invisible sprite (SPRITE_VISIBLE clear) must be skipped, not submitted.
    Sprite s_hidden = s;
    s_hidden.flags = 0;
    const Entity e_hidden = world_spawn(w);
    world_add<Transform>(w, e_hidden, tr);
    world_add<TransformPrev>(w, e_hidden, tp);
    world_add<Sprite>(w, e_hidden, s_hidden);

    world_flush(w);

    f->rq.alpha = 1.0f;
    sys_extract(w);
    sys_sprite_render(w);

    TL_ASSERT_EQ(f->rq.keys.count, 1u);   // only the visible sprite
    TL_ASSERT_EQ(f->rq.data.count, 1u);
    TL_EXPECT_EQ(f->rq.data.x[0], 5.0f);
    TL_EXPECT_EQ(f->rq.data.y[0], 0.0f);
    TL_EXPECT_EQ(f->rq.data.rgba[0], 0xFFFFFFFFu);
    TL_EXPECT_EQ(key_layer(f->rq.keys.data[0]), (u8)LAYER_WORLD);
    TL_EXPECT_EQ(key_depth(f->rq.keys.data[0]), (u32)(DEPTH_SPRITE_BASE + 3u * 256u));
}
