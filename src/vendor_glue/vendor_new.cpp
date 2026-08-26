// vendor_new.cpp - the pool-backed global operator new/delete. Contract: vendor_glue/vendor_new.h.
// Spec: docs/MEMORY.md §1.5/§2 (RR-18, ruled 2026-08-26), docs/PLATFORM.md §9.5.
//
// This TU holds the ONE piece of namespace-scope mutable state the tree allows outside the
// tooling plane, and it is exempted by name in tools/audit/static_allow.txt. The reason is not
// convenience: `operator new` is a replaceable function with a fixed signature and no context
// parameter, so a pool it should draw from cannot be passed to it. Every alternative was
// considered in RR-18 and recorded in TODO.md.
#include "vendor_glue/vendor_new.h"

#include "foundation/tl_assert.h"

#include <stddef.h>   // size_t - operator new's parameter type is not ours to choose

// The exempted pointer. Deliberately the only one in the file, and the file is deliberately the
// only thing in its archive member.
static MemPool* g_vendor_heap = nullptr;

void vendor_heap_install(MemPool* pool) {
    if (pool != nullptr && g_vendor_heap != nullptr && g_vendor_heap != pool) {
        TL_FATAL("vendor_heap_install: a pool is already installed - two owners of one "
                 "program-wide allocator hook (docs/MEMORY.md section 1.5)");
    }
    g_vendor_heap = pool;
}

MemPool* vendor_heap_current(void) { return g_vendor_heap; }

namespace {

// Every allocation carries a 16-byte header holding its size, because the pool needs no size to
// free but `operator delete` is handed a pointer alone and the header keeps the two symmetric
// without a second lookup. 16 rather than 8 so the returned pointer keeps mem_pool's 16-byte
// alignment guarantee (docs/MEMORY.md section 8.6).
struct VendorBlockHeader {
    u64 size;
    u64 _pad;
};

void* vendor_alloc(size_t n) {
    MemPool* p = g_vendor_heap;
    if (p == nullptr) {
        // Exactly the foundation tripwire's contract: with no pool installed, a global new from
        // anywhere in this program is a bug, and it dies here rather than reaching the CRT.
        TL_FATAL("global operator new with no vendor heap installed - use an arena or a pool "
                 "(docs/MEMORY.md section 2)");
    }
    const u64 total = (u64)n + (u64)sizeof(VendorBlockHeader);
    void* raw = pool_alloc(p, total);
    if (raw == nullptr) {
        // The vendored library asked for more than its budget. Returning null from operator new
        // is a std::bad_alloc contract we cannot honour with exceptions off, and a vendored
        // library that gets null from `new` will dereference it - so the budget is the loud
        // failure, here, with the size that broke it findable in the crash report.
        TL_FATAL("vendor heap budget exhausted in operator new (docs/MEMORY.md section 8.6)");
    }
    VendorBlockHeader* h = (VendorBlockHeader*)raw;
    h->size = (u64)n;
    h->_pad = 0;
    return (void*)((u8*)raw + sizeof(VendorBlockHeader));
}

void vendor_free(void* q) {
    if (q == nullptr) return;
    MemPool* p = g_vendor_heap;
    if (p == nullptr) {
        // A block freed after its pool was uninstalled outlived the window it was allocated in.
        // That is the same bug class the tripwire exists for and it dies the same way, rather
        // than leaking silently into a pool nobody owns any more.
        TL_FATAL("global operator delete with no vendor heap installed - a vendored allocation "
                 "outlived its install window (docs/MEMORY.md section 1.5)");
    }
    pool_free(p, (void*)((u8*)q - sizeof(VendorBlockHeader)));
}

}  // namespace

// The replacements, in every tier. They REPLACE foundation's tripwires rather than weakening
// them: with no pool installed both paths above TL_FATAL with the same message shape, so a
// stray `new` from src/ code dies exactly as before. What changed is only that a bounded WINDOW
// exists in which a vendored library may allocate - src/script opens it around luau_compile and
// closes it immediately after, so the window is a few milliseconds per compile and nothing else
// in the process is inside it.
//
// Link mechanics: tl_vendor_glue depends on tl_foundation, so CMake puts it EARLIER on the link
// line; this member is pulled for `operator new` before libtl_foundation.a is scanned, and
// alloc_shim_ops.o is then never needed. That ordering is a dependency, not luck - which is the
// whole reason the tripwires were split into their own member (RR-18).
void* operator new(size_t n) { return vendor_alloc(n); }
void* operator new[](size_t n) { return vendor_alloc(n); }
void operator delete(void* q) noexcept { vendor_free(q); }
void operator delete[](void* q) noexcept { vendor_free(q); }
void operator delete(void* q, size_t) noexcept { vendor_free(q); }
void operator delete[](void* q, size_t) noexcept { vendor_free(q); }
