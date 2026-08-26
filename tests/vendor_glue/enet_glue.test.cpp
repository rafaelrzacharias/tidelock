// enet_glue.h - proves ENet's allocator hooks round-trip through pool_enet.
// Spec: docs/MEMORY.md §8.6, docs/PLATFORM.md §9.5. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "vendor_glue/enet_glue.h"
#include "vendor_glue/pool_enet.h"
#include "foundation/vmem_test_api.h"

#include <enet/enet.h>

namespace {
void ensure_pool_enet(TestCtx* t) {
    static bool ready = false;
    if (ready) return;
    static VMemApi api;
    api = test_vmem_api();
    TL_ASSERT_EQ(pool_enet_init(&api), ERR_OK);
    ready = true;
}
}  // namespace

TL_TEST(enet_glue_host_create_destroy_through_pool_enet, "vendor_glue,enet,smoke") {
    ensure_pool_enet(t);
    TL_ASSERT_TRUE(vendor_glue_enet_install());

    u64 baseline = pool_stats(pool_enet())->live_bytes;
    // A client host (no bind address): still allocates its peer table and channel/queue buffers
    // through pool_enet, with no socket traffic - safe and deterministic in a headless test.
    ENetHost* host = enet_host_create(nullptr, 1, 2, 0, 0);
    TL_ASSERT_TRUE(host != nullptr);
    // The host struct and its peer/channel arrays must show up in pool_enet's own accounting,
    // not merely succeed - a default (non-hooked) allocator would pass every assertion above
    // this line identically (docs/TESTING.md §7 "measure, don't assert").
    TL_EXPECT_TRUE(pool_stats(pool_enet())->live_bytes > baseline);
    enet_host_destroy(host);
    TL_EXPECT_EQ(pool_stats(pool_enet())->live_bytes, baseline);
    enet_deinitialize();
}
