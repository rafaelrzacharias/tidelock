// pool_enet.h - reserve/commit + the instance. Spec: docs/PLATFORM.md §9.5.
#include "vendor_glue/pool_enet.h"
#include "foundation/tl_assert.h"

namespace {
// docs/PLATFORM.md §9.5: vendor_glue is "the one folder allowed a static pool pointer" - the
// same whole-lib exemption sdl3_glue's pool_vendor uses (tools/audit/includes.py,
// tools/audit/symbols.py --vendor-glue-lib).
MemPool g_pool_enet;
bool g_pool_enet_ready = false;
}  // namespace

ErrCode pool_enet_init(const VMemApi* os) {
    TL_ASSERT(!g_pool_enet_ready);
    enum : u64 { POOL_ENET_BYTES = 16ull << 20 };  // docs/PLATFORM.md §9.5
    ErrCode e = pool_init(&g_pool_enet, 0xA002u, POOL_ENET_BYTES, POOL_ENET_BYTES, os);
    if (e == ERR_OK) g_pool_enet_ready = true;
    return e;
}

MemPool* pool_enet(void) {
    TL_ASSERT(g_pool_enet_ready);
    return &g_pool_enet;
}
