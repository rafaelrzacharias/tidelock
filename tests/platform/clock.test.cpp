// clock.test.cpp - docs/PLATFORM.md §9.6 clock_monotonic.
#include "runner/tl_test.h"
#include "platform_test_util.h"

TL_TEST(clock_monotonic, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const ClockApi& c = api->clock;

    TL_EXPECT_GE(c.frequency(c.ctx), 1000000ull);

    u64 prev = c.ticks(c.ctx);
    bool ever_decreased = false;
    for (u32 i = 0; i < 10000u; ++i) {
        const u64 now = c.ticks(c.ctx);
        if (now < prev) { ever_decreased = true; break; }
        prev = now;
    }
    TL_EXPECT_FALSE(ever_decreased);

    platform_test_shutdown(api);
}
