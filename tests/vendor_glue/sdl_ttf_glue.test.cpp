// SDL_ttf has no adaptor .cpp of its own for its 4 SDL_malloc/6 SDL_free call sites - it inherits
// whatever allocator sdl3_glue installed on SDL3 (docs/PLATFORM.md §9.5). The font memory itself
// is FreeType's, not SDL_ttf's: FreeType has no runtime allocator-registration API SDL_ttf's
// TTF_Init() exposes, so that hookup lives at FreeType's own platform-customization seam instead
// (vendor/sdl_ttf/external/freetype/builds/<platform>/ftsystem.c, patched to call
// src/vendor_glue/freetype_glue.cpp's three hooks - docs/BUILD.md §4's declared verbatim
// deviation). This proves both paths land in pool_vendor's own accounting, not merely succeed.
// Spec: docs/PLATFORM.md §9.5. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "vendor_glue/sdl3_glue.h"
#include "pool_vendor_test_api.h"

#include <SDL3_ttf/SDL_ttf.h>

TL_TEST(sdl_ttf_init_quit_through_pool_vendor, "vendor_glue,sdl_ttf,smoke") {
    vendor_glue_test_ensure_pool_vendor(t);
    vendor_glue_sdl3_install();

    u64 baseline = pool_stats(pool_vendor())->live_bytes;
    TL_ASSERT_TRUE(TTF_Init());
    // FreeType's own library object allocates through pool_vendor now (freetype_glue.cpp) - a
    // default (non-hooked) FreeType would satisfy TTF_Init()'s bool return identically
    // (docs/TESTING.md §7 "measure, don't assert").
    TL_EXPECT_TRUE(pool_stats(pool_vendor())->live_bytes > baseline);
    TTF_Quit();
    TL_EXPECT_EQ(pool_stats(pool_vendor())->live_bytes, baseline);
}
