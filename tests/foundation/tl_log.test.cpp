// tl_log.test.cpp - docs/TOOLING.md §9.5 "log_levels_and_compile_out" (the ring/level half; the
// file sink waits for PlatformApi.file.append, TODO.md). tests/ is exempt from the C++ subset's
// io ban (docs/TESTING.md §8 R-2).
#include "runner/tl_test.h"
#include "foundation/tl_log.h"

#include <string.h>

// A level below the tier's TL_LOG_MIN must compile its call site to nothing - the argument is
// never evaluated, so a counter inside it stays at its initial value. The #if mirrors tl_log.h's
// own gates exactly, so whichever tier this test is built under exercises the matching branch
// (docs/LESSONS.md: a tier-conditional test needs the #if INSIDE the body, not around the TL_TEST).
TL_TEST(log_levels_compile_out, "foundation") {
    int trace_n = 0, debug_n = 0, info_n = 0, warn_n = 0;
    TL_LOG_TRACE("%d", ++trace_n);
    TL_LOG_DEBUG("%d", ++debug_n);
    TL_LOG_INFO("%d", ++info_n);
    TL_LOG_WARN("%d", ++warn_n);
#if TL_LOG_MIN <= 0
    TL_EXPECT_EQ(trace_n, 1);
#else
    TL_EXPECT_EQ(trace_n, 0);
#endif
#if TL_LOG_MIN <= 1
    TL_EXPECT_EQ(debug_n, 1);
#else
    TL_EXPECT_EQ(debug_n, 0);
#endif
#if TL_LOG_MIN <= 2
    TL_EXPECT_EQ(info_n, 1);
#else
    TL_EXPECT_EQ(info_n, 0);
#endif
#if TL_LOG_MIN <= 3
    TL_EXPECT_EQ(warn_n, 1);
#else
    TL_EXPECT_EQ(warn_n, 0);
#endif
    // TL_LOG_ERR is never compiled out, in any tier.
    int err_n = 0;
    TL_LOG_ERR("%d", ++err_n);
    TL_EXPECT_EQ(err_n, 1);
}

TL_TEST(log_ring_wraps_overwriting_oldest, "foundation") {
    tl_log_test_reset();
    for (u32 i = 0; i < 4097u; ++i) {
        TL_LOG_ERR("msg %u", i);   // always on - independent of the tier's TL_LOG_MIN
    }
    TL_ASSERT_EQ(tl_log_test_ring_count(), 4096u);
    // 4097 writes into a 4096-slot ring: message 0 was overwritten, 1..4096 survive, and the
    // ring's write cursor is back to where message 4097 (the 4098th) would land - slot 1.
    TL_EXPECT_EQ(tl_log_test_ring_head(), 1u);
    bool found_msg_0 = false, found_msg_4096 = false;
    for (u32 slot = 0; slot < tl_log_test_ring_count(); ++slot) {
        const LogRecord* r = tl_log_test_ring_at(slot);
        if (strcmp(r->msg, "msg 0") == 0) { found_msg_0 = true; }
        if (strcmp(r->msg, "msg 4096") == 0) { found_msg_4096 = true; }
    }
    TL_EXPECT_TRUE(!found_msg_0);    // the oldest record was overwritten
    TL_EXPECT_TRUE(found_msg_4096);  // the newest record survives
    tl_log_test_reset();
}

TL_TEST(log_message_truncates_at_capacity, "foundation") {
    tl_log_test_reset();
    char long_msg[TL_LOG_MSG_CAP + 64];
    for (u32 i = 0; i < sizeof(long_msg) - 1; ++i) { long_msg[i] = 'a'; }
    long_msg[sizeof(long_msg) - 1] = 0;
    TL_LOG_ERR("%s", long_msg);
    TL_ASSERT_EQ(tl_log_test_ring_count(), 1u);
    const LogRecord* r = tl_log_test_ring_at(0);
    TL_EXPECT_EQ((u32)r->len, (u32)(TL_LOG_MSG_CAP - 1));   // capacity minus the NUL
    TL_EXPECT_EQ(r->msg[TL_LOG_MSG_CAP - 1], '\0');
    tl_log_test_reset();
}
