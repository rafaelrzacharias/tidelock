// SDL_ttf has no adaptor .cpp: docs/PLATFORM.md §9.5 - it calls SDL_malloc/SDL_free internally,
// so it inherits whatever allocator sdl3_glue installed on SDL3. This proves the archive links
// and that TTF_Init's FreeType-backed allocations actually round-trip through pool_vendor.
// Spec: docs/PLATFORM.md §9.5. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "vendor_glue/sdl3_glue.h"
#include "pool_vendor_test_api.h"

#include <SDL3_ttf/SDL_ttf.h>

TL_TEST(sdl_ttf_init_quit_through_pool_vendor, "vendor_glue,sdl_ttf,smoke") {
    vendor_glue_test_ensure_pool_vendor(t);
    vendor_glue_sdl3_install();

    TL_ASSERT_TRUE(TTF_Init());
    TTF_Quit();
}
