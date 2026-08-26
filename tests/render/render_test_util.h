#pragma once
// render_test_util.h - a fresh headless-platform-backed RenderQueue per test (docs/TESTING.md §7
// rubric #1). `World` is ~100s of KB (docs/LESSONS.md: "a World-sized fixture on the stack is a
// Windows-only crash") - RenderTestFixture must live in a caller-owned static, never be returned
// by value or held on the stack.
#include "render/render.h"
#include "platform/platform_test_util.h"
#include "foundation/vmem_arena.h"
#include "foundation/scratch.h"
#include "foundation/hash.h"

struct RenderTestFixture {
    const PlatformApi* platform;
    VMemArena arena;
    Scratch scratch;      // w->scratch (render_build_frame's sort_u64_kv needs it - not just .render)
    World world;
};

// Initializes `f` in place (a caller-owned static - see the file comment) and returns the first
// failing call's ErrCode (ERR_OK on success) - the jobs_test_util.h/world_test_util.h idiom, not
// TL_ASSERT: a fixture helper is called from a TL_TEST body where TL_ASSERT_EQ can check the
// result live in every tier, whereas TL_ASSERT alone compiles to `((void)0)` at TL_DEV=0 - and
// wrapping the call expression directly (no local) makes the preprocessor drop the call itself,
// not just the check (docs/LESSONS.md - the class this note itself extends). internal_w/h == 0
// means "no internal target" (render at window res, the common case for queue/batch tests that
// never touch the WORLD layer's texture).
inline ErrCode render_test_init(RenderTestFixture* f, u16 internal_w, u16 internal_h) {
    f->platform = platform_test_init();
    ErrCode e = vmem_arena_init(&f->arena, "test_render_arena"_id, 16u * 1024u * 1024u, 0, &f->platform->vmem);
    if (e != ERR_OK) { return e; }
    e = scratch_init(&f->scratch, "test_render_scratch"_id, 4u * 1024u * 1024u, &f->platform->vmem);
    if (e != ERR_OK) { return e; }
    Presentation pres{};
    pres.internal_w = internal_w; pres.internal_h = internal_h;
    pres.mode = PRES_INTEGER_LETTERBOX; pres.filter = FILTER_NEAREST; pres.pixel_snap = 0; pres._pad0 = 0;
    f->world = World{};
    f->world.scratch = &f->scratch;
    return render_init(&f->world, f->platform, &f->arena, &pres, 256);
}

inline void render_test_shutdown(RenderTestFixture* f) {
    render_shutdown(&f->world);
    platform_test_shutdown(f->platform);
}
