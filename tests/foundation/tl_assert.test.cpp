// tl_assert.test.cpp - proves the real panic-ABI-to-crash-writer contract (docs/CPP-SUBSET.md §9
// R-3, docs/TOOLING.md §9.3.9): a forced TL_FATAL exits with code 2 and prints the "TL_FATAL"
// marker docs/TESTING.md §9.1's fatal-expected tests will grep for once the runner lane's
// TL_TEST_EXPECT_FATAL lands (TODO.md). Relaunches THIS SAME tl_tests binary filtered to the
// trigger test below, rather than a bespoke probe exe or the not-yet-built EXPECT_FATAL machinery
// - tests/ carries the io/process exemption of docs/TESTING.md §8 R-2.
#include "runner/tl_test.h"
#include "foundation/tl_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

// The trigger half: never runs as part of the normal suite (its "slow" tag excludes it from
// `--tag !slow`), only via the exact --filter match below, in its own process.
TL_TEST(tl_assert_forced_fatal_trigger, "foundation,slow") {
    (void)t;
    TL_FATAL("forced by tl_assert_forced_fatal_trigger");
}

TL_TEST(tl_assert_forced_fatal_exits_2_with_marker, "foundation") {
    const char* out_path = "tl_assert_fatal_probe.stderr.tmp";
    char cmd[1024];
#ifdef _WIN32
    // cmd.exe's `/c` strips only the FIRST and LAST quote of the whole command line when the
    // command starts with one; without an extra wrapping pair, the redirect's own quotes corrupt
    // the exe path (a well-known cmd.exe quirk - POSIX's /bin/sh -c has no such rule, so this
    // extra pair must NOT be added there).
    snprintf(cmd, sizeof(cmd), "\"\"%s\" --filter tl_assert_forced_fatal_trigger 2> \"%s\"\"",
             TL_TESTS_EXE, out_path);
#else
    snprintf(cmd, sizeof(cmd), "\"%s\" --filter tl_assert_forced_fatal_trigger 2> \"%s\"",
             TL_TESTS_EXE, out_path);
#endif
    const int rc = system(cmd);
#ifdef _WIN32
    const int code = rc;   // no wait-status encoding on Windows - the return value IS the exit code
#else
    const int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
    TL_EXPECT_EQ(code, 2);

    FILE* f = fopen(out_path, "rb");
    TL_ASSERT_TRUE(f != nullptr);
    char buf[4096];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    remove(out_path);
    TL_EXPECT_TRUE(strstr(buf, "TL_FATAL") != nullptr);
    TL_EXPECT_TRUE(strstr(buf, "forced by tl_assert_forced_fatal_trigger") != nullptr);
}
