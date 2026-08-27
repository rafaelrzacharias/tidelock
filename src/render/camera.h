#pragma once
// ---------------------------------------------------------------------------------------------
// camera.h - Mat3, Camera2D/CameraPrev/CameraFollow, Presentation, Layout, resolve_layout,
//   view_matrix, world_to_screen/screen_to_world, pixel_snap.
//
// Spec: docs/RENDER2D.md §0 (coordinates - the one Y-flip site), §2 (camera design), §9.2
//   (struct shapes, pinned), §9.3.1 (resolve_layout algorithm), §9.3.2 (projection algorithm).
// Purpose: the single source of truth for world<->screen projection, viewport/letterbox layout
//   and picking math (docs/RENDER2D.md §0 - "mixing them up will silently invert your code").
//   Camera2D/CameraPrev are the render-side, float, double-buffered camera state, held per view
//   on RenderQueue.camera/camera_prev (render.h) - NOT an ECS component (review round 1 D1, this
//   header's Determinism note below); interpolated the same way Transform/TransformPrev are
//   (docs/FRAME-LOOP.md §4), just by render's own code, not core's generic mechanism. CameraFollow
//   is optional per-view follow config, also plain RenderQueue state.
// Invariants: the world-Y-up -> screen-Y-down flip is the sign of view_matrix's m[4], applied
//   exactly once, here, and nowhere else in the module (§0). pixel_snap is display-only, applied
//   AFTER interpolation, never to sim state.
// Determinism: none - every field here is f32, and none of it is an ECS component any more
//   (this module is never hashed or snapshotted - docs/RENDER2D.md caption; §9.5). Review round 1
//   D1: Camera2D/CameraPrev/CameraFollow used to be registered via a hand-rolled empty-field
//   ComponentInfo (world_register_component only needs size/align + tl_info_of(T*), so an empty
//   field table was still a valid door) to work around core/reflect.h's FieldKind enum having no
//   float row. That empty field table hid the structs from `world_reflection_hash` (schema-level,
//   consults the field table) but NOT from `registry_hash_all` (arena-level, hashes every
//   ARENA_HASHED arena's raw bytes regardless of what its field table says) - registering these
//   structs at all put their f32 bytes in the lockstep state hash, so a camera pan/follow/resize
//   (none of them sim state) desynced two peers with identical sim state. Fixed by removing the
//   ECS registration entirely rather than chasing a better field-table workaround - camera state
//   now lives where every other render-side field already does (RenderQueue), never touching a
//   registered arena. The underlying core/reflect.h gap (no K_f32 row) may still matter for some
//   future reflected float struct; TODO.md's RR-23 entry is annotated moot for camera specifically
//   but left open for that general question, per docs/ROADMAP.md §0 rule 2 (not this lane's call).
//   f32/<math.h> are legal in every TU under src/render/ (docs/CANON.md "Types";
//   docs/CPP-SUBSET.md §1).
// Threading: none - value types; resolve_layout/view_matrix/world_to_screen/screen_to_world are
//   pure functions.
// Includes: core/reflect.h (Entity, for CameraFollow.target), foundation/rect.h.
// ---------------------------------------------------------------------------------------------
#include "core/reflect.h"
#include "foundation/rect.h"

// row-major affine: [m0 m1 m2; m3 m4 m5; 0 0 1] (docs/RENDER2D.md §9.2)
struct Mat3 { f32 m[9]; };

// 24 B, plain value struct on RenderQueue.camera[view] (render.h) - not an ECS component (this
// header's Determinism note above). No `view` field: which view a slot belongs to is its array
// index, not stored data.
struct Camera2D {
    f32 cx, cy;
    f32 zoom;
    f32 rot_turns;
    f32 ppu;
    u8  pixel_snap;
    u8  _pad0[3];
};
static_assert(__is_trivially_copyable(Camera2D), "");
static_assert(sizeof(Camera2D) == 24, "docs/RENDER2D.md section 9.2");

// 24 B, RenderQueue.camera_prev[view] - the previous-frame snapshot sys_extract interpolates
// from. render_camera_init (render.h) seeds this to the same value as camera[view] on a view's
// FIRST setup (ruled 2026-08-27, Rafael, relayed by the steward - docs/RENDER2D.md §2's full
// account), so the first sys_extract() call never lerps from a zeroed (ppu = 0) prev - review
// round 2 N1 found that a raw camera[v] write left camera_prev[v] at render_init's zero-fill and
// sys_extract aborted (view_matrix's ppu = 0 makes the matrix singular, tripping D10's own
// TL_CHECK(det != 0) in screen_to_world). ADVANCING it on later ticks (camera_prev[v] =
// camera[v], once per sim tick, after the first) is still the future app/wiring.cpp lane's job
// (docs/FRAME-LOOP.md's interp_pingpong hook, not yet built) - same as Transform/TransformPrev's
// own ping-pong, which core's generic mechanism drives for registered components only; camera
// left the ECS (D1 above), so it needs its own explicit copy there, not core's. Not this lane's
// to build the ADVANCE half ahead of that consumer (CLAUDE.md rule 8: "large subsystem = stable
// interface + ONE impl now, pulled in by a real consumer") - only the INIT half, which every
// caller needs immediately to configure a camera safely at all.
struct CameraPrev {
    f32 cx, cy;
    f32 zoom;
    f32 rot_turns;
    f32 ppu;
    u8  pixel_snap;
    u8  _pad0[3];
};
static_assert(__is_trivially_copyable(CameraPrev), "");
static_assert(sizeof(CameraPrev) == 24, "docs/RENDER2D.md section 9.2");

// 12 B, RenderQueue.camera_follow[view] - optional. `target == Entity{}` (the zero/never-issued-
// generation sentinel, docs/CPP-SUBSET.md §3's absence axis) means "no follow"; sys_extract's
// `world_get<Transform>(w, target)` naturally returns null for it, so no separate "has follow"
// flag is needed.
struct CameraFollow {
    Entity target;
    f32 off_x, off_y;
};
static_assert(__is_trivially_copyable(CameraFollow), "");
static_assert(sizeof(CameraFollow) == 12, "docs/RENDER2D.md section 9.2");

// Presentation/Layout (docs/RENDER2D.md §9.2) - plain value structs, never ECS components.
enum : u8 { PRES_INTEGER_LETTERBOX = 0, PRES_ASPECT_FIT = 1, PRES_STRETCH = 2 };
enum : u8 { FILTER_NEAREST = 0, FILTER_LINEAR = 1 };

struct Presentation {              // 8 B
    u16 internal_w, internal_h;    // 0,0 = render at window res (no internal target)
    u8  mode;                      // PRES_* above
    u8  filter;                    // FILTER_* above - the upscale blit only
    u8  pixel_snap;
    u8  _pad0;
};
static_assert(__is_trivially_copyable(Presentation), "");
static_assert(sizeof(Presentation) == 8, "docs/RENDER2D.md section 9.2");

struct Layout {                    // 32 B
    Rect_i32 viewport;             // drawable (window) pixels, Y down
    u16 internal_w, internal_h;    // target size (== viewport.w/h when internal_w == 0)
    f32 scale_x, scale_y;          // internal px -> viewport px; integer-valued under LETTERBOX
    f32 dpi_scale;                 // drawable_w / logical_w
};
static_assert(__is_trivially_copyable(Layout), "");
static_assert(sizeof(Layout) == 32, "docs/RENDER2D.md section 9.2");

// resolve_layout(win_w, win_h, logical_w, pres) -> Layout (docs/RENDER2D.md §9.3.1). Drawable px
// in (PLATFORM.md §9.3 window). A window smaller than `internal` under LETTERBOX yields s = 1
// and a viewport larger than the window (clipped by the device) - never a non-integer scale.
Layout resolve_layout(i32 win_w, i32 win_h, i32 logical_w, const Presentation& pres);

// view_matrix(cam, L) -> Mat3 (docs/RENDER2D.md §9.3.2): world (m, +Y up) -> internal target px
// (+Y down). The single Y flip in the whole module is the sign of m[4]. Snap is applied AFTER
// interpolation (docs/RENDER2D.md §2).
Mat3 view_matrix(const Camera2D& cam, const Layout& L);

// world_to_screen(M, wx, wy) -> target px (docs/RENDER2D.md §9.3.2).
void world_to_screen(const Mat3& M, f32 wx, f32 wy, f32* sx, f32* sy);

// screen_to_world: the affine inverse of M (det = -ppu^2 != 0), applied to target px.
void screen_to_world(const Mat3& M, f32 sx, f32 sy, f32* wx, f32* wy);

// target_to_window(L, tx, ty) -> window px (docs/RENDER2D.md §9.3.2); picking is the inverse
// then screen_to_world.
void target_to_window(const Layout& L, f32 tx, f32 ty, f32* wx, f32* wy);

// pixel_snap(px) = floorf(px + 0.5f) (docs/RENDER2D.md §9.3.2) - applied to a sprite's
// target-space centre when pres.pixel_snap && !(flags & no_snap).
f32 pixel_snap(f32 px);
