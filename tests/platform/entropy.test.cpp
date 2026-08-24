// entropy.test.cpp - docs/PLATFORM.md §9.6 entropy_nonrepeat.
#include "runner/tl_test.h"
#include "platform_test_util.h"
#include "platform/entropy.h"

#include <string.h>
#include <math.h>

TL_TEST(entropy_nonrepeat, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    TL_ASSERT_NOT_NULL(api->entropy);

    enum { N = 1000, LEN = 32 };
    u8 fills[N][LEN];   // 32 KB - fine on the default thread stack
    for (u32 i = 0; i < N; ++i) {
        api->entropy->fill(api->entropy->ctx, fills[i], LEN);
    }

    // no two equal
    bool any_dup = false;
    for (u32 i = 0; i < N && !any_dup; ++i) {
        for (u32 j = i + 1u; j < N; ++j) {
            if (memcmp(fills[i], fills[j], LEN) == 0) { any_dup = true; break; }
        }
    }
    TL_EXPECT_FALSE(any_dup);

    // no all-zero fill
    bool any_all_zero = false;
    for (u32 i = 0; i < N; ++i) {
        bool all_zero = true;
        for (u32 b = 0; b < LEN; ++b) { if (fills[i][b] != 0u) { all_zero = false; break; } }
        if (all_zero) { any_all_zero = true; break; }
    }
    TL_EXPECT_FALSE(any_all_zero);

    // byte histogram within 4 sigma of uniform: N*LEN draws over 256 buckets, mean = N*LEN/256,
    // variance = mean * (255/256) (binomial per bucket)
    u32 hist[256] = {};
    for (u32 i = 0; i < N; ++i) { for (u32 b = 0; b < LEN; ++b) { ++hist[fills[i][b]]; } }
    const double total = (double)N * (double)LEN;
    const double mean = total / 256.0;
    const double variance = mean * (255.0 / 256.0);
    const double sigma4 = 4.0 * (variance > 0.0 ? sqrt(variance) : 1.0);
    bool within = true;
    for (u32 b = 0; b < 256; ++b) {
        const double d = (double)hist[b] - mean;
        if (d < -sigma4 || d > sigma4) { within = false; break; }
    }
    TL_EXPECT_TRUE(within);

    platform_test_shutdown(api);
}
