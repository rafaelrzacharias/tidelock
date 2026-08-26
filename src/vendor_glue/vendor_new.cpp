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
// only thing in its archive member. The two window counters below share its exemption row (the
// allowlist is keyed by lib + directory + STEM, so it is this file that is named, not this
// variable) and share its justification: `operator new` takes no context, so a figure derived
// from what it does has nowhere else to live.
static MemPool* g_vendor_heap = nullptr;
static u64 g_window_live = 0;      // bytes live inside the current window
static u64 g_window_peak = 0;      // max(g_window_live) since the last install

void vendor_heap_install(MemPool* pool) {
    if (pool != nullptr && g_vendor_heap != nullptr && g_vendor_heap != pool) {
        TL_FATAL("vendor_heap_install: a pool is already installed - two owners of one "
                 "program-wide allocator hook (docs/MEMORY.md section 1.5)");
    }
    g_vendor_heap = pool;
    // Every install starts a new window. Reset on install rather than on uninstall so the figure
    // survives for the caller to read after the window has closed - which is when it is wanted.
    if (pool != nullptr) {
        g_window_live = 0;
        g_window_peak = 0;
    }
}

u64 vendor_heap_window_peak(void) { return g_window_peak; }

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
    g_window_live += total;
    if (g_window_live > g_window_peak) g_window_peak = g_window_live;
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
    VendorBlockHeader* h = (VendorBlockHeader*)((u8*)q - sizeof(VendorBlockHeader));
    const u64 total = h->size + (u64)sizeof(VendorBlockHeader);
    g_window_live = g_window_live > total ? g_window_live - total : 0;
    pool_free(p, (void*)h);
}

}  // namespace

// The replacements, in every tier. They REPLACE foundation's tripwires rather than weakening
// them: with no pool installed both paths above TL_FATAL with the same message shape, so a
// stray `new` from src/ code dies exactly as before. What changed is only that a bounded WINDOW
// exists in which a vendored library may allocate - src/script opens it around luau_compile and
// closes it immediately after, so the window is a few milliseconds per compile and nothing else
// in the process is inside it.
//
// Link mechanics, MEASURED on the generated link lines rather than reasoned about (an earlier
// version of this comment claimed CMake orders tl_vendor_glue before tl_foundation because of the
// dependency - it does not; tl_tests lists libtl_foundation.a FIRST):
//   - src/script/vm.cpp references vendor_heap_install, so THIS member is pulled while
//     libtl_vendor_glue.a is scanned, and every reference to `operator new` from
//     libluau_compiler.a is satisfied from here.
//   - alloc_shim_ops.o is pulled only if `operator new` is still undefined when a
//     libtl_foundation.a occurrence is scanned. In every current link line that never happens:
//     nothing before foundation's first occurrence allocates, and by its last occurrence this
//     member has already defined the operators.
// If that ever stops holding, the failure is a DUPLICATE-SYMBOL LINK ERROR - loud, at build
// time, on every leg - not a silent fallback. The property that could regress quietly is the
// other one (the CRT serving the compiler instead of the pool), and that is pinned at runtime by
// tests/script/vm_lifecycle.test.cpp's compile_allocations_go_through_the_vm_pool, which watches
// the pool's peak move across a compile.
void* operator new(size_t n) { return vendor_alloc(n); }
void* operator new[](size_t n) { return vendor_alloc(n); }
void operator delete(void* q) noexcept { vendor_free(q); }
void operator delete[](void* q) noexcept { vendor_free(q); }
void operator delete(void* q, size_t) noexcept { vendor_free(q); }
void operator delete[](void* q, size_t) noexcept { vendor_free(q); }
