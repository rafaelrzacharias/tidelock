// simview.test.cpp - docs/RENDER2D.md §9.6 simview_half_texel, the PURE half:
// simview_texel_to_world is a self-contained function of CANON constants with no sim/views.h
// dependency, so it lands now rather than waiting on Milestone 2's SDF-raster writer (which the
// row's full description also covers - review round 2 N10 found the pure half shipped, live,
// with no test at all).
#include "runner/tl_test.h"
#include "render/simview.h"
#include <math.h>

TL_TEST(simview_texel_to_world_half_texel_rule, "render") {
    // docs/RENDER2D.md §9.6's own row: simview_texel_to_world(cx, 37, 0).x == -4096 + 8cx + 37.5*TEXEL
    // (TEXEL = 1/16 m, foundation/fx_palette.h).
    const u16 cx = 3, cy = 5;
    const f32 texel = 1.0f / 16.0f;
    f32 wx, wy;
    simview_texel_to_world(cx, cy, 37, 0, &wx, &wy);
    const f32 expected_x = -4096.0f + 8.0f * (f32)cx + 37.5f * texel;
    TL_EXPECT_TRUE(fabsf(wx - expected_x) < 1e-3f);

    // ty counts DOWN from the chunk's TOP (row 0 = highest y) while world y counts UP - texel row
    // 0 lands at the chunk's top edge (cy*8 + 8 - 0.5*TEXEL), not the bottom (docs/RENDER2D.md §0).
    const f32 expected_y_row0 = -4096.0f + 8.0f * (f32)cy + 8.0f - 0.5f * texel;
    TL_EXPECT_TRUE(fabsf(wy - expected_y_row0) < 1e-3f);

    // The chunk's own extent is exactly 8 m/axis (docs/CANON.md: CHUNK_TEXELS=128, TEXEL=1/16) -
    // texel row 0 and row 127 (the two extremes) are 127 texels apart in world y.
    f32 wx0, wy0, wx127, wy127;
    simview_texel_to_world(0, 0, 0, 0, &wx0, &wy0);
    simview_texel_to_world(0, 0, 0, 127, &wx127, &wy127);
    TL_EXPECT_TRUE(fabsf((wy0 - wy127) - 127.0f * texel) < 1e-3f);
}

// Review round 3 A-4: simview_update's body is `(void)w;` only (v0's stub, per its own contract
// comment - not yet registered in any phase schedule, Milestone 2's job to fill in once
// sim/views.h lands) - this call pins the symbol at LINK time, the same class of signal D6's own
// record already relies on for simview_texel_to_world, so deleting the function body (not just
// registering it somewhere) is still a build failure, not a silently-passing gap.
TL_TEST(simview_update_stub_is_linkable, "render") {
    simview_update(nullptr);   // never dereferences w - safe, and the point is only that it links
    TL_EXPECT_TRUE(true);      // deliberately trivial - the signal is link-time (deleting the body
                                // makes tl_tests fail to link, the same class of gate D6 relies on
                                // for simview_texel_to_world); a runner with zero recorded checks
                                // reports FAIL, not PASS, so this keeps the test from being vacuous
}
