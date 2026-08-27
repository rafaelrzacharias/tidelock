// ---------------------------------------------------------------------------------------------
// backend_sdl.cpp - render_present: sort -> batch -> emit -> the layer/target walk -> present.
//   The only TU in render/ that calls the platform DrawApi (docs/RENDER2D.md caption); the name
//   records the SDL_Render-shaped verb set (§6) even though every call here is through the
//   platform seam - no SDL header is ever included from src/render/.
//
// Spec: docs/RENDER2D.md §9.4 (the exact sequence, restated per step below).
//
// STATS NOTE (a gap this lane filled - TODO.md): step 7 says "counts <- 0; stats published to
// the profiler counters" but names no profiler call and no reset rule for the stats_* fields
// themselves (foundation/tl_prof.h exists but nothing wires TL_PROF_COUNTER to render yet - the
// editor lane's job, chains after this one per TODO.md). Chosen behaviour, consistent with both
// this file's own present_descriptor test and queue.test.cpp's `reject` test (which reads
// stats_submitted/stats_rejected BEFORE calling render_present): stats_draw_calls/stats_batches
// are OVERWRITTEN with this frame's final values and left readable after render_present returns
// (a caller/test inspects them post-call); stats_submitted/stats_rejected are reset to 0 at the
// end so the NEXT frame's render_submit/render_draw_quad calls start counting from zero, and a
// caller wanting per-frame submit/reject counts reads them before calling render_present.
// ---------------------------------------------------------------------------------------------
#include "render/render_internal.h"

// Leaves a layer's internal target: draws it as a full-viewport textured quad to the window.
// docs/RENDER2D.md §9.4 step 4's "blit(cur_layer)". FILTER NOTE (TODO.md): the doc's comment
// names "filter = pres.filter", but platform/platform.h's DrawApi has no texture-filter verb
// (v0's one SDL_Render backend has no seam to set it per-draw) - the upscale blit always samples
// however the platform's renderer is configured; a filter knob is a platform-lane addition, not
// this file's to invent.
static void blit_layer_to_window(RenderQueue* q, u8 layer) {
    const DrawApi& d = q->platform->draw;
    TL_CHECK(d.set_target(d.ctx, TexHandle{}) == ERR_OK);
    d.set_clip(d.ctx, nullptr);
    const Rect_i32 vp = q->layout.viewport;
    const u32 white = 0xFFFFFFFFu;
    const DrawVertex verts4[4] = {
        { (f32)vp.x,          (f32)vp.y,          0.0f, 0.0f, white },
        { (f32)(vp.x + vp.w), (f32)vp.y,          1.0f, 0.0f, white },
        { (f32)(vp.x + vp.w), (f32)(vp.y + vp.h), 1.0f, 1.0f, white },
        { (f32)vp.x,          (f32)(vp.y + vp.h), 0.0f, 1.0f, white },
    };
    const u32 idx4[6] = { 0, 1, 2, 0, 2, 3 };
    TL_CHECK(d.draw_geometry(d.ctx, q->target[layer], verts4, 4, idx4, 6) == ERR_OK);
}

// steps 1-4 (+ stats_draw_calls/stats_batches, the STATS NOTE above), split out from
// render_present so a test can inspect the headless draw-call log before render_present's own
// d.present(d.ctx) call clears it (docs/PLATFORM.md §9.4: "present() clears the log" - the
// headless impl's own documented, tested behaviour, tests/platform/draw.test.cpp
// headless_draw_log_records_verbs_in_order). Not part of tl_render's public surface (render.h
// is) - declared in render_internal.h beside render_sort_and_batch/render_emit_geometry.
void render_build_frame(World* w) {
    RenderQueue* q = w->render;
    const DrawApi& d = q->platform->draw;

    render_sort_and_batch(q, w->scratch);          // steps 1-2
    render_emit_geometry(q);                        // step 3

    // step 4: window clear (the letterbox bars), then the layer/target walk.
    TL_CHECK(d.set_target(d.ctx, TexHandle{}) == ERR_OK);
    d.set_clip(d.ctx, nullptr);
    d.clear(d.ctx, 0xFF000000u);

    u8 cur_layer = 0xFFu;
    u32 draw_calls = 0;
    for (u32 bi = 0; bi < q->batches.count; ++bi) {
        const Batch b = q->batches.data[bi];
        if (b.layer != cur_layer) {
            if (cur_layer != 0xFFu && !handle_is_null(q->target[cur_layer])) { blit_layer_to_window(q, cur_layer); }
            cur_layer = b.layer;
            TL_CHECK(d.set_target(d.ctx, q->target[cur_layer]) == ERR_OK);
            d.set_clip(d.ctx, nullptr);
            if (!handle_is_null(q->target[cur_layer])) { d.clear(d.ctx, q->clear_rgba[cur_layer]); }
        }
        d.set_clip(d.ctx, b.clip != 0u ? &q->clips.rects[b.clip] : nullptr);
        TL_CHECK(d.draw_geometry(d.ctx, TexHandle{ b.tex }, q->verts.data + (u64)b.first * 4u, b.count * 4u,
                                 q->idx.data + (u64)b.first * 6u, b.count * 6u) == ERR_OK);
        draw_calls += 1;
    }
    if (cur_layer != 0xFFu && !handle_is_null(q->target[cur_layer])) { blit_layer_to_window(q, cur_layer); }

    // step 5 (dev: ImGui) - the editor lane's hook; no PlatformDevApi exists yet (TODO.md, chains
    // after this lane per TODO.md's top blockquote), so there is nothing to call here in v0.

    q->stats_draw_calls = draw_calls;
    q->stats_batches = q->batches.count;
}

void render_present(World* w) {
    RenderQueue* q = w->render;
    render_build_frame(w);
    q->platform->draw.present(q->platform->draw.ctx);   // step 6

    // step 7 (the STATS NOTE above): reset the command buffers and the accumulating submit/
    // reject counters for the next frame; stats_draw_calls/stats_batches stay published (this
    // frame's final values) for a caller to read after this call returns.
    q->stats_submitted = 0;
    q->stats_rejected = 0;
    q->keys.count = 0;
    q->data_index.count = 0;
    q->clip_id.count = 0;
    q->order.count = 0;
    q->batches.count = 0;
    q->verts.count = 0;
    q->idx.count = 0;
    q->data.count = 0;
    q->clips.count = 1;   // slot 0 stays reserved for "no clip" (mirrors render_init, queue.cpp)
    q->clips.depth = 0;   // review round 1 D4: an unpopped clip must not leak into the next frame
}
