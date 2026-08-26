#pragma once
// pool_vendor_test_api.h - idempotent pool_vendor_init for tests/vendor_glue/*.test.cpp.
// pool_vendor_init (docs/PLATFORM.md §9.5) may run exactly once per process; pool_vendor is
// shared across every glue adaptor's tests (sdl3 now, imgui/stb/SDL_ttf later in the same lane),
// so this makes "ensure it is ready" safe to call from each of them regardless of run order -
// required for tl_tests' non-isolated run (docs/BUILD.md §10.4).
#include "vendor_glue/pool_vendor.h"
#include "foundation/vmem_test_api.h"
#include "runner/tl_test.h"

// Initialises pool_vendor on the first call in this process; a no-op on every later call.
inline void vendor_glue_test_ensure_pool_vendor(TestCtx* t) {
    static bool ready = false;
    if (ready) return;
    static VMemApi api;
    api = test_vmem_api();
    TL_ASSERT_EQ(pool_vendor_init(&api), ERR_OK);
    ready = true;
}
