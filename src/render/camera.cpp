// ---------------------------------------------------------------------------------------------
// camera.cpp - resolve_layout, view_matrix, world_to_screen/screen_to_world, target_to_window,
//   pixel_snap. Spec: docs/RENDER2D.md §9.3.1 (resolve_layout), §9.3.2 (projection).
// ---------------------------------------------------------------------------------------------
#include "render/camera.h"
#include "render/render.h"
#include "foundation/tl_assert.h"
#include <math.h>

Layout resolve_layout(i32 win_w, i32 win_h, i32 logical_w, const Presentation& pres) {
    Layout L{};
    L.dpi_scale = logical_w != 0 ? f32(win_w) / f32(logical_w) : 1.0f;

    if (pres.internal_w == 0) {
        L.viewport = Rect_i32{ 0, 0, win_w, win_h };
        L.internal_w = (u16)win_w;
        L.internal_h = (u16)win_h;
        L.scale_x = 1.0f;
        L.scale_y = 1.0f;
        return L;
    }

    const i32 iw = (i32)pres.internal_w;
    const i32 ih = (i32)pres.internal_h;
    // The early return above only checks internal_w == 0 - a Presentation with a nonzero
    // internal_w but a zero internal_h would otherwise fall through to an integer/float divide
    // by ih below (review round 1 D10).
    TL_CHECK(ih != 0);
    i32 vw, vh;

    if (pres.mode == PRES_INTEGER_LETTERBOX) {
        i32 s = win_w / iw;
        i32 s2 = win_h / ih;
        if (s2 < s) { s = s2; }
        if (s < 1) { s = 1; }
        vw = iw * s;
        vh = ih * s;
        L.scale_x = (f32)s;
        L.scale_y = (f32)s;
    } else if (pres.mode == PRES_ASPECT_FIT) {
        f32 sx = f32(win_w) / f32(iw);
        f32 sy = f32(win_h) / f32(ih);
        f32 s = sx < sy ? sx : sy;
        vw = (i32)floorf(f32(iw) * s);
        vh = (i32)floorf(f32(ih) * s);
        L.scale_x = s;
        L.scale_y = s;
    } else {   // PRES_STRETCH
        vw = win_w;
        vh = win_h;
        L.scale_x = f32(win_w) / f32(iw);
        L.scale_y = f32(win_h) / f32(ih);
    }

    L.viewport = Rect_i32{ (win_w - vw) / 2, (win_h - vh) / 2, vw, vh };
    L.internal_w = pres.internal_w;
    L.internal_h = pres.internal_h;
    return L;
}

Mat3 view_matrix(const Camera2D& cam, const Layout& L) {
    const f32 ppu = cam.ppu * cam.zoom;
    const f32 turns_to_rad = 6.283185307f;
    const f32 c = cosf(cam.rot_turns * turns_to_rad);
    const f32 s = sinf(cam.rot_turns * turns_to_rad);
    f32 cx = cam.cx;
    f32 cy = cam.cy;
    if (cam.pixel_snap != 0) {
        // ppu = cam.ppu * cam.zoom - a zero (or NaN) ppu/zoom divides by zero here
        // (review round 1 D10).
        TL_CHECK(ppu != 0.0f);
        cx = roundf(cx * ppu) / ppu;
        cy = roundf(cy * ppu) / ppu;
    }
    const f32 W = (f32)L.internal_w;
    const f32 H = (f32)L.internal_h;
    Mat3 m{};
    m.m[0] = ppu * c;  m.m[1] = ppu * s;  m.m[2] = W * 0.5f - ppu * (c * cx + s * cy);
    m.m[3] = ppu * s;  m.m[4] = -ppu * c; m.m[5] = H * 0.5f + ppu * (c * cy - s * cx);
    m.m[6] = 0.0f;     m.m[7] = 0.0f;     m.m[8] = 1.0f;
    return m;
}

void world_to_screen(const Mat3& M, f32 wx, f32 wy, f32* sx, f32* sy) {
    *sx = M.m[0] * wx + M.m[1] * wy + M.m[2];
    *sy = M.m[3] * wx + M.m[4] * wy + M.m[5];
}

void screen_to_world(const Mat3& M, f32 sx, f32 sy, f32* wx, f32* wy) {
    // affine inverse: translate by -[m2,m5], then invert the 2x2 linear part (det = -ppu^2 != 0
    // by construction - docs/RENDER2D.md §9.3.2).
    const f32 det = M.m[0] * M.m[4] - M.m[1] * M.m[3];
    // det = -ppu^2 by construction (this function's own comment) - nonzero unless the view's ppu
    // is itself zero (review round 1 D10; view_matrix's own TL_CHECK(ppu != 0) guards that at the
    // source, this is the second line of defense for a matrix built some other way).
    TL_CHECK(det != 0.0f);
    const f32 inv_det = 1.0f / det;
    const f32 tx = sx - M.m[2];
    const f32 ty = sy - M.m[5];
    *wx = (M.m[4] * tx - M.m[1] * ty) * inv_det;
    *wy = (M.m[0] * ty - M.m[3] * tx) * inv_det;
}

void target_to_window(const Layout& L, f32 tx, f32 ty, f32* wx, f32* wy) {
    *wx = (f32)L.viewport.x + tx * L.scale_x;
    *wy = (f32)L.viewport.y + ty * L.scale_y;
}

f32 pixel_snap(f32 px) { return floorf(px + 0.5f); }

void render_camera_init(World* w, u8 view, const Camera2D& cam) {
    RenderQueue* q = w->render;
    TL_CHECK(view < MAX_VIEWS);
    q->camera[view] = cam;
    q->camera_prev[view] = CameraPrev{ cam.cx, cam.cy, cam.zoom, cam.rot_turns, cam.ppu, cam.pixel_snap, { 0, 0, 0 } };
    if (view >= q->camera_count) { q->camera_count = (u8)(view + 1u); }
}
