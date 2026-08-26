// pool_vendor.h - reserve/commit + the shared instance. Spec: docs/PLATFORM.md §9.5.
#include "vendor_glue/pool_vendor.h"
#include "foundation/tl_assert.h"

namespace {
// docs/PLATFORM.md §9.5: vendor_glue is "the one folder allowed a static pool pointer" - every
// TU here is a per-lib allocator hookup, so a whole-lib exemption (tools/audit/includes.py,
// tools/audit/symbols.py --vendor-glue-lib) covers this instead of RR-7's stem list.
MemPool g_pool_vendor;
bool g_pool_vendor_ready = false;
}  // namespace

ErrCode pool_vendor_init(const VMemApi* os) {
    TL_ASSERT(!g_pool_vendor_ready);
    enum : u64 { POOL_VENDOR_BYTES = 64ull << 20 };  // docs/PLATFORM.md §9.5
    ErrCode e = pool_init(&g_pool_vendor, 0xA001u, POOL_VENDOR_BYTES, POOL_VENDOR_BYTES, os);
    if (e == ERR_OK) g_pool_vendor_ready = true;
    return e;
}

MemPool* pool_vendor(void) {
    TL_ASSERT(g_pool_vendor_ready);
    return &g_pool_vendor;
}
