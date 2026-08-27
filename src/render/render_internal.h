#pragma once
// ---------------------------------------------------------------------------------------------
// render_internal.h - the sort/batch/emit pipeline steps shared between batch.cpp and
//   backend_sdl.cpp. NOT part of tl_render's public surface (render.h is) - a caller outside
//   src/render/ has no reason to call these directly; render_present (backend_sdl.cpp) is the
//   one sequencer (docs/RENDER2D.md §9.4).
//
// Spec: docs/RENDER2D.md §9.3.5 (sort), §9.3.6 (scan-batching), §9.4 step 3 (vertex/index
//   emission - file layout assigns all three to batch.cpp, docs/RENDER2D.md §9.1).
// Purpose: split render_present's exact sequence (§9.4) so backend_sdl.cpp - the only TU that
//   touches DrawApi - stays a thin sequencer over these three steps plus the device calls.
// Invariants: render_sort_and_batch requires q->order to have capacity >= q->keys.count (true by
//   construction: both are sized to the same `cap` at render_init). render_emit_geometry must run
//   AFTER render_sort_and_batch (it reads q->batches/q->order/q->keys as sorted/batched already).
// Determinism: none - render-side pipeline, never hashed (docs/RENDER2D.md caption).
// Threading: main thread, called from render_present only.
// Includes: render/render.h.
// ---------------------------------------------------------------------------------------------
#include "render/render.h"

// Resolves which view a layer's WORLD-space content projects through. layer_view[layer] == 0xFF
// means the layer has no dedicated view of its own (UI/DEBUG - screen-composited targets), but a
// WORLD-space command CAN still land on one of those layers (world-tracking debug draw, a HUD
// sprite anchored to a world position) and needs a camera to project through: it falls back to
// view 0, the primary view. Shared by rect_visible (queue.cpp) and render_emit_geometry
// (batch.cpp) so the one fallback rule lives in one place (review round 1 D2/D3: an unguarded
// `view_mat[layer_view[layer]]`/`view_world[layer_view[layer]]` read the 0xFF sentinel as an
// index - a TL_CHECK abort in rect_visible, an out-of-bounds read in render_emit_geometry).
inline u8 render_resolve_view(const RenderQueue* q, u8 layer) {
    const u8 v = q->layer_view[layer];
    return v == 0xFFu ? 0u : v;
}

// docs/RENDER2D.md §9.3.5/§9.3.6: fills q->order with the identity permutation, calls
// sort_u64_kv(q->keys, q->order, n, scratch) (q->keys sorts in place), then scan-batches the
// sorted (material, layer, blend, clip) runs into q->batches. depth is deliberately not a batch
// key - the sort already fixed relative order among equal-key runs.
void render_sort_and_batch(RenderQueue* q, Scratch* scratch);

// docs/RENDER2D.md §9.4 step 3: walks q->batches in order, emitting 4 DrawVertex + 6 indices per
// command into q->verts/q->idx. World-space commands go through view_mat[layer_view[b.layer]];
// screen-space commands use target px directly. Pixel-snap (when the view's Presentation asks
// for it and the command's DRAWFLAG_NO_SNAP is clear) is applied to the quad's CENTRE only
// (docs/RENDER2D.md §9.3.2), never per-corner, so a snapped sprite keeps its shape.
void render_emit_geometry(RenderQueue* q);

// docs/RENDER2D.md §9.4 steps 1-4 (+ publishing stats_draw_calls/stats_batches) - render_present
// minus the final d.present(d.ctx) call and the step-7 reset, split out so a test can inspect the
// headless draw-call log before present() clears it (backend_sdl.cpp's own comment explains why).
void render_build_frame(World* w);
