#pragma once
// ---------------------------------------------------------------------------------------------
// camera.h - Mat3, Camera2D/CameraPrev/CameraFollow, Presentation, Layout, resolve_layout,
//   view_matrix, world_to_screen/screen_to_world, pixel_snap.
//
// Spec: docs/RENDER2D.md §0 (coordinates - the one Y-flip site), §2 (camera design), §9.2
//   (struct shapes, pinned), §9.3.1 (resolve_layout algorithm), §9.3.2 (projection algorithm).
// Purpose: the single source of truth for world<->screen projection, viewport/letterbox layout
//   and picking math (docs/RENDER2D.md §0 - "mixing them up will silently invert your code").
//   Camera2D/CameraPrev are the render-side, float, double-buffered camera (interpolated the
//   same way as Transform/TransformPrev, docs/FRAME-LOOP.md §4); CameraFollow is optional.
// Invariants: the world-Y-up -> screen-Y-down flip is the sign of view_matrix's m[4], applied
//   exactly once, here, and nowhere else in the module (§0). pixel_snap is display-only, applied
//   AFTER interpolation, never to sim state.
// Determinism: none - every field here is f32; this module is never hashed or snapshotted
//   (docs/RENDER2D.md caption; §9.5). f32/<math.h> are legal in every TU under src/render/
//   (docs/CANON.md "Types"; docs/CPP-SUBSET.md §1).
// FIELD-KIND GAP (filed in TODO.md as a ruling request): Camera2D/CameraPrev/CameraFollow are
//   "render-side components" per docs/FRAME-LOOP.md §8.2 step 4 (registered alongside Transform),
//   but core/reflect.h's FieldKind enum (docs/ECS.md §10.2, core's own closed kind set) has no
//   float row - by design, since reflection feeds the hash/inspector/wire doors and every other
//   reflected struct in the tree is fx/int/handle. These three structs therefore hand-roll their
//   ComponentInfo below (field_count = 0, fields = nullptr) instead of going through
//   TL_COMPONENT/TL_FIELDS_Name - they are still valid world_register_component/world_column<T>
//   subjects (registration only needs a correct size/align + tl_info_of(T*)), just opaque to the
//   generic inspector and the reflection hash, which is accurate: nothing here is hashed
//   (docs/RENDER2D.md §9.5). Not a workaround for a missing feature of THIS module - the kind
//   set is core's (ECS lane, already merged), and adding a K_f32 row is that lane's call, not
//   this one's (docs/ROADMAP.md §0 rule 2).
// Threading: none - value types; resolve_layout/view_matrix/world_to_screen/screen_to_world are
//   pure functions.
// Includes: core/reflect.h (ComponentInfo/Entity), foundation/rect.h.
// ---------------------------------------------------------------------------------------------
#include "core/reflect.h"
#include "foundation/rect.h"

// row-major affine: [m0 m1 m2; m3 m4 m5; 0 0 1] (docs/RENDER2D.md §9.2)
struct Mat3 { f32 m[9]; };

struct Camera2D {                                          // 24 B, render-side component
    f32 cx, cy;
    f32 zoom;
    f32 rot_turns;
    f32 ppu;
    u8  pixel_snap;
    u8  view;
    u16 _pad0;
};
static_assert(__is_trivially_copyable(Camera2D), "reflected structs are POD (docs/ECS.md section 2)");
static_assert(sizeof(Camera2D) == 24, "docs/RENDER2D.md section 9.2");

struct CameraPrev {                                        // 24 B, hidden column, ping-ponged at the barrier
    f32 cx, cy;
    f32 zoom;
    f32 rot_turns;
    f32 ppu;
    u8  pixel_snap;
    u8  view;
    u16 _pad0;
};
static_assert(__is_trivially_copyable(CameraPrev), "reflected structs are POD (docs/ECS.md section 2)");
static_assert(sizeof(CameraPrev) == 24, "docs/RENDER2D.md section 9.2");

struct CameraFollow {                                       // 12 B, optional
    Entity target;
    f32 off_x, off_y;
};
static_assert(__is_trivially_copyable(CameraFollow), "reflected structs are POD (docs/ECS.md section 2)");
static_assert(sizeof(CameraFollow) == 12, "docs/RENDER2D.md section 9.2");

// Hand-rolled ComponentInfo doors (the FIELD-KIND GAP note above) - same shape TL_COMPONENT
// would generate, minus the field table. world_register_component/world_column<T>/world_get<T>
// resolve T through tl_info_of(const T*), same as any macro-declared component.
inline constexpr ComponentInfo Camera2D_info = {
    "Camera2D", fnv1a64("Camera2D", sizeof("Camera2D") - 1), (u32)sizeof(Camera2D), (u32)alignof(Camera2D), nullptr, 0, 0u, nullptr };
// Camera2D's ComponentInfo (the FIELD-KIND GAP note above); the typed-API hook world_add<T>/
// world_get<T>/world_column<T> resolve Camera2D through this, same as any TL_COMPONENT type.
constexpr const ComponentInfo* tl_info_of(const Camera2D*) { return &Camera2D_info; }

inline constexpr ComponentInfo CameraPrev_info = {
    "CameraPrev", fnv1a64("CameraPrev", sizeof("CameraPrev") - 1), (u32)sizeof(CameraPrev), (u32)alignof(CameraPrev), nullptr, 0, COMP_HIDDEN, nullptr };
// CameraPrev's ComponentInfo, COMP_HIDDEN (the ping-ponged prev column - docs/FRAME-LOOP.md §4).
constexpr const ComponentInfo* tl_info_of(const CameraPrev*) { return &CameraPrev_info; }

inline constexpr ComponentInfo CameraFollow_info = {
    "CameraFollow", fnv1a64("CameraFollow", sizeof("CameraFollow") - 1), (u32)sizeof(CameraFollow), (u32)alignof(CameraFollow), nullptr, 0, 0u, nullptr };
// CameraFollow's ComponentInfo (optional component: a camera entity may carry it or not).
constexpr const ComponentInfo* tl_info_of(const CameraFollow*) { return &CameraFollow_info; }

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
