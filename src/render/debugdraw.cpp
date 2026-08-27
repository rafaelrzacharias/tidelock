// ---------------------------------------------------------------------------------------------
// debugdraw.cpp - immediate lines/rects/circles + the persistent ring.
// Spec: docs/RENDER2D.md §7, §9.3.8.
//
// LINE-AS-QUAD NOTE: a line segment is a rotated rectangle (centre = midpoint, sx = length,
// sy = width_px, rot = the segment's own angle) - exactly render_draw_quad's parameterization
// (docs/RENDER2D.md §9.3.4), so dbg_line reuses it rather than pushing raw quad corners through
// a second code path. depth24 = the current submission count (docs/RENDER2D.md §9.3.8 "depth =
// submission") - RenderQueue.stats_submitted already increments once per render_submit call
// across every draw this frame, so reading it before the call is a free, mutable-state-free
// monotonic counter (docs/CPP-SUBSET.md §1 bans a static one).
// ---------------------------------------------------------------------------------------------
#include "render/debugdraw.h"
#include "render/render_internal.h"
#include <math.h>
#include <string.h>

void debugdraw_init(World* w, VMemArena* arena) {
    RenderQueue* q = w->render;
    const u64 bytes = (u64)DBG_PERSIST_RING_CAP * sizeof(DbgPersist);
    q->dbg_ring = arena_push(arena, bytes, alignof(DbgPersist));
    memset(q->dbg_ring, 0, bytes);
    q->dbg_ring_count = 0;
    q->dbg_ring_next = 0;
}

// The view LAYER_DEBUG's world-space commands project through (the D2/D3 0xFF fallback,
// render_internal.h), and that view's effective scale (ppu * zoom, robust to rotation - the
// magnitude of the matrix's first basis vector). §9.3.8's width/radius are ALWAYS target px
// (review round 2 N4): a WORLD-space command's own sx/sy share ITS space (world m), so a target-px
// width/radius must be converted to world metres here before being handed to render_draw_quad, or
// it comes out `ppu_eff` times too wide/large at emit and changes with zoom - the one thing a
// debug overlay's stroke weight must not do.
static f32 debug_view_ppu_eff(RenderQueue* q) {
    const u8 view = render_resolve_view(q, LAYER_DEBUG);
    const Mat3& M = q->view_mat[view];
    return sqrtf(M.m[0] * M.m[0] + M.m[3] * M.m[3]);
}

void dbg_line(World* w, f32 ax, f32 ay, f32 bx, f32 by, f32 width_px, u32 rgba, RectSpace space) {
    RenderQueue* q = w->render;
    const f32 dx = bx - ax, dy = by - ay;
    const f32 len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0f) { return; }   // a degenerate (zero-length) segment draws nothing
    const f32 rot = atan2f(dy, dx) / 6.283185307f;
    const f32 cx = (ax + bx) * 0.5f, cy = (ay + by) * 0.5f;
    const u32 depth24 = q->stats_submitted & 0xFFFFFFu;
    const u8 flags = space == RECT_SPACE_SCREEN ? DRAWFLAG_SCREEN_SPACE : 0u;
    f32 width = width_px;
    if (space == RECT_SPACE_WORLD) {
        const f32 ppu_eff = debug_view_ppu_eff(q);
        width = ppu_eff != 0.0f ? width_px / ppu_eff : 0.0f;
    }
    render_draw_quad(w, LAYER_DEBUG, depth24, cx, cy, rot, len, width, Rect_u16{ 0, 0, 0, 0 }, rgba, TexHandle{}, flags);
}

void dbg_rect(World* w, f32 x, f32 y, f32 width, f32 height, f32 line_width_px, u32 rgba, RectSpace space) {
    dbg_line(w, x, y, x + width, y, line_width_px, rgba, space);
    dbg_line(w, x + width, y, x + width, y + height, line_width_px, rgba, space);
    dbg_line(w, x + width, y + height, x, y + height, line_width_px, rgba, space);
    dbg_line(w, x, y + height, x, y, line_width_px, rgba, space);
}

void dbg_circle(World* w, f32 cx, f32 cy, f32 r_px, f32 line_width_px, u32 rgba, RectSpace space) {
    // The segment count is keyed on r_px itself (the ON-SCREEN radius, by §9.3.8's contract - see
    // debug_view_ppu_eff's comment) regardless of space, so it already tracks actual on-screen
    // size correctly. Only the GEOMETRY (the world-space centre offset) needs the px->world
    // conversion, same as dbg_line's width (review round 2 N4) - r_px is never a world distance.
    u32 n = (u32)ceilf(r_px * 0.5f);
    if (n < 8u) { n = 8u; }
    if (n > 64u) { n = 64u; }
    f32 r = r_px;
    if (space == RECT_SPACE_WORLD) {
        const f32 ppu_eff = debug_view_ppu_eff(w->render);
        r = ppu_eff != 0.0f ? r_px / ppu_eff : 0.0f;
    }
    f32 prev_x = cx + r, prev_y = cy;
    for (u32 i = 1; i <= n; ++i) {
        const f32 t = (f32)i / (f32)n * 6.283185307f;
        const f32 x = cx + r * cosf(t), y = cy + r * sinf(t);
        dbg_line(w, prev_x, prev_y, x, y, line_width_px, rgba, space);
        prev_x = x; prev_y = y;
    }
}

// Pushes one ring row; wraps at DBG_PERSIST_RING_CAP, overwriting the oldest (debugdraw.h's own
// contract - not a caller-bug TL_FATAL).
static void ring_push(World* w, DbgKind kind, u32 rgba, RectSpace space, u64 ticks,
                      f32 p0, f32 p1, f32 p2, f32 p3, f32 p4) {
    RenderQueue* q = w->render;
    TL_CHECK(q->dbg_ring != nullptr);   // debugdraw_init must run first
    DbgPersist* ring = (DbgPersist*)q->dbg_ring;
    DbgPersist e{};
    e.until_tick = w->state->tick + ticks;
    e.kind = (u8)kind; e.space = (u8)space; e._pad0 = 0; e.rgba = rgba;
    e.p[0] = p0; e.p[1] = p1; e.p[2] = p2; e.p[3] = p3; e.p[4] = p4; e.p[5] = 0.0f;
    ring[q->dbg_ring_next] = e;
    q->dbg_ring_next = (q->dbg_ring_next + 1u) % DBG_PERSIST_RING_CAP;
    if (q->dbg_ring_count < DBG_PERSIST_RING_CAP) { q->dbg_ring_count += 1u; }
}

void dbg_line_persist(World* w, f32 ax, f32 ay, f32 bx, f32 by, f32 width_px, u32 rgba, RectSpace space, u64 ticks) {
    ring_push(w, DBG_LINE, rgba, space, ticks, ax, ay, bx, by, width_px);
    dbg_line(w, ax, ay, bx, by, width_px, rgba, space);
}

void dbg_rect_persist(World* w, f32 x, f32 y, f32 width, f32 height, f32 line_width_px, u32 rgba, RectSpace space, u64 ticks) {
    ring_push(w, DBG_RECT, rgba, space, ticks, x, y, width, height, line_width_px);
    dbg_rect(w, x, y, width, height, line_width_px, rgba, space);
}

void dbg_circle_persist(World* w, f32 cx, f32 cy, f32 r_px, f32 line_width_px, u32 rgba, RectSpace space, u64 ticks) {
    ring_push(w, DBG_CIRCLE, rgba, space, ticks, cx, cy, r_px, line_width_px, 0.0f);
    dbg_circle(w, cx, cy, r_px, line_width_px, rgba, space);
}

void debugdraw_replay_persistent(World* w) {
    RenderQueue* q = w->render;
    if (q->dbg_ring == nullptr) { return; }   // debugdraw_init never called - nothing to replay
    const DbgPersist* ring = (const DbgPersist*)q->dbg_ring;
    const u64 tick = w->state->tick;
    // Insertion order, not ring-slot order (docs/RENDER2D.md §9.3.8 "depth = submission" - review
    // round 2 N11): before a wrap, slot order IS insertion order (entries fill 0, 1, 2, ...);
    // once wrapped (dbg_ring_count == DBG_PERSIST_RING_CAP), the OLDEST live entry sits at
    // dbg_ring_next - the slot the next push will overwrite - so that is where the walk starts.
    const u32 start = q->dbg_ring_count < DBG_PERSIST_RING_CAP ? 0u : q->dbg_ring_next;
    for (u32 k = 0; k < DBG_PERSIST_RING_CAP; ++k) {
        const u32 i = (start + k) % DBG_PERSIST_RING_CAP;
        const DbgPersist e = ring[i];
        if (e.until_tick <= tick) { continue; }   // expired, or a never-written (zeroed) slot
        const RectSpace space = (RectSpace)e.space;
        switch ((DbgKind)e.kind) {
            case DBG_LINE:   dbg_line(w, e.p[0], e.p[1], e.p[2], e.p[3], e.p[4], e.rgba, space); break;
            case DBG_RECT:   dbg_rect(w, e.p[0], e.p[1], e.p[2], e.p[3], e.p[4], e.rgba, space); break;
            case DBG_CIRCLE: dbg_circle(w, e.p[0], e.p[1], e.p[2], e.p[3], e.rgba, space); break;
        }
    }
}
