// ---------------------------------------------------------------------------------------------
// extract.cpp - sys_extract (PRE_RENDER, first system of the phase): fx -> f32, lerp by alpha,
//   the render packet, camera interpolation + view matrices.
// Spec: docs/RENDER2D.md §9.3.3. One of the two to_f32 call sites in the binary (the other is
//   simview.cpp, docs/RENDER2D.md §9.5) - CI greps for exactly these two.
// ---------------------------------------------------------------------------------------------
#include "render/render.h"
#include "core/transform.h"
#include "foundation/fx_float.h"
#include <math.h>

static void packet_reserve(RenderPacket* pk, Scratch* scratch, u32 n) {
    pk->x = (f32*)scratch_push(scratch, (u64)n * sizeof(f32), alignof(f32));
    pk->y = (f32*)scratch_push(scratch, (u64)n * sizeof(f32), alignof(f32));
    pk->rot_turns = (f32*)scratch_push(scratch, (u64)n * sizeof(f32), alignof(f32));
    pk->sx = (f32*)scratch_push(scratch, (u64)n * sizeof(f32), alignof(f32));
    pk->sy = (f32*)scratch_push(scratch, (u64)n * sizeof(f32), alignof(f32));
    pk->count = n;
    pk->_pad0 = 0;
}

// Shortest-arc delta in turns: d ∈ [-0.5, 0.5) (docs/RENDER2D.md §9.3.3).
static f32 shortest_arc(f32 r0, f32 r1) {
    f32 d = r1 - r0;
    d -= floorf(d + 0.5f);
    return d;
}

void sys_extract(World* w) {
    RenderQueue* q = w->render;
    const f32 alpha = q->alpha;

    const Span<Transform> cur = world_column<Transform>(w);
    const Span<TransformPrev> prev = world_column<TransformPrev>(w);
    const u32 n = cur.count;
    TL_CHECK(prev.count == n);
    packet_reserve(&q->packet, w->scratch, n);

    for (u32 i = 0; i < n; ++i) {
        const f32 a = (cur.data[i].flags & TRANSFORM_SNAP) != 0u ? 1.0f : alpha;
        const f32 x0 = fx::to_f32(prev.data[i].x), x1 = fx::to_f32(cur.data[i].x);
        q->packet.x[i] = x0 + (x1 - x0) * a;
        const f32 y0 = fx::to_f32(prev.data[i].y), y1 = fx::to_f32(cur.data[i].y);
        q->packet.y[i] = y0 + (y1 - y0) * a;
        const f32 r0 = fx::to_f32(prev.data[i].rot), r1 = fx::to_f32(cur.data[i].rot);
        q->packet.rot_turns[i] = r0 + shortest_arc(r0, r1) * a;
        q->packet.sx[i] = 1.0f;   // scale columns exist for a future Scale component; v0 constant
        q->packet.sy[i] = 1.0f;
    }

    // Camera state lives on RenderQueue, not the ECS (review round 1 D1, camera.h's Determinism
    // note) - only views [0, camera_count) are configured, matching the old design's "however
    // many camera entities exist" (an unconfigured view is simply never processed, same as zero
    // Camera2D entities meant zero loop iterations before).
    TL_CHECK(q->camera_count <= MAX_VIEWS);
    for (u32 view = 0; view < q->camera_count; ++view) {
        const Camera2D c = q->camera[view];
        const CameraPrev p = q->camera_prev[view];
        Camera2D interp{};
        interp.cx = p.cx + (c.cx - p.cx) * alpha;
        interp.cy = p.cy + (c.cy - p.cy) * alpha;
        interp.zoom = p.zoom + (c.zoom - p.zoom) * alpha;
        interp.ppu = p.ppu + (c.ppu - p.ppu) * alpha;
        // A camera whose EFFECTIVE ppu lerps to zero makes view_matrix build a singular matrix -
        // D10's own TL_CHECK(det != 0) would catch it three calls deeper, in unrelated matrix
        // math, with no clue which camera did it (review round 2 N1). render_camera_init
        // (render.h) is the sanctioned way to configure a view precisely to avoid this - it seeds
        // camera_prev[view] to camera[view] on first setup, so this only fires for a caller that
        // bypassed it (a raw camera[view] write with camera_prev[view] left at render_init's
        // zero-fill) or a genuinely degenerate camera (ppu == 0 by construction).
        TL_CHECK(interp.ppu != 0.0f);
        interp.rot_turns = p.rot_turns + shortest_arc(p.rot_turns, c.rot_turns) * alpha;
        interp.pixel_snap = c.pixel_snap;

        // camera_follow[view].target == Entity{} (never configured) resolves to null here, same
        // as "no follow" - no separate has-follow flag needed (camera.h's CameraFollow comment).
        const CameraFollow follow = q->camera_follow[view];
        const Transform* t = world_get<Transform>(w, follow.target);
        if (t != nullptr) {
            const u32 idx = (u32)(t - cur.data);
            interp.cx = q->packet.x[idx] + follow.off_x;
            interp.cy = q->packet.y[idx] + follow.off_y;
        }

        const Mat3 M = view_matrix(interp, q->layout);
        q->view_mat[view] = M;

        // view_world[view] = AABB of the 4 viewport corners through screen_to_world.
        const f32 W = (f32)q->layout.internal_w, H = (f32)q->layout.internal_h;
        const f32 cxs[4] = { 0.0f, W, W, 0.0f };
        const f32 cys[4] = { 0.0f, 0.0f, H, H };
        f32 minx, maxx, miny, maxy;
        screen_to_world(M, cxs[0], cys[0], &minx, &miny);
        maxx = minx; maxy = miny;
        for (u32 k = 1; k < 4; ++k) {
            f32 wx, wy;
            screen_to_world(M, cxs[k], cys[k], &wx, &wy);
            if (wx < minx) { minx = wx; } if (wx > maxx) { maxx = wx; }
            if (wy < miny) { miny = wy; } if (wy > maxy) { maxy = wy; }
        }
        q->view_world[view] = Rect_f32{ minx, miny, maxx - minx, maxy - miny };
    }
}
