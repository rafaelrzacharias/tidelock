// camera.test.cpp - docs/RENDER2D.md §9.6 resolve_layout_table, world_screen_roundtrip.
#include "runner/tl_test.h"
#include "render/camera.h"
#include <math.h>

static Presentation make_pres(u16 iw, u16 ih, u8 mode) {
    Presentation p{};
    p.internal_w = iw; p.internal_h = ih; p.mode = mode; p.filter = FILTER_NEAREST; p.pixel_snap = 0; p._pad0 = 0;
    return p;
}

TL_TEST(resolve_layout_table, "render") {
    // Row 1: 1280x720 / 320x180 LETTERBOX -> s=4, centred (evenly divides, so viewport == window).
    {
        Layout L = resolve_layout(1280, 720, 0, make_pres(320, 180, PRES_INTEGER_LETTERBOX));
        TL_EXPECT_EQ(L.scale_x, 4.0f);
        TL_EXPECT_EQ(L.scale_y, 4.0f);
        TL_EXPECT_EQ(L.viewport.x, 0); TL_EXPECT_EQ(L.viewport.y, 0);
        TL_EXPECT_EQ(L.viewport.w, 1280); TL_EXPECT_EQ(L.viewport.h, 720);
        TL_EXPECT_EQ(L.internal_w, 320u); TL_EXPECT_EQ(L.internal_h, 180u);
    }
    // Row 2: 1279x719 / 320x180 LETTERBOX -> s=3, viewport 960x540 at (159,89).
    {
        Layout L = resolve_layout(1279, 719, 0, make_pres(320, 180, PRES_INTEGER_LETTERBOX));
        TL_EXPECT_EQ(L.scale_x, 3.0f);
        TL_EXPECT_EQ(L.scale_y, 3.0f);
        TL_EXPECT_EQ(L.viewport.x, 159); TL_EXPECT_EQ(L.viewport.y, 89);
        TL_EXPECT_EQ(L.viewport.w, 960); TL_EXPECT_EQ(L.viewport.h, 540);
    }
    // Row 3: 300x100 / 320x180 LETTERBOX, window smaller than internal -> s=1 (never < 1), the
    // viewport clipped by the device (negative origin is legal - docs/RENDER2D.md §9.3.1).
    {
        Layout L = resolve_layout(300, 100, 0, make_pres(320, 180, PRES_INTEGER_LETTERBOX));
        TL_EXPECT_EQ(L.scale_x, 1.0f);
        TL_EXPECT_EQ(L.scale_y, 1.0f);
        TL_EXPECT_EQ(L.viewport.w, 320); TL_EXPECT_EQ(L.viewport.h, 180);
        TL_EXPECT_EQ(L.viewport.x, -10); TL_EXPECT_EQ(L.viewport.y, -40);
    }
    // Row 4: ASPECT_FIT 1000x1000 / 320x180 -> 1000x562 at (0,219).
    {
        Layout L = resolve_layout(1000, 1000, 0, make_pres(320, 180, PRES_ASPECT_FIT));
        TL_EXPECT_EQ(L.viewport.x, 0); TL_EXPECT_EQ(L.viewport.y, 219);
        TL_EXPECT_EQ(L.viewport.w, 1000); TL_EXPECT_EQ(L.viewport.h, 562);
    }
    // Row 5: dpi 2.0 - no internal target (pres.internal_w == 0), window res, dpi from logical_w.
    {
        Layout L = resolve_layout(2560, 1440, 1280, make_pres(0, 0, PRES_INTEGER_LETTERBOX));
        TL_EXPECT_EQ(L.dpi_scale, 2.0f);
        TL_EXPECT_EQ(L.internal_w, 2560u); TL_EXPECT_EQ(L.internal_h, 1440u);
        TL_EXPECT_EQ(L.scale_x, 1.0f); TL_EXPECT_EQ(L.scale_y, 1.0f);
        TL_EXPECT_EQ(L.viewport.x, 0); TL_EXPECT_EQ(L.viewport.y, 0);
        TL_EXPECT_EQ(L.viewport.w, 2560); TL_EXPECT_EQ(L.viewport.h, 1440);
    }
}

// A tiny deterministic LCG - render tests are not sim-determinism-sensitive (docs/RENDER2D.md
// caption: nothing here is hashed), so foundation/rng.h's fx-keyed generator is not the tool;
// this is local, fixed-seed, and reproducible across runs.
static u32 lcg_next(u32* state) { *state = *state * 1664525u + 1013904223u; return *state; }
static f32 lcg_f32(u32* state, f32 lo, f32 hi) {
    const f32 t = (f32)(lcg_next(state) >> 8) * (1.0f / 16777216.0f);   // [0,1)
    return lo + t * (hi - lo);
}

TL_TEST(world_screen_roundtrip, "render") {
    const f32 zooms[] = { 0.5f, 1.0f, 4.0f };
    const f32 rots[]  = { 0.0f, 0.125f, 0.5f };
    Layout L = resolve_layout(1280, 720, 0, make_pres(320, 180, PRES_INTEGER_LETTERBOX));
    u32 seed = 0xC0FFEEu;
    u32 tested = 0;

    for (u32 zi = 0; zi < 3; ++zi) {
        for (u32 ri = 0; ri < 3; ++ri) {
            Camera2D cam{};
            cam.cx = 0.0f; cam.cy = 0.0f; cam.zoom = zooms[zi]; cam.rot_turns = rots[ri];
            cam.ppu = 16.0f; cam.pixel_snap = 0;
            Mat3 M = view_matrix(cam, L);

            for (u32 i = 0; i < 1111; ++i) {   // ~10k total across the 9 (zoom, rot) pairs
                const f32 wx = lcg_f32(&seed, -100.0f, 100.0f);
                const f32 wy = lcg_f32(&seed, -100.0f, 100.0f);
                f32 sx, sy, wx2, wy2;
                world_to_screen(M, wx, wy, &sx, &sy);
                screen_to_world(M, sx, sy, &wx2, &wy2);
                TL_EXPECT_TRUE(fabsf(wx2 - wx) < 1e-3f);
                TL_EXPECT_TRUE(fabsf(wy2 - wy) < 1e-3f);
                ++tested;
            }
        }
    }
    TL_EXPECT_GE(tested, 9990u);   // the "~10k" the spec names - counted, not assumed (docs/TESTING.md §7)

    // The Y flip: world +Y maps to decreasing screen Y (docs/RENDER2D.md §0).
    {
        Camera2D cam{}; cam.cx = 0.0f; cam.cy = 0.0f; cam.zoom = 1.0f; cam.rot_turns = 0.0f;
        cam.ppu = 16.0f; cam.pixel_snap = 0;
        Mat3 M = view_matrix(cam, L);
        f32 sx0, sy0, sx1, sy1;
        world_to_screen(M, 0.0f, 0.0f, &sx0, &sy0);
        world_to_screen(M, 0.0f, 1.0f, &sx1, &sy1);
        TL_EXPECT_TRUE(sy1 < sy0);
        TL_EXPECT_EQ(sx1, sx0);
    }

    // Picking through target_to_window's inverse (docs/RENDER2D.md §9.3.2): window px -> target
    // px (manual inverse of target_to_window's affine map) -> world px, round-trips through
    // world_to_screen/target_to_window.
    {
        Camera2D cam{}; cam.cx = 5.0f; cam.cy = -3.0f; cam.zoom = 2.0f; cam.rot_turns = 0.125f;
        cam.ppu = 16.0f; cam.pixel_snap = 0;
        Mat3 M = view_matrix(cam, L);
        const f32 wx = 12.0f, wy = -4.0f;
        f32 tx, ty, winx, winy;
        world_to_screen(M, wx, wy, &tx, &ty);
        target_to_window(L, tx, ty, &winx, &winy);
        // inverse of target_to_window: tx = (winx - viewport.x) / scale_x
        const f32 tx2 = (winx - (f32)L.viewport.x) / L.scale_x;
        const f32 ty2 = (winy - (f32)L.viewport.y) / L.scale_y;
        f32 wx2, wy2;
        screen_to_world(M, tx2, ty2, &wx2, &wy2);
        TL_EXPECT_TRUE(fabsf(wx2 - wx) < 1e-3f);
        TL_EXPECT_TRUE(fabsf(wy2 - wy) < 1e-3f);
    }
}

// Review round 1 D10 (verified discriminating in round 2): resolve_layout's internal_h == 0 guard
// is TL_CHECK (live in every tier), not TL_ASSERT - internal_w != 0 but internal_h == 0 would
// otherwise fall through to an integer divide-by-zero at `win_h / ih`.
TL_TEST_EXPECT_FATAL(resolve_layout_zero_internal_h_fatal, "render,fatal") {
    (void)t;
    resolve_layout(1280, 720, 0, make_pres(320, 0, PRES_INTEGER_LETTERBOX));   // ih == 0 -> TL_FATAL
}

// Review round 1 D10 (verified discriminating in round 2): view_matrix's pixel_snap branch divides
// by ppu = cam.ppu * cam.zoom to round the snapped centre - a zero ppu (here cam.ppu == 0) would
// otherwise divide by zero. Only reachable when pixel_snap != 0 - the non-snap path never divides.
TL_TEST_EXPECT_FATAL(view_matrix_zero_ppu_pixel_snap_fatal, "render,fatal") {
    (void)t;
    Layout L = resolve_layout(1280, 720, 0, make_pres(320, 180, PRES_INTEGER_LETTERBOX));
    Camera2D cam{}; cam.cx = 0.0f; cam.cy = 0.0f; cam.zoom = 1.0f; cam.rot_turns = 0.0f;
    cam.ppu = 0.0f; cam.pixel_snap = 1;
    view_matrix(cam, L);   // ppu == 0 -> TL_FATAL
}

// Review round 1 D10 (verified discriminating in round 2): screen_to_world's det == 0 guard is the
// second line of defense (view_matrix's own ppu != 0 guard is the first) for a matrix built some
// other way - a degenerate Mat3 (m0=m1=m3=m4=0, det = 0*0 - 0*0 = 0) exercises it directly without
// going through view_matrix at all.
TL_TEST_EXPECT_FATAL(screen_to_world_zero_det_fatal, "render,fatal") {
    (void)t;
    Mat3 M{};   // all-zero: det = m0*m4 - m1*m3 = 0
    f32 wx, wy;
    screen_to_world(M, 1.0f, 1.0f, &wx, &wy);   // det == 0 -> TL_FATAL
}
