// extract.test.cpp - docs/RENDER2D.md §9.6 extract_snap_and_arc.
#include "runner/tl_test.h"
#include "core/transform.h"
#include "render/render.h"
#include "foundation/vmem_test_api.h"
#include "foundation/fx.h"
#include <string.h>
#include <math.h>

// A real (non-headless-platform) ECS World: sys_extract only touches w->render (a manually-
// built RenderQueue - no PlatformApi/DrawApi call anywhere in extract.cpp) and the Transform/
// TransformPrev/Camera2D/CameraPrev/CameraFollow columns, so the lighter tests/core-style
// fixture (docs/ECS.md, mirrored from tests/core/world_test_util.h) is the right tool, not
// tests/render/render_test_util.h's headless-platform-backed one.
struct ExtractFixture {
    VMemApi api;
    ArenaRegistry reg;
    Scratch scratch;
    World w;
    RenderQueue rq;
};

// World carries comps[1024] (~100s of KB) - a caller-owned static, never the stack
// (docs/LESSONS.md "a World-sized fixture on the stack is a Windows-only crash").
static ExtractFixture* et_fixture() {
    static ExtractFixture f;
    return &f;
}

static void et_init(ExtractFixture* f) {
    f->api = test_vmem_api();
    memset(&f->reg, 0, sizeof(f->reg));
    TL_ASSERT(scratch_init(&f->scratch, "et.scratch"_id, 16u * 1024u * 1024u, &f->api) == ERR_OK);
    WorldDesc d{};
    d.seed = 1;
    TL_ASSERT(world_init(&f->w, &f->reg, &f->scratch, &f->api, &d) == ERR_OK);
    world_register_component(&f->w, &Transform_info);
    world_register_component(&f->w, &TransformPrev_info);
    world_register_component(&f->w, &Camera2D_info);
    world_register_component(&f->w, &CameraPrev_info);
    world_register_component(&f->w, &CameraFollow_info);
    world_build_schedule(&f->w);

    memset(&f->rq, 0, sizeof(f->rq));
    f->rq.layout.internal_w = 320;
    f->rq.layout.internal_h = 180;
    f->w.render = &f->rq;
}

TL_TEST(extract_snap_and_arc, "render") {
    ExtractFixture* f = et_fixture();
    et_init(f);
    World* w = &f->w;

    // snap bit forces a = 1: prev at 0, cur at 10, flags carries TRANSFORM_SNAP.
    Transform t_cur{}; t_cur.x = fx::fx_int<pos_t>(10); t_cur.y = fx::fx_int<pos_t>(0); t_cur.rot = fx::fx_int<angle_t>(0);
    t_cur.flags = TRANSFORM_SNAP;
    TransformPrev t_prev{}; t_prev.x = fx::fx_int<pos_t>(0); t_prev.y = fx::fx_int<pos_t>(0); t_prev.rot = fx::fx_int<angle_t>(0); t_prev.flags = 0;
    const Entity e_snap = world_spawn(w);
    world_add<Transform>(w, e_snap, t_cur);
    world_add<TransformPrev>(w, e_snap, t_prev);

    // rotation 0.9 -> 0.1 turns: the shortest arc goes FORWARD through 1.0 (+0.2), not backward
    // through 0.5 (-0.8) or the naive direct lerp toward 0.5 (docs/RENDER2D.md §9.3.3).
    Transform t_cur2{}; t_cur2.x = fx::fx_int<pos_t>(0); t_cur2.y = fx::fx_int<pos_t>(0); t_cur2.rot = fx::fx_lit<angle_t>(1, 10); t_cur2.flags = 0;
    TransformPrev t_prev2{}; t_prev2.x = fx::fx_int<pos_t>(0); t_prev2.y = fx::fx_int<pos_t>(0); t_prev2.rot = fx::fx_lit<angle_t>(9, 10); t_prev2.flags = 0;
    const Entity e_arc = world_spawn(w);
    world_add<Transform>(w, e_arc, t_cur2);
    world_add<TransformPrev>(w, e_arc, t_prev2);

    world_flush(w);

    f->rq.alpha = 0.5f;
    sys_extract(w);

    // packet index == Transform dense index (docs/RENDER2D.md §9.2 RenderPacket comment); both
    // entities were added in spawn order with nothing removed, so dense index == spawn order.
    TL_ASSERT_EQ(f->rq.packet.count, 2u);
    TL_EXPECT_TRUE(fabsf(f->rq.packet.x[0] - 10.0f) < 1e-5f);   // snap: a = 1, not alpha = 0.5 (would be 5.0)
    TL_EXPECT_TRUE(fabsf(f->rq.packet.y[0] - 0.0f) < 1e-5f);

    // 0.9 + shortest_arc(0.9, 0.1) * 0.5 = 0.9 + 0.2 * 0.5 = 1.0 (through the wrap), not
    // 0.9 + (0.1 - 0.9) * 0.5 = 0.5 (the naive direct lerp, going the wrong way).
    TL_EXPECT_TRUE(fabsf(f->rq.packet.rot_turns[1] - 1.0f) < 1e-5f);
    TL_EXPECT_TRUE(fabsf(f->rq.packet.rot_turns[1] - 0.5f) > 0.1f);   // not the naive answer
}
