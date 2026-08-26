// sdl3_glue.h - SDL3's four allocator hooks over pool_vendor. Spec: docs/MEMORY.md §8.6.
#include "vendor_glue/sdl3_glue.h"
#include "vendor_glue/pool_vendor.h"

#include <SDL3/SDL.h>
#include <string.h>

namespace {

void* SDLCALL tl_sdl_malloc(size_t size) {
    return pool_alloc(pool_vendor(), (u64)size);
}

void* SDLCALL tl_sdl_calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb) return nullptr;  // overflow guard, like libc calloc
    u64 bytes = (u64)nmemb * (u64)size;
    void* p = pool_alloc(pool_vendor(), bytes);
    if (p) memset(p, 0, (size_t)bytes);  // pool_alloc's reused blocks are not zeroed (docs/MEMORY.md §1.1)
    return p;
}

void* SDLCALL tl_sdl_realloc(void* mem, size_t size) {
    return pool_realloc(pool_vendor(), mem, (u64)size);
}

void SDLCALL tl_sdl_free(void* mem) {
    pool_free(pool_vendor(), mem);
}

}  // namespace

void vendor_glue_sdl3_install(void) {
    SDL_SetMemoryFunctions(tl_sdl_malloc, tl_sdl_calloc, tl_sdl_realloc, tl_sdl_free);
}
