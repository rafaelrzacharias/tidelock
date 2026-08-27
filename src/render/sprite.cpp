// ---------------------------------------------------------------------------------------------
// sprite.cpp - sys_sprite_render (RENDER phase): queries (packet, Sprite), submits.
// Spec: docs/RENDER2D.md §3, §9.2.
//
// SPRITE SIZE NOTE (a gap this lane filled - TODO.md): §9.2's Sprite struct has no explicit
// world-space width/height field - only the texel `src` rect. Chosen behaviour: the quad's
// world size is `src.w/src.h` TEXELS converted at CANON.md's fixed ratio (`TEXEL` = 1/16 m), so a
// sprite authored at native texel resolution renders pixel-perfect at PPU = 16 * integer_scale
// (docs/RENDER2D.md §0's own pixel-perfect condition). A future per-sprite/asset scale (the
// assets+data lane's data tables) can override this; nothing here assumes it will not.
// ---------------------------------------------------------------------------------------------
#include "render/sprite.h"
#include "core/transform.h"
#include "foundation/fx_float.h"
#include "foundation/fx_palette.h"

void sys_sprite_render(World* w) {
    RenderQueue* q = w->render;
    const Span<Sprite> sprites = world_column<Sprite>(w);
    const Span<Entity> ents = world_entities<Sprite>(w);
    const Transform* tbase = world_column<Transform>(w).data;

    // TEXEL is CANON.md's one home for this ratio (foundation/fx_palette.h) - derived, not
    // restated (review round 1 M2). fx::TEXEL_M (foundation/fx_float.h), not fx::to_f32(fx::TEXEL)
    // directly: a to_f32 CALL SITE here would be a third render-side site outside §9.5's allowlist
    // (extract.cpp, simview.cpp, editor/ only) - review round 2 N5 caught M2's fix doing exactly
    // that, in the same commit series that reasserted the allowlist for D11.
    const f32 texel_m = fx::TEXEL_M;

    for (u32 i = 0; i < sprites.count; ++i) {
        const Sprite s = sprites.data[i];
        if ((s.flags & SPRITE_VISIBLE) == 0u) { continue; }
        const Transform* t = world_get<Transform>(w, ents.data[i]);
        if (t == nullptr) { continue; }
        const u32 idx = (u32)(t - tbase);
        if (idx >= q->packet.count) { continue; }   // defensive: packet not populated for this index

        u8 flags = 0;
        if ((s.flags & SPRITE_FLIP_X) != 0u) { flags |= DRAWFLAG_FLIP_X; }
        if ((s.flags & SPRITE_FLIP_Y) != 0u) { flags |= DRAWFLAG_FLIP_Y; }
        const Rect_u16 uv{ s.src[0], s.src[1], s.src[2], s.src[3] };
        const i32 depth24 = (i32)DEPTH_SPRITE_BASE + (i32)s.depth_bias * 256;
        render_draw_quad(w, s.layer, (u32)depth24, q->packet.x[idx], q->packet.y[idx], q->packet.rot_turns[idx],
                         (f32)s.src[2] * texel_m, (f32)s.src[3] * texel_m, uv, s.rgba, s.tex, flags);
    }
}
