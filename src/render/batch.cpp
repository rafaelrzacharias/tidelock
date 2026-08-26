// ---------------------------------------------------------------------------------------------
// batch.cpp - key sort (sort_u64_kv), scan-batching, vertex/index emission.
// Spec: docs/RENDER2D.md §9.3.5, §9.3.6, §9.4 step 3.
// ---------------------------------------------------------------------------------------------
#include "render/render_internal.h"
#include "foundation/sort.h"
#include <math.h>

void render_sort_and_batch(RenderQueue* q, Scratch* scratch) {
    const u32 n = q->keys.count;
    for (u32 i = 0; i < n; ++i) { q->order.data[i] = i; }
    q->order.count = n;
    sort_u64_kv(q->keys.data, q->order.data, n, scratch);

    q->batches.count = 0;
    u16 cur_tex = 0, cur_clip = 0;
    u8 cur_blend = 0, cur_layer = 0;
    bool have_cur = false;
    for (u32 i = 0; i < n; ++i) {
        const u32 c = q->order.data[i];
        const u64 k = q->keys.data[i];
        const u16 tex = key_material(k);
        const u8 layer = key_layer(k);
        const u8 blend = key_blend(k);
        const u16 clip = q->clip_id.data[c];
        if (!have_cur || tex != cur_tex || layer != cur_layer || blend != cur_blend || clip != cur_clip) {
            array_push(&q->batches, Batch{ tex, clip, blend, layer, 0, i, 0 });
            cur_tex = tex; cur_layer = layer; cur_blend = blend; cur_clip = clip; have_cur = true;
        }
        q->batches.data[q->batches.count - 1].count += 1;
    }
}

// Transforms a LOCAL point (before rotation) by the quad's own rotation only (docs/RENDER2D.md
// §9.4 step 3: "corners of the quad (x,y,sx,sy,rot; flip bits...)").
static void rotate_local(f32 lx, f32 ly, f32 cs, f32 sn, f32* rx, f32* ry) {
    *rx = lx * cs - ly * sn;
    *ry = lx * sn + ly * cs;
}

void render_emit_geometry(RenderQueue* q) {
    q->verts.count = 0;
    q->idx.count = 0;
    const u32 nb = q->batches.count;
    for (u32 bi = 0; bi < nb; ++bi) {
        const Batch b = q->batches.data[bi];
        const u8 view = render_resolve_view(q, b.layer);

        // Batch key already fixes the texture (key_material) for every command in this run, so the
        // size query is loop-invariant - hoisted here instead of once per command (review round 1
        // M1).
        u16 tw = 0, th = 0;
        if (b.tex != 0) {
            TexHandle t{ b.tex };
            q->platform->draw.texture_size(q->platform->draw.ctx, t, &tw, &th);
        }

        for (u32 j = b.first; j < b.first + b.count; ++j) {
            const u32 c = q->order.data[j];
            const f32 x = q->data.x[c], y = q->data.y[c], rot = q->data.rot_turns[c];
            const f32 sx = q->data.sx[c], sy = q->data.sy[c];
            const u8 flags = q->data.flags[c];
            const Rect_u16 uv = q->data.uv[c];
            const u32 rgba = q->data.rgba[c];

            const f32 turn = rot * 6.283185307f;
            const f32 cs = cosf(turn), sn = sinf(turn);
            const f32 hx = sx * 0.5f, hy = sy * 0.5f;
            const f32 lx[4] = { -hx,  hx, hx, -hx };   // TL, TR, BR, BL
            const f32 ly[4] = { -hy, -hy, hy,  hy };
            f32 rx[4], ry[4];
            for (u32 k = 0; k < 4; ++k) { rotate_local(lx[k], ly[k], cs, sn, &rx[k], &ry[k]); }

            f32 tcx, tcy;
            f32 drx[4], dry[4];
            const bool screen_space = (flags & DRAWFLAG_SCREEN_SPACE) != 0;
            if (screen_space) {
                tcx = x; tcy = y;
                for (u32 k = 0; k < 4; ++k) { drx[k] = rx[k]; dry[k] = ry[k]; }
            } else {
                const Mat3& M = q->view_mat[view];
                world_to_screen(M, x, y, &tcx, &tcy);
                for (u32 k = 0; k < 4; ++k) {
                    drx[k] = M.m[0] * rx[k] + M.m[1] * ry[k];
                    dry[k] = M.m[3] * rx[k] + M.m[4] * ry[k];
                }
            }
            if (q->pres[view].pixel_snap != 0 && (flags & DRAWFLAG_NO_SNAP) == 0) {
                tcx = pixel_snap(tcx);
                tcy = pixel_snap(tcy);
            }

            f32 u0, v0, u1, v1;
            if (tw != 0 && th != 0) {
                u0 = (f32)uv.x / (f32)tw;           v0 = (f32)uv.y / (f32)th;
                u1 = (f32)(uv.x + uv.w) / (f32)tw;  v1 = (f32)(uv.y + uv.h) / (f32)th;
            } else {   // untextured, or the size query came back unknown (docs/RENDER2D.md §9.4 step 3)
                u0 = (f32)uv.x / 65536.0f;          v0 = (f32)uv.y / 65536.0f;
                u1 = (f32)(uv.x + uv.w) / 65536.0f; v1 = (f32)(uv.y + uv.h) / 65536.0f;
            }
            if ((flags & DRAWFLAG_FLIP_X) != 0) { const f32 t = u0; u0 = u1; u1 = t; }
            if ((flags & DRAWFLAG_FLIP_Y) != 0) { const f32 t = v0; v0 = v1; v1 = t; }
            const f32 uu[4] = { u0, u1, u1, u0 };
            const f32 vv[4] = { v0, v0, v1, v1 };

            const u32 base = q->verts.count;
            for (u32 k = 0; k < 4; ++k) {
                array_push(&q->verts, DrawVertex{ tcx + drx[k], tcy + dry[k], uu[k], vv[k], rgba });
            }
            array_push(&q->idx, base + 0); array_push(&q->idx, base + 1); array_push(&q->idx, base + 2);
            array_push(&q->idx, base + 0); array_push(&q->idx, base + 2); array_push(&q->idx, base + 3);
        }
    }
}
