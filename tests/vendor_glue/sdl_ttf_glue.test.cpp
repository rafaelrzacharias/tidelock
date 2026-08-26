// SDL_ttf has no adaptor .cpp of its own for its 4 SDL_malloc/6 SDL_free call sites - it inherits
// whatever allocator sdl3_glue installed on SDL3 (docs/PLATFORM.md §9.5). The font memory itself
// is FreeType's, not SDL_ttf's: FreeType has no runtime allocator-registration API SDL_ttf's
// TTF_Init() exposes, so that hookup lives at FreeType's own platform-customization seam instead
// (vendor/sdl_ttf/external/freetype/builds/<platform>/ftsystem.c, patched to call
// src/vendor_glue/freetype_glue.cpp's three hooks - vendor/VERSIONS' declared freetype deviation).
//
// vendor_glue_sdl3_install() is deliberately NOT called here (review round 2, D2). Review round 3
// (N3) found that dropping it is not enough on its own: SDL_SetMemoryFunctions is process-wide
// and permanent, so once ANY earlier row in the same process (e.g. an sdl3_glue_* row, when
// tl_tests runs without --isolate - docs/BUILD.md §10.4, exercised by pr.yml's sanitize-linux
// job) has installed it, SDL_ttf's own SDL_malloc sites move pool_vendor's live_bytes too, and a
// live_bytes-only assertion is satisfied by that alone, with FreeType still on libc. A raw
// pool_vendor delta cannot tell the two contributors apart in a shared process.
//
// Fix: tl_freetype_call_count() (freetype_glue.h) counts calls into
// tl_freetype_alloc/realloc/free specifically, so it is attributable to FreeType's seam alone in
// ANY invocation mode, isolated or not - SDL3's allocator state cannot move it. This is the
// row's real witness; the pool_vendor delta below is kept as a secondary check that the
// allocation actually landed in the shared pool, not just that the hook fired.
#include "runner/tl_test.h"
#include "vendor_glue/freetype_glue.h"
#include "pool_vendor_test_api.h"

#include <SDL3_ttf/SDL_ttf.h>

TL_TEST(sdl_ttf_init_quit_through_pool_vendor, "vendor_glue,sdl_ttf,smoke") {
    vendor_glue_test_ensure_pool_vendor(t);

    u64 baseline = pool_stats(pool_vendor())->live_bytes;
    u64 calls_before = tl_freetype_call_count();
    TL_ASSERT_TRUE(TTF_Init());
    // Attributable to FreeType's seam alone, independent of SDL3's process-wide hook state
    // (docs/TESTING.md §7 "measure, don't assert"; review round 3, N3).
    TL_EXPECT_TRUE(tl_freetype_call_count() > calls_before);
    TL_EXPECT_TRUE(pool_stats(pool_vendor())->live_bytes > baseline);
    TTF_Quit();
    TL_EXPECT_EQ(pool_stats(pool_vendor())->live_bytes, baseline);
}
