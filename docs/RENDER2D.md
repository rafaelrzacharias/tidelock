# Render2D — coordinates, extract, submission, the sim view (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §10. Carries foundry D8/D9/D10 and fork 1
> (SDL_Render v0, SDL_GPU reserved) into C++ with an fx sim on the other side of the extract step.
> **Owns:** `src/render/camera.h`, `extract.h`, `queue.h`, `sprite.h`, `simview.h`,
> `backend_sdl.cpp` (the only TU that calls the platform `DrawApi`; no TU in `render/` includes an
> SDL header — `PLATFORM.md` §0, CANON firewall). Implementation spec: §9.
> **Nothing in this module is hashed or snapshotted. Floats are legal here, in `editor/`,
> `platform/` and `tools/` (CANON types table), and nowhere upstream.**

---

## 0. Coordinates (DECIDED — D8, with fx on the sim side)

| Space | +Y | Origin | Units | Type |
|---|---|---|---|---|
| **World (sim)** | up | centered | metres, `pos_t` | fx |
| **World (render)** | up | centered | metres, `f32` — after extract | float |
| **Screen / framebuffer / UI** | down | top-left | pixels | `i32` / `f32` |
| **Texture / texel** | down | top-left; UV(0,0) top-left; pixel centre `i+0.5` | texel | `u16` |
| **Sim view texel** | down (image) — the SDF chunk's own raster | chunk-local | `TEXEL` = 1/16 m | `u16` |

- **The world-Y-up → screen-Y-down flip is applied exactly once**, in `camera.h`'s
  `world_to_screen(mat3)` / `screen_to_world` — the single source of truth for projection,
  viewport, letterbox and picking math. Layr's hard-won lesson; the analysis docs literally open
  with "mixing them up will silently invert your code".
- **`Rect<T>` is `pos + size`, axis-named (`x,y,w,h`), `min/max` derived** — never `top/bottom`
  (the trap that flips visual meaning between spaces). `Rect<f32>` world/AABB, `Rect<i32>`
  screen/scissor, `Rect<u16>` texel. (`Rect<T>` is notation: the code spells three concrete POD
  structs, `Rect_f32`/`Rect_i32`/`Rect_u16`, in `foundation/rect.h` — see §9.2.)
- **One module owns pixel/texel ↔ world conversions with the half-texel shift baked in**
  (`simview.h` for the sim raster; Layr hit a real 0.5-texel shadow-hull bug).
- **Sim positions are never quantized to the pixel grid.** Pixel-snap is display-only, per view,
  to that view's `1/PPU`, applied after interpolation.
- `PixelsPerUnit` lives on the camera: 1 m = `PPU` pixels at zoom 1. A Noita-class view is
  `PPU = 16 × integer_scale` so one sim texel = an integer number of screen pixels.

---

## 1. Extract — the fx → float boundary (DECIDED)

`PRE_RENDER` runs one flat pass per interpolated column: `prev`/`current` `Transform` (fx) →
`to_f32` → lerp by `alpha` → the **render packet** (SoA float columns on main scratch: `x, y,
rot, sx, sy` per entity with a `Sprite`, plus the camera). Render systems read the packet and
columns that are *render-side* (Sprite's texture/uv/tint), never fx columns directly, and never
write anything the sim reads. This is INV-6 made structural: the only fx→float conversions in the
binary are in `extract.cpp` and the sim-view writer.

---

## 2. Camera (DECIDED — amended review round 1 D1, 2026-08-27: off the ECS)

```cpp
struct Camera2D { f32 cx, cy; f32 zoom; f32 rot_turns; f32 ppu; u8 pixel_snap; };   // plain value struct, RenderQueue.camera[view]
```

Double-buffered + interpolated like any transform (the camera follows an entity by reading its
*packet* position). Order: interpolate → snap. A view's `Presentation` (mode / internal res /
scaling / filter / pixel-snap / integer-letterbox) is a value struct; `resolve_layout(window,
presentation) → {internal_size, viewport, scale}` is the one function every pass asks — aspect
math cannot drift. Default: integer letterbox for the pixel-art/sim view, aspect-fit otherwise.

**Not an ECS component.** `Camera2D`/`CameraPrev`/`CameraFollow` live on `RenderQueue` — one slot
per view (`camera`/`camera_prev`/`camera_follow`, indexed `[0, camera_count)`), the same place
every other render-side field already lives — never as registered ECS components. The rev-1 text
above called `Camera2D` a "render-side component," registered via a hand-rolled empty-field
`ComponentInfo` (core's `FieldKind` enum has no float row). That empty field table hid the struct
from `world_reflection_hash` (schema-level) but not from `registry_hash_all` (arena-level — hashes
every `ARENA_HASHED` arena's raw bytes regardless of what its field table says), so a camera pan,
a follow retarget, or a window resize feeding a different zoom — none of them sim state — moved
the lockstep state hash. Two peers with identical sim state but different camera positions
desynced. Fixed by removing the ECS registration entirely: camera state cannot enter
`registry_hash_all` if it never touches a registered arena, so "nothing in this module is hashed
or snapshotted" (this doc's own caption) now holds by construction. `CameraFollow`'s
`target == Entity{}` means "no follow" for that view — `world_get<Transform>(w, target)` resolves
a never-issued generation to null on its own, so no separate has-follow flag is needed.

**`camera_prev` initialization vs. advance (ruled 2026-08-27, Rafael, relayed by the steward,
review round 2 N1).** `render_camera_init(w, view, cam)` seeds BOTH `camera[view]` and
`camera_prev[view]` to `cam` the first time a view is configured — the first frame's lerp is then
`prev == cur`, the identity, for any `alpha`, so `sys_extract`'s per-view loop stays an
unconditional lerp with no sentinel test and no branch. Before this ruling, a raw `camera[view]`
write left `camera_prev[view]` at `render_init`'s zero-fill (`ppu == 0`), and the first
`sys_extract()` call aborted (a singular `view_matrix`, tripping D10's `TL_CHECK(det != 0)` three
calls deep) — this closes that spec silence rather than working around it. Advancing
`camera_prev` on later ticks (the `camera_prev[v] = camera[v]` copy, once per sim tick, AFTER the
first) is still the future `app/wiring.cpp` lane's job (no consumer calls it before W4
v0-integration exists, same status as `sys_extract`/`render_present`/`simview_update` today) -
camera left core's generic Transform/TransformPrev ping-pong mechanism when it left the ECS, so it
needs its own explicit copy there, not core's; initialization is defined, only the per-frame
ADVANCE is unbuilt.

---

## 3. Submission — one primitive, a 64-bit sort key (DECIDED — D9)

```cpp
struct DrawCommand { u64 key; u32 data_index; u16 clip_id; u16 _pad0; };
// key = [ layer:8 ▸ blend:1 ▸ depth:24 ▸ material/texture:16 ▸ tiebreak:15 ]
u32  render_push_data(World*, f32 x, f32 y, f32 rot, f32 sx, f32 sy, Rect_u16 uv, u32 rgba, TexHandle);  // SoA draw data → index
void render_submit(World*, DrawCommand);                                                                 // keys in their own column
```

- **Keys in their own `u64` column; draw data in SoA columns indexed by `data_index`** (keys sort,
  data stays put). **LSD radix, base 256, stable** → deterministic order with submission-order
  tiebreak; scales to 1M.
- **v0 = painter's order:** depth-first, back-to-front everywhere; material batches only among
  equal depth. The **`blend` bit is reserved**: under SDL_GPU + a depth buffer the opaque bucket
  flips to material-first (hard batching; z-test handles overlap) while transparent stays
  depth-first. Designed in, zero v0 cost, `render_submit` never changes.
- **Batching:** after the sort, scan-batch consecutive equal `(texture, clip, blend)` → one
  `SDL_RenderGeometry` call per batch. Atlasing is the cook pipeline's job later.
- **Clipping and culling, three mechanisms:** (1) `clip_id` into a small clip-rect stack table
  (correctness; `SDL_SetRenderClipRect`); (2) **emission-time viewport reject** — an immediate
  draw fully outside the active view/clip is dropped before entering the buffer (O(1) AABB test;
  `rect_visible(r)` exposed); (3) **logical culling at the source** — bulk world draws query the
  visible set (the spatial index when it exists; the chunk grid for the sim view), and lists
  virtualize. Scissor clips pixels but still submits geometry; virtualization avoids the
  submission.
- **Built-in `sprite_render` system** queries `(packet, Sprite)` and submits — the data-oriented
  default. **Immediate submission is first-class and unrestricted** (debug, UI, procedural) —
  the same primitive.
- `Sprite { TexHandle tex; Rect<u16> src; u32 rgba; u8 layer; u8 flags /* flip_x, flip_y,
  visible */; i16 depth_bias; }` — `u16` texel rect (no half-texel trap), `unorm8×4` colour
  (never float), flags packed.

---

## 4. Layers, views, targets (DECIDED — D7's three-layer model)

Render commands carry a **layer** (8 bits of the key). A layer = target + content + optional
FX chain, composited bottom-to-top; v0 has three layers — `WORLD` (the sim view quad + sprites,
internal-res target, pixel-snapped), `UI` (Luau HUD later, window-res), `DEBUG` (ImGui + debug
draw). **Per-view internal-resolution target + one upscale blit** makes per-pixel cost scale with
internal res, not window res. Post-FX, viewport-to-texture, split-screen and multi-window are
layer/target concerns, never phases. Target descriptors carry a format so an HDR/float target is
expressible later (SDL_GPU path).

---

## 5. The sim view — Alloy to pixels (DECIDED shape; Milestone 2 builds it)

Alloy exposes read-only **views**: per terrain chunk (128² texels) an SDF + material-id raster and
a dirty flag; per body an SDF raster + pose; particle arrays (pos, species, temp/burning flags);
cavity/basin summaries; burning/ember views. The sim view is:

1. **A streaming texture per visible chunk** (`ASSETS-AND-DATA.md` §2), rewritten only when the
   chunk is dirty: material id → palette colour (a Luau-authored material palette compiled into a
   `u32[256]` LUT), SDF sign → opaque/transparent, one-texel edge tint per material (no coverage
   AA — §10 R-2; writer in §9.3.7). A CPU writer in `simview.cpp`; chunk-parallel via `JOBS.md`
   later (output keyed by chunk — free).
2. **Dynamic bodies** render as world-space quads sampling their own body-local SDF raster
   (rotation is free — the quad rotates, the raster doesn't), same streaming-texture path.
3. **Particles** as point sprites/quads from the particle view (species → colour, temp → tint),
   culled by the chunk grid; bulk basins render as fill polygons from the fill surface.
4. **Fire/embers/smoke** are derived from burning-state + ember views: render-side particles and
   tints, never sim state.

All of it goes through `render_submit` with world-space transforms — **one projection path**, no
special background blit (the D8 single-source-of-truth rule). Pixel-perfect when internal res =
`TEXEL` × integer scale with snap on.

---

## 6. Backends (DECIDED)

| | SDL_Render (v0) | SDL_GPU (reserved) |
|---|---|---|
| why | the sim view is a CPU pixel buffer: `SDL_TEXTUREACCESS_STREAMING` upload + `SDL_RenderGeometry` batched quads is exactly its sweet spot; no shader toolchain | custom shaders (lighting, post, palette LUT on GPU), compute for a future GPU-side view |
| seam | `backend_sdl.cpp` implements `present(sorted commands)` over the platform draw surface | a second `present`; the `blend` bit flips the opaque bucket; shaders compiled offline (`tools/`) |
| trigger | now | a shader a demo needs that SDL_Render cannot express |

Boundary altitude (A1): the **platform** owns device operations (create/upload/lock texture, draw
geometry, set clip, present); **render** owns orchestration (sort, batch, layers, targets). If
render needs an SDL type to express an algorithm, the wrap is missing a verb.

---

## 7. Text and debug draw

- **Dev text** is ImGui's (its own atlas). **Game text** (reserved, `RESERVED-SEAMS.md` §2):
  SDL_ttf rasterizes glyphs into an engine-owned atlas texture; a stateless layout function writes
  quads into a caller buffer (Layr's `TextLayoutEngine` shape); submitted as sprites.
- **Debug draw:** immediate lines/rects/circles/text in world or screen space, data-only into the
  draw buffer (tessellated lines via `RenderGeometry`); viewport-scoped; persistent-lifetime
  variants (n ticks) for sim debugging; dev tiers only. Per-system `debug_draw(World*)` hooks
  (`TOOLING.md` §2) emit here.

---

## 8. Tests

"Test the descriptor, not the pixels": sort-key packing/unpacking; radix order incl. ties;
batch boundaries; clip stack; emission reject; `resolve_layout` for letterbox/fit at odd window
sizes; world↔screen round trip and picking; half-texel alignment of the sim view writer (a known
SDF edge lands on the expected pixel column). Pixel goldens are nightly (FLIP-compared, headless
SDL software renderer), never PR-blocking.

---

## 9. Implementation specification

Scope: everything an engineer needs to build `src/render/` without guessing. `f32`/`f64` and
`<math.h>` are legal in every TU here (CANON types table; `CPP-SUBSET.md` §1). Nothing in this
module is hashed, snapshotted, or read by `sim/`. All structs are POD with explicit padding; all
lists are arena-backed fixed-capacity `Array<T>` (`CONTAINERS.md` §1 — overflow is `TL_FATAL`);
failures are `Result<T>`/`ErrCode` in the `ERR_RENDER_*` range. Draw verbs are the platform's
(`PLATFORM.md` §9.2 `DrawApi`, `DrawVertex`, `TexHandle`); this module never names an SDL type.

### 9.1 File layout — `src/render/` (lib `tl_render`)

| File | Contents | Compiled in |
|---|---|---|
| `render.h` | public header: `RenderQueue`, `render_init/shutdown/present`, `render_push_data`, `render_submit`, `render_draw_quad`, `rect_visible`, clip push/pop, `ERR_RENDER_*` | all tiers |
| `camera.h/.cpp` | `Camera2D`, `CameraPrev`, `CameraFollow`, `Presentation`, `Layout`, `Mat3`, `resolve_layout`, `view_matrix`, `world_to_screen`, `screen_to_world`, `pixel_snap` | all |
| `extract.cpp` | `sys_extract` (`PRE_RENDER`): fx→f32, lerp, packet, camera | all |
| `queue.h/.cpp` | `DrawCommand`, key pack/unpack, `DrawData` SoA, `ClipTable`, submit, reject | all |
| `batch.cpp` | key sort (calls `sort_u64_kv`), scan-batching, vertex/index emission | all |
| `sprite.cpp` | `Sprite` component + `sys_sprite_render` (`RENDER`) | all |
| `simview.h/.cpp` | material LUT, chunk/body/particle/basin writers over `sim/views.h`, `simview_texel_to_world` | all (Milestone 2; v0 ships the header + an empty update) |
| `debugdraw.h/.cpp` | immediate lines/rects/circles, persistent list, tessellation | all — the `foundation/tl_prof.h`/`tl_probe.h` precedent (`CPP-SUBSET.md` §7b): the TU itself builds and is tested in every tier; only a call site that goes through the `TL_DBG_*` macros pays for it, and outside `TL_DEV` those expand to `((void)0)`, argument list unevaluated |
| `text.cpp` | reserved stub: `text_layout()` returns `ERR_RENDER_UNSUPPORTED` | all |
| `backend_sdl.cpp` | `render_present`: sort → batch → `DrawApi` verbs. Includes `platform/platform.h` only; the name records the SDL_Render-shaped verb set. A future `backend_gpu.cpp` replaces this one TU | all |

`tl_render` links into `tidelock` and into `tl_tests` (with `tl_platform_headless`) for
`tests/render/`. The sim-only audit shape (`ARCHITECTURE.md` §5) never links it.

### 9.2 Structs

```cpp
// foundation/rect.h — one X-macro emits three structs + per-struct min/max/overlap helpers.
struct Rect_f32 { f32 x, y, w, h; };  struct Rect_i32 { i32 x, y, w, h; };  struct Rect_u16 { u16 x, y, w, h; };
// camera.h
struct Mat3 { f32 m[9]; };                       // row-major affine: [m0 m1 m2; m3 m4 m5; 0 0 1]
struct Camera2D   { f32 cx, cy; f32 zoom; f32 rot_turns; f32 ppu; u8 pixel_snap; u8 _pad0[3]; };        // 24 B, RenderQueue.camera[view] - not an ECS component (§2)
struct CameraPrev { f32 cx, cy; f32 zoom; f32 rot_turns; f32 ppu; u8 pixel_snap; u8 _pad0[3]; };        // 24 B, RenderQueue.camera_prev[view]
struct CameraFollow { Entity target; f32 off_x, off_y; };                                              // 12 B, RenderQueue.camera_follow[view]; target == Entity{} = no follow
struct Presentation { u16 internal_w, internal_h;   // 0,0 = render at window res (no internal target)
                      u8 mode;   /* PRES_INTEGER_LETTERBOX=0 | PRES_ASPECT_FIT=1 | PRES_STRETCH=2 */
                      u8 filter; /* FILTER_NEAREST=0 | FILTER_LINEAR=1 — the upscale blit only */
                      u8 pixel_snap; u8 _pad0; };                                                         // 8 B
struct Layout { Rect_i32 viewport;        // drawable (window) pixels, Y down
                u16 internal_w, internal_h;  // target size (== viewport.w/h when internal_w == 0)
                f32 scale_x, scale_y;     // internal px → viewport px; integer-valued under LETTERBOX
                f32 dpi_scale; };         // drawable_w / logical_w                                    // 32 B
```

```cpp
// queue.h — key bit layout, MSB→LSB: [63:56] layer:8 | [55] blend:1 | [54:31] depth:24 | [30:15] material:16 | [14:0] tiebreak:15
struct DrawCommand { u64 key; u32 data_index; u16 clip_id; u16 _pad0; };                               // 16 B
constexpr u64 key_pack(u8 layer, u8 blend, u32 depth24, u16 material, u16 tiebreak15) {
    return (u64(layer) << 56) | (u64(blend & 1u) << 55) | (u64(depth24 & 0xFFFFFFu) << 31)
         | (u64(material) << 15) | u64(tiebreak15 & 0x7FFFu); }
constexpr u8  key_layer(u64 k)    { return u8(k >> 56); }
constexpr u8  key_blend(u64 k)    { return u8((k >> 55) & 1u); }
constexpr u32 key_depth(u64 k)    { return u32((k >> 31) & 0xFFFFFFu); }
constexpr u16 key_material(u64 k) { return u16((k >> 15) & 0xFFFFu); }
constexpr u16 key_tiebreak(u64 k) { return u16(k & 0x7FFFu); }
static_assert(key_depth(key_pack(0xAB, 1, 0x123456, 0xBEEF, 0x7ABC)) == 0x123456);  // + one per field
enum : u8  { LAYER_WORLD = 0, LAYER_UI = 1, LAYER_DEBUG = 2, MAX_LAYERS = 8, MAX_VIEWS = 4 };
enum : u32 { DEPTH_BASIN = 0x000000, DEPTH_CHUNK = 0x100000, DEPTH_BODY = 0x200000, DEPTH_PARTICLE = 0x300000,
             DEPTH_SPRITE_BASE = 0x800000 };   // sprite depth24 = DEPTH_SPRITE_BASE + i32(depth_bias) * 256 ∈ [0, 0xFFFF00]
```

- `material` = `TexHandle.bits` (null = untextured, vertex colour only). `tiebreak` is
  caller-supplied (default 0); submission order is the final tiebreak *by sort stability*, so
  1M commands with equal keys present in submission order without consuming bits.
- Ascending key = draw order: lower depth draws first (back), `layer` is the top byte so all of
  layer 0 precedes layer 1 (one target switch per layer per frame).

```cpp
struct DrawData {                 // SoA on main scratch, reserved once per frame (cap = cvar render_max_commands, default 65536)
    f32* x; f32* y; f32* rot_turns; f32* sx; f32* sy;   // centre + rotation + size in the command's space (world m, or target px if flags bit2)
    Rect_u16* uv; u32* rgba; u16* tex; u8* flags;       // flags: bit0 flip_x · bit1 flip_y · bit2 screen_space · bit3 no_snap
    u32 count, cap; };
struct ClipTable { Rect_i32 rects[256]; u16 count; u16 stack[32]; u8 depth; u8 _pad0[3]; };   // id 0 = no clip; rects in target px of the command's layer
struct Batch { u16 tex; u16 clip; u8 blend; u8 layer; u16 _pad0; u32 first; u32 count; };   // 16 B; first/count index the sorted order
struct RenderPacket { f32* x; f32* y; f32* rot_turns; f32* sx; f32* sy; u32 count; u32 _pad0; }; // parallel to the Transform column (index = Transform dense index)
struct RenderQueue {
    Array<u64> keys; Array<u32> data_index; Array<u16> clip_id;   // the command columns; keys sort, data stays put
    Array<u32> order;            // sort output: order[i] = command index at sorted position i
    DrawData data; ClipTable clips; Array<Batch> batches;
    Array<DrawVertex> verts; Array<u32> idx;                        // 4n / 6n, scratch
    RenderPacket packet; Rect_f32 view_world[MAX_VIEWS]; Mat3 view_mat[MAX_VIEWS];
    Presentation pres[MAX_VIEWS]; Layout layout; TexHandle target[MAX_LAYERS]; u32 clear_rgba[MAX_LAYERS];
    u8 layer_view[MAX_LAYERS];   // which view's matrix a world-space layer uses (UI/DEBUG: 0xFF = screen space)
    // Camera state (§2, amended review round 1 D1) - not an ECS component; camera_count gates how
    // many of the MAX_VIEWS slots sys_extract processes.
    Camera2D camera[MAX_VIEWS]; CameraPrev camera_prev[MAX_VIEWS]; CameraFollow camera_follow[MAX_VIEWS]; u8 camera_count;
    f32 alpha; u32 stats_submitted, stats_rejected, stats_draw_calls, stats_batches; };
```

```cpp
// sprite.h — 20 B: rgba@0 src@4 tex@12 depth_bias@14 layer@16 flags@17 _pad0@18; src = {x,y,w,h} texels (ECS §6 kinds: no Rect kind, so XA(u16,4))
#define TL_FIELDS_Sprite(X, XA, XH) \
    X (u32, rgba) XA(u16, src, 4) XH(TexHandle, tex) X (i16, depth_bias) X (u8, layer) X (u8, flags) X (u16, _pad0)
TL_COMPONENT(Sprite)     // flags: bit0 flip_x · bit1 flip_y · bit2 visible
// core-owned, stated here because extract reads them (FRAME-LOOP §4): Transform { pos_t x, y; angle_t rot; u32 flags; }
// with TRANSFORM_SNAP = 1u<<0 in flags (the reference shape in CPP-SUBSET §8 names this word _pad0; it is the flags word);
// TransformPrev has the identical field list, registered by core immediately after Transform with flag HIDDEN,
// added/removed together with Transform so both columns share one dense order.
```

Views expected from `sim/views.h` (`ARCHITECTURE.md` §8 R-1; fx/integer only, read-only; render
converts with `to_f32`): `ChunkView { u16 cx, cy; const i16* sdf /*128², 4 frac bits, <0 = inside solid*/; const u8* material; u32 dirty_serial; }`
· `BodyView { pos_t x, y, x_prev, y_prev; q_t sin, cos, sin_prev, cos_prev; const i16* sdf; const u8* material; u16 w, h; u32 dirty_serial; }`
· `ParticleView { const pos_t* pos_x; const pos_t* pos_y; const u16* species; const u16* flags; u32 stride_bytes /*32 for the AoS hot row, 4 for SoA*/; u32 count; }`
· `BurnView { const u32* carrier; const u16* intensity; u32 count; }` · `BasinView { const pos_t* surf_x; const pos_t* surf_y; u32 surf_count; pos_t floor_y; u16 species; u16 _pad0; }`.
Chunk `(cx, cy)` covers world `x ∈ [−4096 + 8cx, −4096 + 8cx + 8)`, same for `y` with `cy`;
raster row 0 is the chunk's **top** (highest y), texel `(tx, ty)` centre is
`(−4096 + 8cx + (tx + 0.5)·TEXEL, −4096 + 8cy + 8 − (ty + 0.5)·TEXEL)` — `simview_texel_to_world`
is the one function that states this (§0's half-texel rule).

### 9.3 Algorithms

**9.3.1 `resolve_layout(win_w, win_h, logical_w, pres) → Layout`** (drawable px in, `PLATFORM.md` §9.3 window):
```
dpi_scale = logical_w ? f32(win_w) / f32(logical_w) : 1
if pres.internal_w == 0: viewport = {0,0,win_w,win_h}; internal = (win_w, win_h); scale = 1; return
iw, ih = pres.internal_w, pres.internal_h
LETTERBOX: s = max(1, min(win_w / iw, win_h / ih))  (integer division)   vw = iw*s; vh = ih*s; scale = (s, s)
ASPECT_FIT: s = min(f32(win_w)/iw, f32(win_h)/ih)                         vw = floor(iw*s); vh = floor(ih*s); scale = (s, s)
STRETCH:    vw = win_w; vh = win_h; scale = (f32(win_w)/iw, f32(win_h)/ih)
viewport = { (win_w - vw) / 2, (win_h - vh) / 2, vw, vh }    // integer centring; an odd spare pixel goes right/bottom
internal = (iw, ih)
```
A window smaller than `internal` under LETTERBOX yields `s = 1` and a viewport larger than the
window (clipped by the device) — never a non-integer scale.

**9.3.2 Projection** — the single Y flip is the sign of `m[4]`; nothing else in the module negates a Y.
```
view_matrix(cam, L, out):                       // world (m, +Y up) → internal target px (+Y down)
  ppu = cam.ppu * cam.zoom;  (c, s) = (cosf, sinf)(cam.rot_turns * 6.283185307f)
  cx, cy = cam.cx, cam.cy;  if cam.pixel_snap: cx = roundf(cx*ppu)/ppu; cy = roundf(cy*ppu)/ppu    // snap AFTER interpolation (§2)
  W, H = L.internal_w, L.internal_h
  m = [ ppu*c,  ppu*s,  W/2 - ppu*(c*cx + s*cy),
        ppu*s, -ppu*c,  H/2 + ppu*(c*cy - s*cx),
        0, 0, 1 ]
world_to_screen(M, wx, wy) → (M0*wx + M1*wy + M2, M3*wx + M4*wy + M5)          // target px
screen_to_world: affine inverse of M (det = -ppu² ≠ 0), applied to target px
target_to_window(L, tx, ty) → (L.viewport.x + tx*L.scale_x, L.viewport.y + ty*L.scale_y);   picking = inverse then screen_to_world
pixel_snap(px) = floorf(px + 0.5f)  applied to a sprite's target-space centre when pres.pixel_snap && !(flags & no_snap)
```
With `ppu = 16k` one world texel is exactly `k` target pixels, so a snapped sprite whose
world position is texel-aligned lands on a pixel boundary — the §0 pixel-perfect condition.

**9.3.3 Extract** (`sys_extract`, `PRE_RENDER`, the first system of the phase; `alpha` from the loop via `World.render->alpha`):
```
cur = world_column<Transform>(w); prev = world_column<TransformPrev>(w); n = cur.count   (TL_CHECK prev.count == n)
pk = packet_reserve(scratch_main, n)
for i in 0..n (ascending dense index):
  a = (cur[i].flags & TRANSFORM_SNAP) ? 1.0f : alpha        // the bit is read, never written here; the barrier clears it (FRAME-LOOP §3 step 3)
  x0 = to_f32(prev[i].x); x1 = to_f32(cur[i].x); pk.x[i] = x0 + (x1 - x0) * a;   same for y
  r0 = to_f32(prev[i].rot); r1 = to_f32(cur[i].rot)          // turns
  d = r1 - r0; d -= floorf(d + 0.5f)                           // shortest arc: d ∈ [-0.5, 0.5)
  pk.rot_turns[i] = r0 + d * a;  pk.sx[i] = pk.sy[i] = 1.0f    // scale columns exist for a future Scale component; v0 constant
cameras (§2, amended D1 - RenderQueue state, not ECS columns): TL_CHECK(camera_count <= MAX_VIEWS)
  for view in 0..camera_count: lerp cx, cy, zoom, ppu linearly from camera_prev[view] -> camera[view]; rot_turns shortest-arc as above
    if world_get<Transform>(w, camera_follow[view].target) resolves: cam.cx = pk.x[dense(target)] + off_x (same y) - target == Entity{} (no follow) resolves to null on its own
    view_world[view] = AABB of the 4 viewport corners through screen_to_world; view_mat[view] = view_matrix(cam, layout)
```
These two loops plus `simview.cpp` and `sprite.cpp` are this module's `to_f32` call sites (§9.5's
allowlist; not yet CI-enforced there).

**9.3.4 Submission and reject**
```
render_push_data(w, x, y, rot, sx, sy, uv, rgba, tex, flags) → u32   // appends one SoA row; TL_FATAL at cap
render_submit(w, DrawCommand c): c.clip_id = clips.stack[clips.depth-1] (0 if empty); keys.push(c.key); data_index.push(c.data_index); clip_id.push(c.clip_id); stats_submitted++
render_clip_push(w, Rect_i32 r) → u16 id (TL_FATAL at 256 / depth 32);  render_clip_pop(w)
rect_visible(w, Rect_f32 r, space) → bool:  v = (space == WORLD) ? view_world[layer_view[layer]] : clip rect (or target rect if clip 0) as f32
                                            return r.x <= v.x + v.w && r.x + r.w >= v.x && r.y <= v.y + v.h && r.y + r.h >= v.y   (non-strict)
render_draw_quad(w, layer, depth24, x, y, rot, sx, sy, uv, rgba, tex, flags):   // the immediate helper; sprite_render uses it too
  aabb = rotated quad AABB (half-diagonal bound: r = 0.5*sqrtf(sx²+sy²)) ; if !rect_visible: stats_rejected++; return
  d = render_push_data(...); render_submit(w, { key_pack(layer, 0, depth24, tex.bits, 0), d, 0, 0 })
```
`render_submit` itself never rejects (it is the raw primitive); reject is in the helper and in the
bulk writers. Bulk world draws cull at the source (chunk grid, §9.3.7) and then skip the AABB test.

**9.3.5 Sort** — `sort_u64_kv(keys, order, n, scratch)` from `foundation/sort.h` (`CONTAINERS.md` §4). Its contract, restated because the order is load-bearing:
```
order[i] = i for i in 0..n;  tmp_k = scratch u64[n]; tmp_v = scratch u32[n]   (frame arena; freed by the frame reset)
src = (keys, order); dst = (tmp_k, tmp_v)
for pass in 0..8:  shift = pass*8
  hist[256] = 0; for i in 0..n: hist[(src.k[i] >> shift) & 255]++
  if hist[(src.k[0] >> shift) & 255] == n: continue          // identity pass: skipping is a pure function of the keys → still deterministic
  off[0] = 0; for b in 1..256: off[b] = off[b-1] + hist[b-1]
  for i in 0..n (ascending — the stability guarantee): b = (src.k[i] >> shift) & 255; dst.k[off[b]] = src.k[i]; dst.v[off[b]] = src.v[i]; off[b]++
  swap(src, dst)
if src.k != keys: memcpy(keys, src.k, n*8); memcpy(order, src.v, n*4)
```
O(8n) worst, stable, no comparisons; 1M keys measured 89–144 ms on the CI container (review rounds
1 and 2, `radix_order`) - the test's own bound is < 5000 ms, a generous non-strict smoke check for
a gross algorithmic regression, not a perf grade (§9.6; `WORKFLOW.md` §4 owns real perf grading).
This paragraph previously said "≈ 8 ms... the test's upper bound is 30 ms" - neither number held
against a measurement (review round 2 N9); fixed to match §9.6's row, the one home for this bound.

**9.3.6 Scan-batching** (after the sort; `order` gives sorted position → command):
```
batches.clear(); cur = null
for i in 0..n: c = order[i]; k = keys[i]  (keys is sorted in place)
  key4 = (key_material(k), key_layer(k), key_blend(k), clip_id[c])
  if cur == null || key4 != cur.key4: batches.push({tex, clip, blend, layer, first = i, count = 0}); cur = last
  cur.count++
```
Depth is not a batch key: consecutive equal `(tex, layer, blend, clip)` at different depths merge
(the sort already fixed their order). A layer change always splits (target switch).

**9.3.7 Sim view** (`simview_update(w)`, first system of `RENDER`, before `sys_sprite_render`):
```
struct MaterialLut { u32 rgba[256]; u32 edge_rgba[256]; u8 edge_width[256]; u32 species_rgba[256]; u32 ember_rgba; };  // compiled at init from the Luau palette (UI VM data; 0xAABBGGRR)
struct ChunkSlot { u16 cx, cy; TexHandle tex; u32 seen_serial; u32 last_frame; };  slots[MAX_RESIDENT_CHUNKS = 256]; Map<u32 /*cy<<16|cx*/, u16> slot_of
1. visible chunk range from view_world[0]: cx0 = clamp(floor((minx + 4096) / 8), 0, 1023) … cy1 likewise; expect ≤ 16×12 at nominal zoom
2. for cy in cy0..cy1, for cx in cx0..cx1 (row-major = chunk key order): slot = slot_of.get(key) or acquire (free slot, else evict min last_frame; none → TL_FATAL "chunk budget")
   if slot.tex.null: slot.tex = draw.texture_create(128, 128, PIXFMT_RGBA8, TEX_STREAMING) (Result checked); slot.seen_serial = 0
   v = alloy_chunk_view(w->sim, cx, cy);  if v.dirty_serial != slot.seen_serial: jobs.push({slot, v}); staging[j] = scratch u32[16384]
3. parallel_for(jobs, job_count, grain = 1, write_chunk, ctx)         // JOBS §1; chunk index = job index; v0: a plain loop
   write_chunk(j): for ty in 0..128, tx in 0..128: i = ty*128+tx; m = v.material[i]; d = v.sdf[i]      // d in 1/16 texel
       if d >= 0: out[i] = 0                                                                          // air (liquids/gases are not in the SDF)
       else: ew = lut.edge_width[m]; out[i] = (ew != 0 && d > -(16 * i16(ew))) ? lut.edge_rgba[m] : lut.rgba[m]
4. upload, main thread, job order (DrawApi is main-thread-only — PLATFORM §9.3): p = draw.texture_lock(tex, &pitch); for row: memcpy(p + row*pitch, staging + row*128, 512); draw.texture_unlock(tex); slot.seen_serial = v.dirty_serial
5. submit every visible chunk (dirty or not): render_push_data(x = -4096 + 8cx + 4, y = -4096 + 8cy + 4, rot 0, sx = sy = 8, uv = {0,0,128,128}, 0xFFFFFFFF, slot.tex, 0) → key_pack(LAYER_WORLD, 0, DEPTH_CHUNK, tex.bits, 0)
   slot.last_frame = frame
```
No flip: raster row 0 is the chunk top and UV (0,0) is the texture's top-left (§0 table). The
quad spans exactly 128 texels so texel `k` maps to `u ∈ [k/128, (k+1)/128]` under NEAREST with no
half-texel offset; the offset lives only in `simview_texel_to_world` (picking, debug markers).

Bodies: per `BodyView` a `BodySlot {BodyHandle, TexHandle (w×h, streaming), seen_serial}`; same
writer over `w×h`; pose lerp `x = lerp(to_f32(x_prev), to_f32(x), alpha)`, angle from
`(sin, cos)` pairs lerped then `atan2f`; quad `sx = w·TEXEL, sy = h·TEXEL`, `DEPTH_BODY`. If
`views.h` ships without `*_prev`, draw the current pose (no lerp) — stated, not guessed.
Particles: one pass over `ParticleView` (stride-aware reads), AABB test against `view_world`
(20k × O(1); fine-tier culling reserved), quad side `2·TEXEL`, colour `species_rgba[species]`,
`flags & PARTICLE_BURNING → ember_rgba`, null texture, `DEPTH_PARTICLE` — one batch.
Basins: for each segment `i` of the fill surface emit the quad `(p_i, p_{i+1}, (p_{i+1}.x, floor_y), (p_i.x, floor_y))`
(x-monotone polyline → strip, no general triangulation), colour `species_rgba`, `DEPTH_BASIN` —
drawn *before* chunks so opaque rock occludes the strip's overdraw. Fire/embers: `BurnView` →
tinted quads at the carrier position, render-side particle budget `cvar render_ember_max`.

**9.3.8 Debug draw** (`debugdraw.cpp`): `dbg_line(a, b, width_px, rgba, space)`:
`d = normalize(b − a)` in target px; `n = (−d.y, d.x)·width/2`; quad `a+n, b+n, b−n, a−n`
(butt caps, no joins; polylines are independent segments). `dbg_rect` = 4 lines; `dbg_circle`:
`N = clamp(ceil(r_px / 2), 8, 64)` segments. Text forwards to ImGui's background draw list (dev).
Persistent: `DbgPersist { u64 until_tick; u8 kind; u8 space; u16 _pad0; u32 rgba; f32 p[6]; }`
(40 B) in a 4096-entry ring on a render-owned arena; each frame re-emits entries with
`until_tick > world.tick`. All debug geometry goes to `LAYER_DEBUG`, null texture, depth = submission.

### 9.4 `render_present(w)` — the exact sequence

```
q = w->render; n = q.keys.count
1. sort_u64_kv(q.keys, q.order, n, scratch)               (§9.3.5)
2. batch(q)                                                  (§9.3.6)
3. emit: for each batch, for j in first..first+count: c = order[j]; corners of the quad (x,y,sx,sy,rot; flip bits swap u0/u1, v0/v1; uv → normalized by texture size from the asset registry, 1/65536 when unknown)
         WORLD-space → view_mat[layer_view[layer]]; screen_space → identity (target px); snap if pres.pixel_snap
         4 DrawVertex + indices (b,b+1,b+2, b,b+2,b+3) — vertex colour = rgba
4. draw.set_target(null); draw.set_clip(null); draw.clear(0xFF000000)             // letterbox bars
   cur_layer = 0xFF
   for each batch b in order:
     if b.layer != cur_layer:
        if cur_layer != 0xFF && target[cur_layer] != null: blit(cur_layer)        // leave: set_target(null); draw_geometry(target[cur_layer] as texture, viewport quad, filter = pres.filter)
        cur_layer = b.layer; draw.set_target(target[cur_layer]) (null = window); draw.set_clip(null); if target != null: draw.clear(clear_rgba[cur_layer])
     draw.set_clip(b.clip ? &clips.rects[b.clip] : null)
     draw.draw_geometry(TexHandle{b.tex}, verts + b.first*4, b.count*4, idx + b.first*6, b.count*6);  stats_draw_calls++
   if cur_layer != 0xFF && target[cur_layer] != null: blit(cur_layer)
5. dev: platform_dev->imgui_render(ctx)     // ImGui's SDLRenderer3 backend draws straight to the window (PLATFORM §9.2 PlatformDevApi); accepted as dev-only
6. draw.present(ctx)
7. q.keys/data_index/clip_id/order/batches/verts/idx counts ← 0; stats published to the profiler counters (draw_calls, batches, submitted, rejected)
```
Targets: `target[LAYER_WORLD]` is created at init/resize by `resolve_layout` →
`texture_create(internal_w, internal_h, RGBA8, TEX_TARGET)`; UI/DEBUG draw to the window. A
resize event re-runs `resolve_layout` and recreates the target (old one destroyed).

### 9.5 Determinism note

Nothing here is hashed; no sim TU reads render state. **This module's own** fx→float conversions
are confined to `extract.cpp` (§9.3.3), `simview.cpp` (§9.3.7 body/particle/basin positions — the
SDF raster is `i16` and compared as an integer), and `sprite.cpp` (its one fx-palette ratio,
`fx::to_f32(fx::TEXEL)` — review round 2 N5 found round 1's M2 fix had introduced this call site
without updating the allowlist below; RULED 2026-08-27, Rafael, relayed by the steward: a
compile-time `TEXEL_M` constant in `foundation/fx_float.h` would have been the better engineering,
but `src/foundation/` is a different module's cone and this lane took it without a scoped
exception — the in-cone fix is naming `sprite.cpp` as this module's own third allowlisted site
instead, at the cost of one runtime `to_f32` call in place of a compile-time constant, accepted
deliberately). The allowlist (`FX-PALETTE.md` §6): `to_f32`/`to_f64` under `src/` only in
`render/extract.cpp`, `render/simview.cpp`, `render/sprite.cpp`, `editor/`, and
`from_f32_quantized` only in `core/producers/live.cpp` and `editor/`. **Known to not hold on the
tree today** (review round 2 N5's own audit): `src/script/bind_fx.cpp` and `src/script/vm.h` also
call `to_f32`/`to_f64`, neither `render/` nor `editor/` — pre-existing on `main`, not this lane's
to fix, but the allowlist sentence is inaccurate as written until amended or those sites are
relocated. **Not yet CI-enforced** — no grep step exists in `tools/` or `.github/workflows/` for
this today (review round 1 D11); this paragraph states the intent other lanes are expected to
honour, not a live gate. Filed in `TODO.md` as RR-24, amended with the `script/` finding, for
whichever lane owns CI tooling. `-ffast-math` stays off (`CPP-SUBSET.md` §7).

### 9.6 Tests — `tests/render/` (in `tl_tests`, headless platform)

| Test | Asserts |
|---|---|
| `key_pack_unpack` | every field round-trips at 0, max, and a random 10k sample; field masks do not overlap (`static_assert` set) |
| `radix_order` | sorted ascending; equal keys keep submission order (ties at every byte) over 1M random keys, checked against a naive stable-insertion-sort oracle on a 200-sample slice; all-identical 1M keys unchanged; < 5000 ms — a generous, non-strict smoke bound catching only a gross algorithmic regression (e.g. an accidental O(n²)), not a perf grade (`WORKFLOW.md` §4 owns that) |
| `batch_boundaries` | splits exactly on tex/clip/blend/layer changes, never on depth; counts sum to n; empty queue → 0 batches |
| `clip_stack` | push/pop ids, submit stamps the top, depth 32 overflow → fatal (child process), id 0 when empty |
| `reject` | quads fully outside `view_world` increment `stats_rejected` and add no command; touching-edge quads are kept |
| `resolve_layout_table` | table of (win, internal, mode) → expected viewport/scale: 1280×720/320×180 LETTERBOX → s=4 centred; 1279×719 → s=3, viewport 960×540 at (159,89); 300×100 → s=1 oversize; ASPECT_FIT 1000×1000/320×180 → 1000×562 at (0,219); dpi 2.0 |
| `world_screen_roundtrip` | 10k random points, zoom ∈ {0.5,1,4}, rot ∈ {0,0.125,0.5}: `screen_to_world(world_to_screen(p))` within 1e-3 m; the Y flip: world +Y maps to decreasing screen Y; picking through `target_to_window` inverse |
| `simview_half_texel` | **Milestone 2** (needs the SDF-raster writer over `sim/views.h`, not yet on `main`): a synthetic chunk with an SDF edge at texel column 37 (`sdf < 0` for `tx ≥ 37`), ppu = 32, camera at the chunk origin: the first opaque staging pixel is column 37 and the chunk quad's left edge lands on target px `W/2 − 128·2·…` exactly |
| `simview_texel_to_world_half_texel_rule` | the pure half of the row above, landed early (review round 2 N10 - no `sim/views.h` dependency): `simview_texel_to_world(cx, 37, 0).x == −4096 + 8cx + 37.5·TEXEL`; row 0 is the chunk's TOP, not bottom; row 0 and row 127 are 127 texels apart |
| `present_descriptor` | headless `DrawApi` records calls: a 3-layer queue (WORLD with its internal target, UI/DEBUG to the window, per §9.4's own "Targets" paragraph) yields `set_target` ×5 (the step-4 top-level window clear's own call, world target, the WORLD blit's own call back to window, then window again for UI and for DEBUG - each layer transition calls `set_target` unconditionally, so two consecutive window-target layers are two calls, not a merged one), `clear` ×2 (the top-level window clear plus WORLD's own - UI/DEBUG have a null target, so step 4's `if target != null` guards their clear out), `draw_geometry` ×4 in the raw call log (one per batch, three, plus the WORLD blit's own quad) while `stats_draw_calls == stats_batches` (three, the per-batch count the blit is deliberately not part of), one blit, one `present`; every `draw_geometry` call (batches and the blit alike) carries 4 vertices |
| `extract_snap_and_arc` | snap bit forces `a = 1`; rotation 0.9 → 0.1 turns lerps through 1.0, not 0.5 |
| `camera_state_is_not_hashed` | `registry_hash_all` is invariant under `RenderQueue.camera`/`camera_follow` changes (a pan, `±0.0f`, a follow retarget) — the review round 1 D1 invariant §2's caption claims |

Pixel goldens (FLIP-compared, `--render=software`) are nightly, never PR-blocking (§8).

### 9.7 Build order and done criteria

1. `foundation/rect.h`, `camera.h/.cpp` + `resolve_layout_table`, `world_screen_roundtrip`.
2. `queue.h/.cpp` + `key_pack_unpack`, `clip_stack`, `reject`; `batch.cpp` + `radix_order`, `batch_boundaries`.
3. `backend_sdl.cpp` over the headless `DrawApi` + `present_descriptor`; then on sdl3: a window that clears and presents.
4. `extract.cpp`, `sprite.cpp` + `extract_snap_and_arc`; a textured sprite moving under interpolation at 144 Hz render / 60 Hz sim shows no stutter (manual).
   **render2d v0 done (this module's own provable scope — review round 1 D8):** steps 1–4
   green on BOTH the `dev` and `netcode` tiers (`WORKFLOW.md` §6 R-11); `stats_draw_calls ==
   stats_batches` asserted directly (`present_descriptor`); every render test exercises the
   real pipeline (`render_present`, `sys_extract`, `sys_sprite_render`, `render_build_frame`)
   repeatedly with no allocator tripwire fatal — `MEMORY.md` §2's CRT-malloc *counter* was
   dropped by a 2026-08-26 ruling (`foundation/alloc_shim.h`'s contract block); the live
   mechanism is tripwires, not a number to assert, and it is satisfied. **"`tidelock` draws
   sprites through a real window" and "fingerprints logged" are NOT this module's to claim** —
   they need `app/wiring.cpp` (W4, not built by any merged lane yet) and belong to
   `ARCHITECTURE.md` §9's own **v0** milestone row, which spans SDL3 platform + assets + data
   compiler + Luau VMs + this module + the ImGui shell together. `TODO.md`'s "W3 render2d" lane
   notes carry the measured verification for the four bullets above.
5. Milestone 2: `simview.cpp` chunks (+ `simview_half_texel`), then bodies, particles, basins, burn; `parallel_for` adoption when `JOBS.md` lands (the writer is already chunk-keyed).
   **Milestone 2 done:** a carved chunk re-uploads only on `dirty_serial` change (counter test), ~60 visible chunks ≤ 1 ms CPU write + upload, graded per `WORKFLOW.md` §4 (the committed PC rev-2 record until the Deck re-anchors).

---

## 10. Rulings

- **R-1 One streaming texture per chunk.** Dirty tracking is per chunk already; ~30–60 visible
  chunks is ~60 draws, far below any SDL_Render concern; an atlas would couple upload granularity
  to dirty granularity for a bind count nobody has measured as a problem. Per-chunk is also what
  the chunk-parallel writer (`JOBS.md` §4) wants — one chunk, one output.
- **R-2 Sign test + one-texel edge tint, no coverage AA.** The art direction is texel-crisp
  (Noita-class); AA would blur the one thing the sim view must make legible — the carve edge.
  The edge tint colour and width (0 or 1 texel) come from the Luau material palette LUT per
  material, so "soft" materials can still read as such without an AA path.
- **R-3 Camera comes off the ECS (Rafael, 2026-08-27, review round 1 D1).** `Camera2D`/
  `CameraPrev`/`CameraFollow` were registered ECS components (a hand-rolled empty-field
  `ComponentInfo`, working around `core/reflect.h`'s missing float `FieldKind` row); their raw f32
  bytes still entered `registry_hash_all`'s per-arena byte hash despite the empty field table
  (that hash reads bytes, never the field table), so a camera pan/follow/resize desynced two
  lockstep peers with identical sim state. Fixed by moving camera state onto `RenderQueue` (§2)
  instead of chasing a better field-table workaround — it is structurally impossible to hash once
  it never touches a registered arena. See §2 for the full account and `camera_state_is_not_hashed`
  (§9.6) for the passing regression.

*Rev 1 — 2026-08-22, reconciled 2026-08-26 (w3-render2d): §9.6's `present_descriptor` row
corrected to match §9.4's own step-4 pseudocode and "Targets" paragraph exactly (the row's
"`set_target` ×3, one `clear` per layer" undercounted - the algorithm calls `set_target`
unconditionally on every layer transition and clears only when the layer's own target is
non-null; measured by building the actual sequence for the row's own 3-layer example, not
guessed). Reconciled 2026-08-27 (w3-render2d, review round 1 D1): §2/§9.1/§9.2/§9.3.3 amended for
R-3 above - `Camera2D`/`CameraPrev` drop their `view` field (array-indexed now, not stored), the
`RenderQueue` struct dump gains `camera`/`camera_prev`/`camera_follow`/`camera_count`, and §9.3.3's
extract pseudocode reads from RenderQueue state instead of ECS columns (its stale `TL_ASSERT
prev.count == n` also corrected to `TL_CHECK`, matching review round 1 D5's already-shipped code
fix that this pass had missed).*
