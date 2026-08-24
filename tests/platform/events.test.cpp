// events.test.cpp - docs/PLATFORM.md §9.6 event_ring_overflow.
#include "runner/tl_test.h"
#include "platform_test_util.h"
#include "platform/impl_headless/headless_test_api.h"

TL_TEST(event_ring_overflow, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    TL_EXPECT_EQ(api->events.dropped_total(api->events.ctx), 0u);

    RingBuffer<RawEvent>* ring = headless_event_ring(api);
    TL_ASSERT_NOT_NULL(ring);
    const u32 cap = ring->cap;
    TL_ASSERT_EQ(cap, 1024u);   // default event_ring_cap_log2 == 10

    for (u32 i = 0; i < cap + 37u; ++i) {
        RawEvent ev{};
        ev.timestamp_ticks = i;
        ev.kind = EV_KEY;
        ev.u.key.scancode = i;
        TL_ASSERT_TRUE(ring_push(ring, ev));   // overwrite_oldest: never refused
    }

    TL_EXPECT_EQ(ring_count(ring), cap);
    TL_EXPECT_EQ(api->events.dropped_total(api->events.ctx), 37u);
    // oldest surviving event is #37 (0-indexed: events 0..36 were evicted)
    TL_EXPECT_EQ(ring_peek(ring, 0u).timestamp_ticks, 37u);

    platform_test_shutdown(api);
}
