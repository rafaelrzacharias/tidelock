// stb_glue.h. The pool_vendor hookup stb_image's STBI_MALLOC/REALLOC/FREE macros call
// (docs/MEMORY.md §8.6); the macro-driven IMPLEMENTATION body itself lives in
// vendor/stb/stb_impl.c (vendor code, vendor's own flags - see that file's header comment for
// why it cannot live here). stb_image.h/stb_sprintf.h are included here in DECLARATION-ONLY mode
// (no *_IMPLEMENTATION macro defined), which is just prototypes and compiles clean under
// tl_flags_common.
#include "vendor_glue/stb_glue.h"
#include "vendor_glue/pool_vendor.h"

#include <stb_image.h>
#include <stb_sprintf.h>
#include <stdarg.h>

extern "C" {

void* tl_stbi_malloc(size_t size) {
    return pool_alloc(pool_vendor(), (u64)size);
}

void* tl_stbi_realloc(void* p, size_t new_size) {
    return pool_realloc(pool_vendor(), p, (u64)new_size);
}

void tl_stbi_free(void* p) {
    pool_free(pool_vendor(), p);
}

}  // extern "C"

u8* vendor_glue_stbi_load_from_memory(const u8* buffer, i32 len, i32* out_w, i32* out_h) {
    int channels_in_file = 0;
    return stbi_load_from_memory(buffer, len, out_w, out_h, &channels_in_file, 4);
}

void vendor_glue_stbi_free(void* image_data) {
    stbi_image_free(image_data);
}

i32 vendor_glue_stbsp_snprintf(char* buf, i32 buf_size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    i32 r = stbsp_vsnprintf(buf, buf_size, fmt, args);
    va_end(args);
    return r;
}
