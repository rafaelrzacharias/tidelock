#pragma once
// ---------------------------------------------------------------------------------------------
// debugdraw.h - immediate lines/rects/circles/text in world or screen space, + persistent
//   (n-tick) variants for sim debugging.
//
// Spec: docs/RENDER2D.md §7 (design), §9.3.8 (algorithms), §9.1 (file layout: TL_DBG_* macros
//   expand to ((void)0) outside TL_DEV - the tl_prof.h/tl_probe.h precedent, review round 1 D7 -
//   this TU itself still builds and is still tested in every tier).
// Purpose: data-only geometry into the LAYER_DEBUG draw buffer - per-system `debug_draw(World*)`
//   hooks (docs/TOOLING.md §2, the editor lane) emit here through the TL_DBG_* macros; this
//   module owns only the primitives (and the macros gating them).
// Invariants: butt caps, no joins (polylines are independent segments); all debug geometry goes
//   to LAYER_DEBUG, null texture, depth = submission order (docs/RENDER2D.md §9.3.8). Persistent
//   entries live in a 4096-entry ring on a render-owned arena and are re-emitted every frame
//   while `until_tick > world.tick` (docs/RENDER2D.md §7).
// Determinism: none - dev-tooling. Outside TL_DEV every TL_DBG_* call site compiles to
//   ((void)0), argument list unevaluated, so no netcode/ship build pays for it; the underlying
//   dbg_* functions stay linkable in every tier (so this TU's own tests still run there) but no
//   sim TU calls them directly (the hook is per-system, but the call itself is dev-tooling,
//   docs/TOOLING.md §2).
// Threading: main thread, RENDER phase (or any dev-tier immediate caller).
// Includes: render/render.h.
// ---------------------------------------------------------------------------------------------
#include "render/render.h"

// TL_DBG_* (docs/CPP-SUBSET.md §7b macro catalogue): the call-site macros for the six emit
// entry points below, following the foundation/tl_prof.h and foundation/tl_probe.h precedent -
// this TU (debugdraw.cpp) still builds and is still tested in every tier (a caller like
// debugdraw.test.cpp may call dbg_line/dbg_rect/... directly, in any tier); only a CALL SITE
// that goes through these macros pays zero cost - argument list unevaluated - outside TL_DEV
// (review round 1 D7).
#if TL_DEV
#  define TL_DBG_LINE(w, ax, ay, bx, by, width_px, rgba, space) \
       dbg_line((w), (ax), (ay), (bx), (by), (width_px), (rgba), (space))
#  define TL_DBG_RECT(w, x, y, width, height, line_width_px, rgba, space) \
       dbg_rect((w), (x), (y), (width), (height), (line_width_px), (rgba), (space))
#  define TL_DBG_CIRCLE(w, cx, cy, r_px, line_width_px, rgba, space) \
       dbg_circle((w), (cx), (cy), (r_px), (line_width_px), (rgba), (space))
#  define TL_DBG_LINE_PERSIST(w, ax, ay, bx, by, width_px, rgba, space, ticks) \
       dbg_line_persist((w), (ax), (ay), (bx), (by), (width_px), (rgba), (space), (ticks))
#  define TL_DBG_RECT_PERSIST(w, x, y, width, height, line_width_px, rgba, space, ticks) \
       dbg_rect_persist((w), (x), (y), (width), (height), (line_width_px), (rgba), (space), (ticks))
#  define TL_DBG_CIRCLE_PERSIST(w, cx, cy, r_px, line_width_px, rgba, space, ticks) \
       dbg_circle_persist((w), (cx), (cy), (r_px), (line_width_px), (rgba), (space), (ticks))
#else
#  define TL_DBG_LINE(w, ax, ay, bx, by, width_px, rgba, space)                          ((void)0)
#  define TL_DBG_RECT(w, x, y, width, height, line_width_px, rgba, space)                ((void)0)
#  define TL_DBG_CIRCLE(w, cx, cy, r_px, line_width_px, rgba, space)                     ((void)0)
#  define TL_DBG_LINE_PERSIST(w, ax, ay, bx, by, width_px, rgba, space, ticks)           ((void)0)
#  define TL_DBG_RECT_PERSIST(w, x, y, width, height, line_width_px, rgba, space, ticks) ((void)0)
#  define TL_DBG_CIRCLE_PERSIST(w, cx, cy, r_px, line_width_px, rgba, space, ticks)      ((void)0)
#endif

enum DbgKind : u8 { DBG_LINE = 0, DBG_RECT = 1, DBG_CIRCLE = 2 };

enum : u32 { DBG_PERSIST_RING_CAP = 4096 };

// 40 B (docs/RENDER2D.md §9.3.8): p[6] holds the shape's params by kind - LINE: ax,ay,bx,by,
// width_px,_; RECT: x,y,w,h,line_width_px,_; CIRCLE: cx,cy,r_px,line_width_px,_,_.
struct DbgPersist { u64 until_tick; u8 kind; u8 space; u16 _pad0; u32 rgba; f32 p[6]; };
static_assert(sizeof(DbgPersist) == 40, "docs/RENDER2D.md section 9.3.8");

// Allocates the DBG_PERSIST_RING_CAP-entry ring from `arena` and wires it onto w->render (the
// RenderQueue.dbg_ring/dbg_ring_count/dbg_ring_next fields - render.h's own SIGNATURE NOTE: the
// element type is debugdraw.h's, so render.h carries it opaque). Call once, after render_init.
// The persistent dbg_*_persist calls TL_FATAL if this was never called (a null dbg_ring).
void debugdraw_init(World* w, VMemArena* arena);

// dbg_line(a, b, width_px, rgba, space): d = normalize(b - a) in target px; n = (-d.y, d.x)*
// width/2; quad a+n, b+n, b-n, a-n (docs/RENDER2D.md §9.3.8).
void dbg_line(World* w, f32 ax, f32 ay, f32 bx, f32 by, f32 width_px, u32 rgba, RectSpace space);

// dbg_rect = 4 dbg_line calls (docs/RENDER2D.md §9.3.8), corners of {x,y,w,h}.
void dbg_rect(World* w, f32 x, f32 y, f32 width, f32 height, f32 line_width_px, u32 rgba, RectSpace space);

// dbg_circle: N = clamp(ceil(r_px / 2), 8, 64) segments (docs/RENDER2D.md §9.3.8).
void dbg_circle(World* w, f32 cx, f32 cy, f32 r_px, f32 line_width_px, u32 rgba, RectSpace space);

// Persistent line (docs/RENDER2D.md §7): pushes a DbgPersist row (until_tick = current tick +
// ticks) into the ring - a true ring past DBG_PERSIST_RING_CAP entries, overwriting the OLDEST
// live one (a debug-visualization overflow is expected capacity pressure, not a caller bug -
// unlike Array's fixed-cap TL_FATAL, docs/CONTAINERS.md §1) - and draws it immediately this frame.
void dbg_line_persist(World* w, f32 ax, f32 ay, f32 bx, f32 by, f32 width_px, u32 rgba, RectSpace space, u64 ticks);
// Persistent rect - same ring/lifetime contract as dbg_line_persist.
void dbg_rect_persist(World* w, f32 x, f32 y, f32 width, f32 height, f32 line_width_px, u32 rgba, RectSpace space, u64 ticks);
// Persistent circle - same ring/lifetime contract as dbg_line_persist.
void dbg_circle_persist(World* w, f32 cx, f32 cy, f32 r_px, f32 line_width_px, u32 rgba, RectSpace space, u64 ticks);

// Re-emits every ring entry with until_tick > world.tick (docs/RENDER2D.md §7). Call once per
// frame, RENDER phase, before any one-shot dbg_* calls the same frame would double-draw.
void debugdraw_replay_persistent(World* w);
