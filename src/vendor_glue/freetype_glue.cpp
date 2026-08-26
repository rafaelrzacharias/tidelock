// freetype_glue.cpp - defines the three hooks vendor/sdl_ttf/external/freetype's
// builds/<platform>/ftsystem.c calls instead of malloc/realloc/free (Unix) or
// HeapAlloc/HeapReAlloc/HeapFree (Windows). Spec: docs/MEMORY.md §8.6, docs/PLATFORM.md §9.5.
//
// FreeType has no runtime allocator-registration API SDL_ttf's TTF_Init() exposes (it calls
// FT_Init_FreeType() directly, which builds a memory manager with no injection point) - so the
// hookup happens at FreeType's OWN platform-customization seam instead, ftsystem.c, patched to
// call these three functions (docs/BUILD.md §4 "vendored verbatim" deviation, declared in
// vendor/VERSIONS' freetype row). No install() call is needed: the wiring is compile-time, not
// runtime, so the only precondition is that pool_vendor_init has already run before FreeType's
// first allocation (i.e. before the first TTF_Init()).
#include "vendor_glue/pool_vendor.h"

extern "C" {

void* tl_freetype_alloc(long size) {
    return pool_alloc(pool_vendor(), (u64)size);
}

void* tl_freetype_realloc(long /*cur_size*/, long new_size, void* block) {
    return pool_realloc(pool_vendor(), block, (u64)new_size);
}

void tl_freetype_free(void* block) {
    pool_free(pool_vendor(), block);
}

}  // extern "C"
