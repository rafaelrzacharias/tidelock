// write_atomic.test.cpp - docs/PLATFORM.md §9.6 write_atomic_crash_safety.
//
// The spec's own wording is "killed (TerminateProcess/SIGKILL) by the parent at each of three
// instrumented points". This test's trigger process SELF-terminates at the matching point
// instead (os_file_atomic.cpp's TL_TEST_ATOMIC_KILL_AT hook) - deterministic to run, and
// observably identical: the process stops mid-function with no further bytes touched, which is
// the only thing the filesystem-state assertions below can see either way.
#include "runner/tl_test.h"
#include "platform_test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#define KILL_ENV "TL_TEST_ATOMIC_KILL_AT"
#define TARGET_PATH "tl_platform_write_atomic_target.txt"

// The trigger child. Inert without TL_TEST_ATOMIC_KILL_AT, and inert means SKIP, not a bare
// return: a return with zero checks is a FAILURE verdict (runner_ctx_verdict_zero_checks_is_a_
// failure), so `tl_tests` with no tag filter was red on this test. The PR lane runs --tag '!slow'
// and hid it; the nightly run, which drops the filter, would not have. Same finding, same fix as
// the tl_assert probe triggers (git 790f8fb).
TL_TEST(write_atomic_crash_trigger, "platform,slow") {
    if (getenv(KILL_ENV) == nullptr) {
        TL_SKIP("inert without TL_TEST_ATOMIC_KILL_AT; write_atomic_crash_safety sets it");
    }
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const u8 content[] = { 'A' };
    (void)api->file.write_atomic(api->file.ctx, sv(TARGET_PATH), Span<const u8>{ content, 1u });
}

namespace {

void run_trigger_at(int point) {
    char env_val[8];
    snprintf(env_val, sizeof(env_val), "%d", point);
#ifdef _WIN32
    char envset[64];
    snprintf(envset, sizeof(envset), KILL_ENV "=%s", env_val);
    _putenv(envset);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"\"%s\" --filter write_atomic_crash_trigger\"", TL_TESTS_EXE);
#else
    setenv(KILL_ENV, env_val, 1);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" --filter write_atomic_crash_trigger", TL_TESTS_EXE);
#endif
    (void)system(cmd);
#ifdef _WIN32
    _putenv(KILL_ENV "=");
#else
    unsetenv(KILL_ENV);
#endif
}

}  // namespace

TL_TEST(write_atomic_crash_safety, "platform,slow") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);

    // baseline: the old content, so a killed write must leave THIS untouched or fully replaced.
    {
        FILE* f = fopen(TARGET_PATH, "wb");
        TL_ASSERT_TRUE(f != nullptr);
        fwrite("OLD", 1, 3, f);
        fclose(f);
    }

    // "no stray .tmp.* after a successful call" is a promise about the UNINSTRUMENTED call
    // below, not about a killed one: a kill at point 1 (mid-write) or point 2 (after fsync,
    // before rename) legitimately leaves the tmp file on disk - the process died before the
    // rename that would remove it. The one guarantee at every kill point is the target file
    // itself, checked here.
    for (int point = 1; point <= 3; ++point) {
        run_trigger_at(point);

        FILE* f = fopen(TARGET_PATH, "rb");
        TL_ASSERT_TRUE(f != nullptr);
        char buf[16] = {};
        const usize n = fread(buf, 1, sizeof(buf) - 1u, f);
        fclose(f);
        const bool is_old = (n == 3u && memcmp(buf, "OLD", 3) == 0);
        const bool is_new = (n == 1u && buf[0] == 'A');
        TL_EXPECT_TRUE(is_old || is_new);
    }

    // clean up whatever stray tmp files the three kills left behind (each carried a different
    // pid, so up to three), so the "no stray tmp" check below is testing THIS call, not stale
    // debris from the kills above.
    {
        FileEntry entries[64];
        Result<u32> r = api->file.enumerate(api->file.ctx, sv("."), entries, 64u);
        TL_ASSERT_EQ(r.err, ERR_OK);
        const char* prefix = TARGET_PATH ".tmp.";
        const usize prefix_len = strlen(prefix);
        for (u32 i = 0; i < r.value; ++i) {
            if (strncmp(entries[i].name, prefix, prefix_len) == 0) { remove(entries[i].name); }
        }
    }

    // a normal, uninstrumented call succeeds and leaves the new content with no stray tmp.
    {
        const u8 content[] = { 'A' };
        TL_EXPECT_EQ(api->file.write_atomic(api->file.ctx, sv(TARGET_PATH), Span<const u8>{ content, 1u }), ERR_OK);
        FILE* f = fopen(TARGET_PATH, "rb");
        TL_ASSERT_TRUE(f != nullptr);
        char buf[4] = {};
        const usize n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        TL_EXPECT_TRUE(n == 1u && buf[0] == 'A');
    }

    {
        FileEntry entries[64];
        Result<u32> r = api->file.enumerate(api->file.ctx, sv("."), entries, 64u);
        TL_ASSERT_EQ(r.err, ERR_OK);
        const char* prefix = TARGET_PATH ".tmp.";
        const usize prefix_len = strlen(prefix);
        bool found_tmp = false;
        for (u32 i = 0; i < r.value; ++i) {
            if (strncmp(entries[i].name, prefix, prefix_len) == 0) { found_tmp = true; break; }
        }
        TL_EXPECT_FALSE(found_tmp);
    }

    remove(TARGET_PATH);
    platform_test_shutdown(api);
}
