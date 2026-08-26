#pragma once
// ---------------------------------------------------------------------------------------------
// sprite.h - Sprite: the data-oriented default draw component, + sys_sprite_render (RENDER).
//
// Spec: docs/RENDER2D.md §3 ("built-in sprite_render system queries (packet, Sprite) and
//   submits - the data-oriented default"), §9.2 (the field list, pinned byte-for-byte), §9.1
//   (file layout: sprite.cpp is this component + the system).
// Purpose: the common case - an entity with Transform + Sprite draws itself, no immediate call
//   needed. Immediate submission (render_draw_quad/render_submit) stays first-class for debug/
//   UI/procedural draws (docs/RENDER2D.md §3).
// Invariants: `src` is a texel rect (u16, no half-texel trap - docs/RENDER2D.md §0); `rgba` is
//   unorm8x4 vertex colour, never float; flags packed (bit0 flip_x, bit1 flip_y, bit2 visible).
//   No Rect kind exists in core/reflect.h's FieldKind set (docs/ECS.md §10.2), so `src` is four
//   loose u16 fields via XA, exactly as the pinned struct spells it.
// TEX FIELD-KIND GAP (filed in TODO.md as a ruling request, same class as camera.h's note):
//   reflect.h's token-keyed kind table (§section "the token-keyed kind constants") has K_Tex in
//   the FieldKind enum but no `tl_field_kind_TexHandle` constant yet - its own comment assigns
//   that to "the not-yet-built handle domain's owner, beside its type definition". TexHandle is
//   platform's type (platform/platform.h) and platform's module DAG cannot include core/reflect.h
//   (core depends on platform, not the reverse - tools/audit/includes.py MODULE_DAG), so the
//   constant cannot physically live beside the typedef. render is the first module needing a
//   reflected TexHandle field and can see both headers (render's DAG: render, core, foundation,
//   platform), so it is added here rather than left as a compile error at TL_COMPONENT(Sprite).
// Determinism: none - render-only; f32 is not used here, but the module is never hashed anyway
//   (docs/RENDER2D.md caption).
// Threading: sys_sprite_render runs in the RENDER phase, main thread, v0 single-threaded
//   (docs/FRAME-LOOP.md §0).
// Includes: render/render.h (World, RenderQueue, render_draw_quad), foundation/rect.h.
// ---------------------------------------------------------------------------------------------
#include "render/render.h"

constexpr FieldKind tl_field_kind_TexHandle = K_Tex;   // the TEX FIELD-KIND GAP note above

// 20 B: rgba@0 src@4 tex@12 depth_bias@14 layer@16 flags@17 _pad0@18 (docs/RENDER2D.md §9.2).
#define TL_FIELDS_Sprite(X, XA, XH) \
    X (u32, rgba) XA(u16, src, 4) XH(TexHandle, tex) X (i16, depth_bias) X (u8, layer) X (u8, flags) X (u16, _pad0)
TL_COMPONENT(Sprite)   // flags: bit0 flip_x - bit1 flip_y - bit2 visible

enum : u8 { SPRITE_FLIP_X = 1u << 0, SPRITE_FLIP_Y = 1u << 1, SPRITE_VISIBLE = 1u << 2 };

static_assert(sizeof(Sprite) == 20, "docs/RENDER2D.md section 9.2");
static_assert(offsetof(Sprite, rgba) == 0 && offsetof(Sprite, src) == 4 && offsetof(Sprite, tex) == 12 &&
              offsetof(Sprite, depth_bias) == 14 && offsetof(Sprite, layer) == 16 &&
              offsetof(Sprite, flags) == 17 && offsetof(Sprite, _pad0) == 18,
              "docs/RENDER2D.md section 9.2 byte layout");

// Queries (RenderPacket, Sprite): for each visible Sprite whose entity also carries a Transform
// (the packet is parallel to Transform's dense column, docs/RENDER2D.md §9.2 RenderPacket
// comment), submits via render_draw_quad at LAYER_WORLD, depth24 = DEPTH_SPRITE_BASE +
// depth_bias*256 (docs/RENDER2D.md §9.2 DEPTH_SPRITE_BASE comment). A Sprite with SPRITE_VISIBLE
// clear, or with no Transform, is skipped (not an error - docs/RENDER2D.md §3's "the data-
// oriented default" does not require every entity to carry both).
void sys_sprite_render(World* w);
