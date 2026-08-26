// ---------------------------------------------------------------------------------------------
// simview.cpp - simview_texel_to_world (the half-texel rule, pure and self-contained) plus v0's
//   empty simview_update stub. Spec: docs/RENDER2D.md §0, §9.2, §9.3.7; see simview.h's CRITICAL
//   DEPENDENCY note for why the Milestone-2 writer isn't here yet (sim/views.h not on main).
// ---------------------------------------------------------------------------------------------
#include "render/simview.h"
#include "foundation/fx_float.h"
#include "foundation/fx_palette.h"

// docs/RENDER2D.md §0 (half-texel rule) / §9.2: chunk (cx,cy) covers world
// [-WORLD_HALF + chunk_m*cx, +chunk_m) on both axes; raster row 0 is the chunk's TOP (highest y),
// so ty counts DOWN while world y counts UP - hence the `chunk_m - (ty+0.5)*texel` term. All three
// magic numbers (world half-extent, texel size, chunk size) are derived from CANON.md's own
// fx_palette.h constants rather than restated (one fact, one home).
void simview_texel_to_world(u16 cx, u16 cy, u16 tx, u16 ty, f32* wx, f32* wy) {
    const f32 world_half = fx::to_f32(fx::WORLD_HALF);
    const f32 texel = fx::to_f32(fx::TEXEL);
    const f32 chunk_m = (f32)fx::CHUNK_TEXELS * texel;

    *wx = -world_half + chunk_m * (f32)cx + ((f32)tx + 0.5f) * texel;
    *wy = -world_half + chunk_m * (f32)cy + chunk_m - ((f32)ty + 0.5f) * texel;
}

// v0: no view parameter yet (simview.h's CRITICAL DEPENDENCY note) - registered as an empty stub
// so the RENDER phase's schedule shape is stable across the Milestone-2 landing.
void simview_update(World* w) {
    (void)w;
}
