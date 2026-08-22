# Render2D — coordinates, extract, submission, the sim view (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §9. Carries foundry D8/D9/D10 and fork 1
> (SDL_Render v0, SDL_GPU reserved) into C++ with an fx sim on the other side of the extract step.
> **Owns:** `src/render/camera.h`, `extract.h`, `queue.h`, `sprite.h`, `simview.h`,
> `backend_sdl.cpp` (the only TU that includes SDL render headers).
> **Nothing in this module is hashed or snapshotted. Floats are legal here and only here (+editor).**

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
  screen/scissor, `Rect<u16>` texel.
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

## 2. Camera (DECIDED)

```cpp
struct Camera2D { f32 cx, cy; f32 zoom; f32 rot_turns; f32 ppu; u8 pixel_snap; u8 view; };   // render-side component
```

Double-buffered + interpolated like any transform (the camera follows an entity by reading its
*packet* position). Order: interpolate → snap. A view's `Presentation` (mode / internal res /
scaling / filter / pixel-snap / integer-letterbox) is a value struct; `resolve_layout(window,
presentation) → {internal_size, viewport, scale}` is the one function every pass asks — aspect
math cannot drift. Default: integer letterbox for the pixel-art/sim view, aspect-fit otherwise.

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
   `u32[256]` LUT), SDF sign → coverage, optional sub-texel AA from the SDF gradient. A CPU
   writer in `simview.cpp`; chunk-parallel via `JOBS.md` later (output keyed by chunk — free).
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

## 9. Open

- **O-1** Sim-view chunk texture granularity: one texture per chunk (many small uploads, trivial
  dirty tracking) vs one large atlas texture with sub-rect uploads (fewer binds). Lean: per chunk
  at Milestone 2; measure draw-call count at the real viewport (≈ 30–60 visible chunks).
- **O-2** SDF anti-aliasing in the CPU writer (coverage from distance) vs plain sign test (crisper,
  Noita-like). A Luau-tunable per material? Lean: sign test + a 1-texel edge tint; AA only if the
  art direction asks.

*Rev 1 — 2026-08-22.*
