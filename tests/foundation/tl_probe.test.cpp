// tl_probe.test.cpp - docs/TOOLING.md §9.5 "probe_tsv_golden", the parts testable without
// TL_GOLDEN_TSV (TESTING.md's runner does not have it yet - filed in TODO.md; these assert the
// staging buffer's exact bytes directly instead of a golden-file diff). Calls the underlying
// tl_probe_* functions with a caller-computed key, not the TL_PROBE_* macros - those need
// NameHash's "lit"_id literal (foundation/hash.h, not built yet; tl_probe.h's contract block).
#include "runner/tl_test.h"
#include "foundation/tl_probe.h"

#include <string.h>

TL_TEST(probe_log_throttles_by_tick_count, "foundation") {
#if TL_DEV
    tl_probe_test_reset();
    tl_probe_test_set_tick(0);
    tl_probe_log(1, "k.log", 10, 0, 3);   // first call always rows
    tl_probe_test_set_tick(2);
    tl_probe_log(1, "k.log", 20, 0, 3);   // 2 - 0 < 3: throttled, no row
    tl_probe_test_set_tick(3);
    tl_probe_log(1, "k.log", 30, 0, 3);   // 3 - 0 >= 3: rows
    const char* s = tl_probe_test_staging();
    TL_EXPECT_TRUE(strstr(s, "0\tk.log\t10\n") != nullptr);
    TL_EXPECT_TRUE(strstr(s, "2\tk.log\t20\n") == nullptr);
    TL_EXPECT_TRUE(strstr(s, "3\tk.log\t30\n") != nullptr);
    TL_ASSERT_EQ(tl_probe_key_count(), 1u);
    const ProbeKey* k = tl_probe_key_at(0);
    TL_EXPECT_EQ((u32)k->count, 2u);      // the throttled call did not update stats
    TL_EXPECT_EQ((u32)k->changes, 1u);    // 10 -> 30 is one change; the baseline is not a change
    tl_probe_test_reset();
#else
    // The probe runtime and its tl_probe_test_* hooks are TL_DEV-only symbols
    // (docs/TOOLING.md section 9: probes are compiled out of netcode/ship, argument
    // evaluation included). A visible SKIP, never a vacuous pass.
    TL_SKIP("probes are dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}

TL_TEST(probe_log_fx_scales_by_frac_bits, "foundation") {
#if TL_DEV
    tl_probe_test_reset();
    tl_probe_test_set_tick(0);
    tl_probe_log(2, "k.fx", 3 << 18, 18, 1);   // pos_t-shaped: 3.0 at FRAC=18
    const char* s = tl_probe_test_staging();
    TL_EXPECT_TRUE(strstr(s, "0\tk.fx\t3\n") != nullptr);
    tl_probe_test_reset();
#else
    // The probe runtime and its tl_probe_test_* hooks are TL_DEV-only symbols
    // (docs/TOOLING.md section 9: probes are compiled out of netcode/ship, argument
    // evaluation included). A visible SKIP, never a vacuous pass.
    TL_SKIP("probes are dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}

TL_TEST(probe_on_change_rows_only_past_eps, "foundation") {
#if TL_DEV
    tl_probe_test_reset();
    tl_probe_on_change(3, "k.chg", 100, 5);   // first call always rows
    tl_probe_on_change(3, "k.chg", 103, 5);   // |103-100|=3 <= 5: no row
    tl_probe_on_change(3, "k.chg", 200, 5);   // |200-100|=100 > 5: rows
    const char* s = tl_probe_test_staging();
    TL_EXPECT_TRUE(strstr(s, "\tk.chg\t100\n") != nullptr);
    TL_EXPECT_TRUE(strstr(s, "\tk.chg\t103\n") == nullptr);
    TL_EXPECT_TRUE(strstr(s, "\tk.chg\t200\n") != nullptr);
    tl_probe_test_reset();
#else
    // The probe runtime and its tl_probe_test_* hooks are TL_DEV-only symbols
    // (docs/TOOLING.md section 9: probes are compiled out of netcode/ship, argument
    // evaluation included). A visible SKIP, never a vacuous pass.
    TL_SKIP("probes are dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}

TL_TEST(probe_mark_rows_every_call, "foundation") {
#if TL_DEV
    tl_probe_test_reset();
    tl_probe_mark(4, "k.mark");
    tl_probe_mark(4, "k.mark");
    tl_probe_mark(4, "k.mark");
    const char* s = tl_probe_test_staging();
    u32 rows = 0;
    for (const char* p = s; (p = strstr(p, "k.mark\t\n")) != nullptr; p += 8) { ++rows; }
    TL_EXPECT_EQ(rows, 3u);
    TL_ASSERT_EQ(tl_probe_key_count(), 1u);
    TL_EXPECT_EQ((u32)tl_probe_key_at(0)->count, 3u);
    tl_probe_test_reset();
#else
    // The probe runtime and its tl_probe_test_* hooks are TL_DEV-only symbols
    // (docs/TOOLING.md section 9: probes are compiled out of netcode/ship, argument
    // evaluation included). A visible SKIP, never a vacuous pass.
    TL_SKIP("probes are dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}

TL_TEST(probe_assert_out_of_range_rows_and_logs, "foundation") {
#if TL_DEV
    tl_probe_test_reset();
    tl_probe_assert(5, "k.assert", 50, 0, 10);   // out of range: [0,10]
    const char* s = tl_probe_test_staging();
    TL_EXPECT_TRUE(strstr(s, "\tk.assert\t50\n") != nullptr);
    tl_probe_test_reset();
    tl_probe_assert(5, "k.assert", 5, 0, 10);    // in range: no row
    s = tl_probe_test_staging();
    TL_EXPECT_TRUE(strstr(s, "k.assert") == nullptr);
    tl_probe_test_reset();
#else
    // The probe runtime and its tl_probe_test_* hooks are TL_DEV-only symbols
    // (docs/TOOLING.md section 9: probes are compiled out of netcode/ship, argument
    // evaluation included). A visible SKIP, never a vacuous pass.
    TL_SKIP("probes are dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}

TL_TEST(probe_disabled_key_emits_nothing, "foundation") {
#if TL_DEV
    tl_probe_test_reset();
    tl_probe_test_set_enabled(6, "k.off", 0);
    tl_probe_log(6, "k.off", 42, 0, 1);
    const char* s = tl_probe_test_staging();
    TL_EXPECT_TRUE(strstr(s, "k.off") == nullptr);
    TL_ASSERT_EQ(tl_probe_key_count(), 1u);
    TL_EXPECT_EQ((u32)tl_probe_key_at(0)->count, 0u);   // no stats update either
    tl_probe_test_reset();
#else
    // The probe runtime and its tl_probe_test_* hooks are TL_DEV-only symbols
    // (docs/TOOLING.md section 9: probes are compiled out of netcode/ship, argument
    // evaluation included). A visible SKIP, never a vacuous pass.
    TL_SKIP("probes are dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}

TL_TEST(probe_summary_line_matches_registration_order, "foundation") {
#if TL_DEV
    tl_probe_test_reset();
    tl_probe_test_set_tick(0);
    tl_probe_log(10, "a", 2, 0, 1);
    tl_probe_test_set_tick(1);   // n=1: the second call must be at least 1 tick later to not throttle
    tl_probe_log(10, "a", 4, 0, 1);
    tl_probe_log(11, "b", 100, 0, 1);
    tl_probe_write_summary();
    const char* s = tl_probe_test_staging();
    const char* summary = strstr(s, "#summary\n");
    TL_ASSERT_TRUE(summary != nullptr);
    // "a": count=2 changes=1 min=2 max=4 mean=3 first=2 last=4
    TL_EXPECT_TRUE(strstr(summary, "a\t2\t1\t2\t4\t3\t2\t4\n") != nullptr);
    // "b": count=1 changes=0 min=max=mean=first=last=100
    TL_EXPECT_TRUE(strstr(summary, "b\t1\t0\t100\t100\t100\t100\t100\n") != nullptr);
    const char* a_pos = strstr(summary, "a\t");
    const char* b_pos = strstr(summary, "b\t");
    TL_ASSERT_TRUE(a_pos != nullptr && b_pos != nullptr);
    TL_EXPECT_TRUE(a_pos < b_pos);   // registration order: "a" (key 10) before "b" (key 11)
    tl_probe_test_reset();
#else
    // The probe runtime and its tl_probe_test_* hooks are TL_DEV-only symbols
    // (docs/TOOLING.md section 9: probes are compiled out of netcode/ship, argument
    // evaluation included). A visible SKIP, never a vacuous pass.
    TL_SKIP("probes are dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}
