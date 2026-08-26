// tl_luau_alloc - the lua_Alloc adaptor. Contract: vendor_glue/luau_alloc.h.
//
// This TU deliberately does NOT include a Luau header: the signature is pinned by hand so that
// tools/audit/includes.py's BACKEND_HEADERS rule ("luau"/"lua.h" only under src/script) stays a
// single-module claim. The compatibility check that would otherwise need <lua.h> lives in
// src/script/vm.cpp, where a static_assert compares this function against lua_Alloc.
#include "vendor_glue/luau_alloc.h"

extern "C" void* tl_luau_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    (void)osize;                                   // recovered from the page header, §8.6
    MemPool* pool = (MemPool*)ud;
    TL_ASSERT(pool != nullptr);
    if (nsize == 0) {
        pool_free(pool, ptr);
        return nullptr;
    }
    if (ptr == nullptr) {
        return pool_alloc(pool, (u64)nsize);
    }
    return pool_realloc(pool, ptr, (u64)nsize);
}
