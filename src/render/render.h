#pragma once
// ---------------------------------------------------------------------------------------------
// render.h - RenderQueue, render_init/shutdown/present/resize, render_push_data, render_submit,
//   render_draw_quad, rect_visible, clip push/pop, ERR_RENDER_*: the public surface of tl_render.
//
// Spec: docs/RENDER2D.md §3 (design - D9, one primitive), §9.1 (file layout - this is the public
//   header every other render TU and every consumer module includes), §9.2 (RenderQueue's field
//   list, pinned), §9.3.4 (submission/reject algorithm), §9.4 (render_present's exact sequence,
//   implemented in backend_sdl.cpp), §9.7 (v0 done criterion).
// Purpose: everything above the sort/batch/backend split. render_init wires a RenderQueue onto
//   World (docs/CANON.md "Module/include firewall": render <- core, foundation, platform); every
//   draw path - the built-in sprite_render system and immediate callers alike (debug, UI,
//   procedural) - goes through render_submit (docs/RENDER2D.md §3, "the same primitive").
// Invariants: nothing here is hashed, snapshotted, or read by sim/ (docs/RENDER2D.md caption).
//   RenderQueue's Array columns are fixed-capacity (arena_init_fixed against the arena render_init
//   is given), sized once at init to `max_commands` (0 -> RENDER_DEFAULT_MAX_COMMANDS = 65536) -
//   TL_FATAL on overflow, never a relocating grow (docs/CONTAINERS.md §1). render_present's last
//   step resets every count to 0 (docs/RENDER2D.md §9.4 step 7); capacity never shrinks.
//   render_clip_push/render_push_data TL_FATAL at their stated caps (256 clips / depth 32, and
//   `max_commands`) - caller-side bugs, not recoverable failures (docs/CPP-SUBSET.md §3).
// SIGNATURE NOTE: RenderQueue.platform (below the pinned §9.2 field list) is this lane's own
//   addition - §9.2's struct dump has no DrawApi/PlatformApi field and no doc states how
//   backend_sdl.cpp's render_present(w) reaches the device verbs, so the queue carries the
//   PlatformApi pointer render_init is given. rect_visible's signature is reconstructed from
//   §9.3.4's elliptical one-liner ("v = ... view_world[layer_view[layer]] : clip rect...") -
//   `layer` is referenced but not in the shown 2-arg call, so it is restored as a parameter here
//   (RectSpace replaces the prose "WORLD"/other).
// Determinism: f32/<math.h> legal throughout src/render/ (docs/CANON.md "Types"). The fx->float
//   boundary is extract.cpp/simview.cpp only (docs/RENDER2D.md §9.5) - this header carries no fx
//   type.
// Threading: single-writer per frame (the RENDER phase's systems + any immediate caller on the
//   main thread); render_present is main-thread-only (DrawApi is, docs/PLATFORM.md §9.3).
// Includes: core/world.h (World, the registration doors), render/camera.h, render/queue.h,
//   platform/platform.h.
// ---------------------------------------------------------------------------------------------
#include "core/world.h"
#include "render/camera.h"
#include "render/queue.h"
#include "platform/platform.h"

// The render module's ErrCode range is 0x05xx (mem 0x01, jobs 0x02, core 0x03, net 0x04 -
// docs/CANON.md "Types"; 0x06/0x07 already taken by foundation/bytes.h and script/script.h).
constexpr ErrCode ERR_RENDER_INIT        = (ErrCode)0x0501;   // the WORLD layer's internal target failed to create
constexpr ErrCode ERR_RENDER_UNSUPPORTED = (ErrCode)0x0502;   // text.cpp's reserved stub (docs/RENDER2D.md §7, §9.1)

// Log-side name for a render ErrCode; "ERR_?" outside this header's codes.
constexpr const char* err_render_name(ErrCode e) {
    return e == ERR_OK ? "ERR_OK"
         : e == ERR_RENDER_INIT ? "ERR_RENDER_INIT"
         : e == ERR_RENDER_UNSUPPORTED ? "ERR_RENDER_UNSUPPORTED" : "ERR_?";
}

enum : u32 { RENDER_DEFAULT_MAX_COMMANDS = 65536 };

// docs/RENDER2D.md §9.2, plus `platform` (the SIGNATURE NOTE above).
struct RenderQueue {
    Array<u64> keys; Array<u32> data_index; Array<u16> clip_id;   // the command columns; keys sort, data stays put
    Array<u32> order;            // sort output: order[i] = command index at sorted position i
    DrawData data; ClipTable clips; Array<Batch> batches;
    Array<DrawVertex> verts; Array<u32> idx;                        // 4n / 6n, scratch
    RenderPacket packet; Rect_f32 view_world[MAX_VIEWS]; Mat3 view_mat[MAX_VIEWS];
    Presentation pres[MAX_VIEWS]; Layout layout; TexHandle target[MAX_LAYERS]; u32 clear_rgba[MAX_LAYERS];
    u8 layer_view[MAX_LAYERS];   // which view's matrix a world-space layer uses (UI/DEBUG: 0xFF = screen space)
    f32 alpha; u32 stats_submitted, stats_rejected, stats_draw_calls, stats_batches;
    const PlatformApi* platform;
    // The persistent debug-draw ring (docs/RENDER2D.md §7, §9.3.8) - opaque here (an untyped
    // pointer + two indices) because its element type (DbgPersist) is debugdraw.h's, and
    // debugdraw.h includes render.h, not the other way around (render.h is the base public
    // header - render/render.h's own contract block). debugdraw_init casts and owns it;
    // null/0/0 (render_init's zero-fill) means "no debug ring" - debugdraw calls before
    // debugdraw_init is a caller bug, TL_FATAL there, not here.
    void* dbg_ring;
    u32 dbg_ring_count;   // live entries pushed so far, capped at the ring's capacity
    u32 dbg_ring_next;    // next write index (wraps)
};

// Which rect rect_visible(...) tests against (the SIGNATURE NOTE above).
enum RectSpace : u8 { RECT_SPACE_WORLD = 0, RECT_SPACE_SCREEN = 1 };

// Allocates a RenderQueue on `arena` (fixed-capacity columns sized to max_commands, 0 = default),
// wires layer_view[LAYER_WORLD] = 0 (world-space) and layer_view[LAYER_UI]/[LAYER_DEBUG] = 0xFF
// (screen-space), resolves the initial Layout from the platform's current window size against
// `pres[0]`, and creates target[LAYER_WORLD] when pres[0].internal_w != 0. Sets w->render.
// ERR_RENDER_INIT if the internal target's texture_create fails.
ErrCode render_init(World* w, const PlatformApi* platform, VMemArena* arena, const Presentation* pres0, u32 max_commands);

// Destroys target[LAYER_WORLD] (and any other non-null layer target) if present. w->render is
// left pointing at freed content (the arena is the caller's to release - docs/RENDER2D.md
// caption: nothing here is hashed or snapshotted, so there is no restore path to protect).
void render_shutdown(World* w);

// Re-resolves `q->layout` from a new window size (docs/RENDER2D.md §9.3.1) and recreates
// target[LAYER_WORLD] at the new internal size when pres[0].internal_w != 0 (the old one
// destroyed first). Call from the platform's on_resize callback (docs/PLATFORM.md §9.2).
ErrCode render_resize(World* w, i32 win_w, i32 win_h, i32 logical_w);

// Appends one SoA row to q->data; TL_FATAL at cap (docs/RENDER2D.md §9.3.4).
u32 render_push_data(World* w, f32 x, f32 y, f32 rot, f32 sx, f32 sy, Rect_u16 uv, u32 rgba, TexHandle tex, u8 flags);

// The raw primitive: stamps c.clip_id from the top of the clip stack (0 if empty), appends to
// keys/data_index/clip_id, increments stats_submitted. Never rejects - reject lives in the
// helpers below (docs/RENDER2D.md §9.3.4).
void render_submit(World* w, DrawCommand c);

// Pushes a clip rect (target px of the command's layer) and returns its id. TL_FATAL at 256
// rects / stack depth 32.
u16  render_clip_push(World* w, Rect_i32 r);
// Pops the innermost clip pushed by render_clip_push. TL_ASSERT on underflow.
void render_clip_pop(World* w);

// True iff `r` overlaps the visible rect for `layer` in `space` (non-strict - touching edges
// count as visible): WORLD space tests view_world[layer_view[layer]]; SCREEN space tests the top
// of the clip stack, or the layer's target rect when the stack is empty (docs/RENDER2D.md §9.3.4).
bool rect_visible(World* w, Rect_f32 r, u8 layer, RectSpace space);

// The immediate helper sys_sprite_render also uses: computes the rotated quad's AABB (half-
// diagonal bound), rejects via rect_visible (WORLD space) before touching the data/command
// columns, else pushes the data row and submits with a 0 blend bit and 0 tiebreak
// (docs/RENDER2D.md §9.3.4).
void render_draw_quad(World* w, u8 layer, u32 depth24, f32 x, f32 y, f32 rot, f32 sx, f32 sy,
                      Rect_u16 uv, u32 rgba, TexHandle tex, u8 flags);

// docs/RENDER2D.md §9.4: sort -> batch -> emit -> the layer/target walk -> ImGui (dev) ->
// present -> reset counts and publish stats. Implemented in backend_sdl.cpp (the only TU that
// calls the platform DrawApi - docs/RENDER2D.md caption).
void render_present(World* w);

// sys_extract (extract.cpp): the first system of PRE_RENDER (docs/RENDER2D.md §9.3.3). fx ->
// f32, lerp by w->render->alpha, into w->render->packet; cameras interpolated the same way into
// view_world/view_mat. Depends on core/transform.h's Transform/TransformPrev (this lane's
// cross-lane landing note, TODO.md).
void sys_extract(World* w);
