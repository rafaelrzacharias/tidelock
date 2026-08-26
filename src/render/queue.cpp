// ---------------------------------------------------------------------------------------------
// queue.cpp - render_init/shutdown/resize, render_push_data, render_submit, clip push/pop,
//   rect_visible, render_draw_quad. Spec: docs/RENDER2D.md §9.3.4 (submission/reject).
// ---------------------------------------------------------------------------------------------
#include "render/render.h"
#include "render/render_internal.h"
#include <math.h>
#include <string.h>

static f32* push_f32(VMemArena* arena, u32 n) { return (f32*)arena_push(arena, (u64)n * sizeof(f32), alignof(f32)); }

ErrCode render_init(World* w, const PlatformApi* platform, VMemArena* arena, const Presentation* pres0, u32 max_commands) {
    const u32 cap = max_commands == 0 ? RENDER_DEFAULT_MAX_COMMANDS : max_commands;

    RenderQueue* q = (RenderQueue*)arena_push(arena, sizeof(RenderQueue), alignof(RenderQueue));
    memset(q, 0, sizeof(RenderQueue));
    q->platform = platform;

    array_init_fixed(&q->keys, arena, cap);
    array_init_fixed(&q->data_index, arena, cap);
    array_init_fixed(&q->clip_id, arena, cap);
    array_init_fixed(&q->order, arena, cap);
    array_init_fixed(&q->batches, arena, cap);          // worst case: every command its own batch
    array_init_fixed(&q->verts, arena, cap * 4u);        // 4 verts/quad (docs/RENDER2D.md §9.1)
    array_init_fixed(&q->idx, arena, cap * 6u);          // 6 indices/quad

    q->data.x = push_f32(arena, cap);
    q->data.y = push_f32(arena, cap);
    q->data.rot_turns = push_f32(arena, cap);
    q->data.sx = push_f32(arena, cap);
    q->data.sy = push_f32(arena, cap);
    q->data.uv = (Rect_u16*)arena_push(arena, (u64)cap * sizeof(Rect_u16), alignof(Rect_u16));
    q->data.rgba = (u32*)arena_push(arena, (u64)cap * sizeof(u32), alignof(u32));
    q->data.tex = (u16*)arena_push(arena, (u64)cap * sizeof(u16), alignof(u16));
    q->data.flags = (u8*)arena_push(arena, (u64)cap * sizeof(u8), alignof(u8));
    q->data.count = 0;
    q->data.cap = cap;

    q->clips.count = 1;   // slot 0 is reserved for "no clip" (docs/RENDER2D.md §9.2 ClipTable comment)
    q->clips.depth = 0;

    q->pres[0] = *pres0;
    q->layer_view[LAYER_WORLD] = 0;      // world-space
    q->layer_view[LAYER_UI] = 0xFFu;     // screen-space
    q->layer_view[LAYER_DEBUG] = 0xFFu;  // screen-space
    for (u32 i = 3; i < MAX_LAYERS; ++i) { q->layer_view[i] = 0xFFu; }
    q->clear_rgba[LAYER_WORLD] = 0xFF000000u;

    i32 draw_w = 0, draw_h = 0, log_w = 0, log_h = 0;
    platform->window.drawable_size(platform->window.ctx, &draw_w, &draw_h);
    platform->window.size(platform->window.ctx, &log_w, &log_h);
    q->layout = resolve_layout(draw_w, draw_h, log_w, q->pres[0]);

    // Set before the one fallible step below (review round 1 M5): on a texture_create failure a
    // caller's cleanup path calling render_shutdown(w) needs w->render already valid - q->target
    // is all-null from the memset above, so render_shutdown's destroy loop is a no-op, not a null
    // deref through a never-assigned w->render.
    w->render = q;

    if (pres0->internal_w != 0) {
        Result<TexHandle> t = platform->draw.texture_create(platform->draw.ctx, pres0->internal_w, pres0->internal_h, PIXFMT_RGBA8, TEX_TARGET);
        if (t.err != ERR_OK) { return ERR_RENDER_INIT; }
        q->target[LAYER_WORLD] = t.value;
    }

    return ERR_OK;
}

void render_shutdown(World* w) {
    RenderQueue* q = w->render;
    for (u32 i = 0; i < MAX_LAYERS; ++i) {
        if (!handle_is_null(q->target[i])) { q->platform->draw.texture_destroy(q->platform->draw.ctx, q->target[i]); }
    }
}

ErrCode render_resize(World* w, i32 win_w, i32 win_h, i32 logical_w) {
    RenderQueue* q = w->render;
    q->layout = resolve_layout(win_w, win_h, logical_w, q->pres[0]);
    if (!handle_is_null(q->target[LAYER_WORLD])) {
        q->platform->draw.texture_destroy(q->platform->draw.ctx, q->target[LAYER_WORLD]);
        q->target[LAYER_WORLD] = TexHandle{};
    }
    if (q->pres[0].internal_w != 0) {
        Result<TexHandle> t = q->platform->draw.texture_create(q->platform->draw.ctx, q->pres[0].internal_w, q->pres[0].internal_h, PIXFMT_RGBA8, TEX_TARGET);
        if (t.err != ERR_OK) { return ERR_RENDER_INIT; }
        q->target[LAYER_WORLD] = t.value;
    }
    return ERR_OK;
}

u32 render_push_data(World* w, f32 x, f32 y, f32 rot, f32 sx, f32 sy, Rect_u16 uv, u32 rgba, TexHandle tex, u8 flags) {
    RenderQueue* q = w->render;
    if (q->data.count >= q->data.cap) { TL_FATAL("render_push_data: cap exceeded"); }
    const u32 i = q->data.count;
    q->data.x[i] = x; q->data.y[i] = y; q->data.rot_turns[i] = rot; q->data.sx[i] = sx; q->data.sy[i] = sy;
    q->data.uv[i] = uv; q->data.rgba[i] = rgba; q->data.tex[i] = tex.bits; q->data.flags[i] = flags;
    q->data.count += 1;
    return i;
}

void render_submit(World* w, DrawCommand c) {
    RenderQueue* q = w->render;
    c.clip_id = q->clips.depth > 0 ? q->clips.stack[q->clips.depth - 1] : 0u;
    array_push(&q->keys, c.key);
    array_push(&q->data_index, c.data_index);
    array_push(&q->clip_id, c.clip_id);
    q->stats_submitted += 1;
}

u16 render_clip_push(World* w, Rect_i32 r) {
    RenderQueue* q = w->render;
    if (q->clips.count >= 256u) { TL_FATAL("render_clip_push: clip table full"); }
    if (q->clips.depth >= 32u) { TL_FATAL("render_clip_push: clip stack overflow"); }
    const u16 id = q->clips.count;
    q->clips.rects[id] = r;
    q->clips.count += 1;
    q->clips.stack[q->clips.depth] = id;
    q->clips.depth += 1;
    return id;
}

void render_clip_pop(World* w) {
    RenderQueue* q = w->render;
    TL_CHECK(q->clips.depth > 0);
    q->clips.depth -= 1;
}

bool rect_visible(World* w, Rect_f32 r, u8 layer, RectSpace space) {
    RenderQueue* q = w->render;
    Rect_f32 v;
    if (space == RECT_SPACE_WORLD) {
        const u8 view = render_resolve_view(q, layer);
        TL_CHECK(view < MAX_VIEWS);
        v = q->view_world[view];
    } else if (q->clips.depth > 0) {
        const Rect_i32 ci = q->clips.rects[q->clips.stack[q->clips.depth - 1]];
        v = Rect_f32{ (f32)ci.x, (f32)ci.y, (f32)ci.w, (f32)ci.h };
    } else if (!handle_is_null(q->target[layer])) {
        u16 tw = 0, th = 0;
        q->platform->draw.texture_size(q->platform->draw.ctx, q->target[layer], &tw, &th);
        v = Rect_f32{ 0.0f, 0.0f, (f32)tw, (f32)th };
    } else {
        v = Rect_f32{ (f32)q->layout.viewport.x, (f32)q->layout.viewport.y, (f32)q->layout.viewport.w, (f32)q->layout.viewport.h };
    }
    return r.x <= v.x + v.w && r.x + r.w >= v.x && r.y <= v.y + v.h && r.y + r.h >= v.y;
}

void render_draw_quad(World* w, u8 layer, u32 depth24, f32 x, f32 y, f32 rot, f32 sx, f32 sy,
                      Rect_u16 uv, u32 rgba, TexHandle tex, u8 flags) {
    const f32 r = 0.5f * sqrtf(sx * sx + sy * sy);
    const Rect_f32 aabb{ x - r, y - r, 2.0f * r, 2.0f * r };
    const RectSpace space = (flags & DRAWFLAG_SCREEN_SPACE) != 0 ? RECT_SPACE_SCREEN : RECT_SPACE_WORLD;
    if (!rect_visible(w, aabb, layer, space)) {
        w->render->stats_rejected += 1;
        return;
    }
    const u32 d = render_push_data(w, x, y, rot, sx, sy, uv, rgba, tex, flags);
    render_submit(w, DrawCommand{ key_pack(layer, 0, depth24, tex.bits, 0), d, 0, 0 });
}
