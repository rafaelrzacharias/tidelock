/* stb_image.h/stb_sprintf.h have no .c of their own - the IMPLEMENTATION macro pattern needs
 * exactly one TU to define it. That TU is vendor code (docs/BUILD.md §4: vendored libs are
 * exempt from the C++ subset and compile with their own, unrestricted flags) - the actual
 * bodies of these headers were never written to satisfy -Wsign-conversion, so instantiating them
 * inside a strict-flags src/ TU (as a first attempt did) fails to compile. tl_stbi_malloc/
 * realloc/free are implemented in src/vendor_glue/stb_glue.cpp (docs/MEMORY.md §8.6: the pool
 * hookup lives in vendor_glue, never in vendor/); declared here with C linkage since this file
 * compiles as C, matching stb_image.h/stb_sprintf.h's own C-compatible headers.
 */
#include <stddef.h>

extern void* tl_stbi_malloc(size_t size);
extern void* tl_stbi_realloc(void* p, size_t new_size);
extern void tl_stbi_free(void* p);

#define STBI_MALLOC(sz)        tl_stbi_malloc(sz)
#define STBI_REALLOC(p, newsz) tl_stbi_realloc(p, newsz)
#define STBI_FREE(p)           tl_stbi_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_SPRINTF_IMPLEMENTATION
#include "stb_sprintf.h"
