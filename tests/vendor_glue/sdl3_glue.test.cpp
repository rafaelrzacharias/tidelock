// sdl3_glue.h - proves SDL3's allocator hooks actually round-trip through pool_vendor.
// Spec: docs/MEMORY.md §8.6, docs/PLATFORM.md §9.5. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "vendor_glue/sdl3_glue.h"
#include "pool_vendor_test_api.h"

#include <SDL3/SDL.h>
#include <string.h>

TL_TEST(sdl3_glue_malloc_free_roundtrip, "vendor_glue,sdl3,smoke") {
    vendor_glue_test_ensure_pool_vendor(t);
    vendor_glue_sdl3_install();

    void* p = SDL_malloc(128);
    TL_ASSERT_TRUE(p != nullptr);
    memset(p, 0xAB, 128);   // the pool must hand back real, writable memory
    SDL_free(p);
}

TL_TEST(sdl3_glue_calloc_zeroes, "vendor_glue,sdl3,smoke") {
    vendor_glue_test_ensure_pool_vendor(t);
    vendor_glue_sdl3_install();

    unsigned char* p = (unsigned char*)SDL_calloc(16, 8);   // 128 bytes, reused-class-shaped
    TL_ASSERT_TRUE(p != nullptr);
    bool all_zero = true;
    for (u32 i = 0; i < 128; ++i) all_zero = all_zero && (p[i] == 0);
    TL_EXPECT_TRUE(all_zero);
    SDL_free(p);
}

TL_TEST(sdl3_glue_realloc_preserves_content, "vendor_glue,sdl3,smoke") {
    vendor_glue_test_ensure_pool_vendor(t);
    vendor_glue_sdl3_install();

    unsigned char* p = (unsigned char*)SDL_malloc(32);
    TL_ASSERT_TRUE(p != nullptr);
    for (u32 i = 0; i < 32; ++i) p[i] = (unsigned char)i;

    unsigned char* q = (unsigned char*)SDL_realloc(p, 256);   // crosses a size class
    TL_ASSERT_TRUE(q != nullptr);
    bool preserved = true;
    for (u32 i = 0; i < 32; ++i) preserved = preserved && (q[i] == (unsigned char)i);
    TL_EXPECT_TRUE(preserved);
    SDL_free(q);
}
