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

// The trigger half. A tag is NOT enough to keep this out of an ordinary run: tests/runner/main.cpp
// selects every test when no --tag is given, so a bare `tl_tests` ran this, took the exit(2), and
// reported nothing at all (measured: `tl_tests --filter "tl_assert_forced_fatal*"` -> exit 2, no
// PASS lines). The env var the checker sets is the actual gate; the tag only keeps it off the fast
// lane. Without the var this is a no-op that costs one getenv.
#define TL_FATAL_PROBE_ENV "TL_FATAL_PROBE"

TL_TEST(tl_assert_forced_fatal_trigger, "foundation,slow") {
    (void)t;
    if (getenv(TL_FATAL_PROBE_ENV) == nullptr) { return; }
    TL_FATAL("forced by tl_assert_forced_fatal_trigger");
}

// The same for TL_CHECK, so the `origin=` field is pinned as varying while the leading token does
// not - the property docs/TESTING.md §9.1's one check depends on.
TL_TEST(tl_assert_forced_check_trigger, "foundation,slow") {
    (void)t;
    if (getenv(TL_FATAL_PROBE_ENV) == nullptr) { return; }
    TL_CHECK(1 == 2);
}

namespace {
// Runs `tl_tests --filter <name>` with TL_FATAL_PROBE set, and returns the child's exit code;
// its stderr lands in `out_path`.
int run_fatal_probe(const char* name, const char* out_path) {
#ifdef _WIN32
    _putenv(TL_FATAL_PROBE_ENV "=1");
#else
    setenv(TL_FATAL_PROBE_ENV, "1", 1);
#endif
    char cmd[1024];
#ifdef _WIN32
    // cmd.exe's `/c` strips only the FIRST and LAST quote of the whole command line when the
    // command starts with one; without an extra wrapping pair, the redirect's own quotes corrupt
    // the exe path (a well-known cmd.exe quirk - POSIX's /bin/sh -c has no such rule, so this
    // extra pair must NOT be added there).
    snprintf(cmd, sizeof(cmd), "\"\"%s\" --filter %s 2> \"%s\"\"", TL_TESTS_EXE, name, out_path);
#else
    snprintf(cmd, sizeof(cmd), "\"%s\" --filter %s 2> \"%s\"", TL_TESTS_EXE, name, out_path);
#endif
    const int rc = system(cmd);
#ifdef _WIN32
    _putenv(TL_FATAL_PROBE_ENV "=");
#else
    unsetenv(TL_FATAL_PROBE_ENV);
#endif
#ifdef _WIN32
    return rc;   // no wait-status encoding on Windows - the return value IS the exit code
#else
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
}

// Reads the whole file into `buf`, NUL-terminated, strips CRs, and removes it.
// The CR strip is not cosmetic: crash.cpp writes one `\n`, but the child's stderr is a text-mode
// stream, so the Windows CRT translates it to `\r\n` on the way to the redirect file. Whoever
// implements TL_TEST_EXPECT_FATAL against docs/TESTING.md §9.1's marker has to match the line
// ending the same way - the contract is one line terminated by a newline, not by the two exact
// bytes the doc's `\n` might suggest.
bool slurp(const char* path, char* buf, usize cap) {
    FILE* f = fopen(path, "rb");
    if (!f) { return false; }
    const size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    remove(path);
    char* w = buf;
    for (const char* r = buf; *r; ++r) {
        if (*r != '\r') { *w++ = *r; }
    }
    *w = 0;
    return true;
}
}  // namespace

TL_TEST(tl_assert_forced_fatal_exits_2_with_marker, "foundation") {
    const char* out_path = "tl_assert_fatal_probe.stderr.tmp";
    const int code = run_fatal_probe("tl_assert_forced_fatal_trigger", out_path);
    TL_EXPECT_EQ(code, 2);

    char buf[4096];
    TL_ASSERT_TRUE(slurp(out_path, buf, sizeof(buf)));
    // The full contract, not just the leading token: docs/TESTING.md §9.1's fatal-expected tests
    // and the runner lane's TL_TEST_EXPECT_FATAL are built against this exact shape, so a change
    // to `origin=`, to the colon-space, or to the ordering has to break HERE and not at merge.
    const char* line = strstr(buf, "TL_FATAL origin=TL_FATAL ");
    TL_ASSERT_TRUE(line != nullptr);
    const char* tail = strstr(line, "tl_assert.test.cpp:");
    TL_ASSERT_TRUE(tail != nullptr);
    // "<file>:<line>: <msg>\n" - the line number is whatever the trigger sits on, so step over the
    // digits and pin the separator and the message.
    const char* p = tail + strlen("tl_assert.test.cpp:");
    TL_EXPECT_TRUE(*p >= '0' && *p <= '9');
    while (*p >= '0' && *p <= '9') { ++p; }
    TL_EXPECT_TRUE(strcmp(p, ": forced by tl_assert_forced_fatal_trigger\n") == 0);
}

// Same contract, different tier: the leading token stays "TL_FATAL", `origin` names TL_CHECK, and
// the message is the stringised condition.
TL_TEST(tl_check_failure_reports_its_own_origin, "foundation") {
    const char* out_path = "tl_assert_check_probe.stderr.tmp";
    const int code = run_fatal_probe("tl_assert_forced_check_trigger", out_path);
    TL_EXPECT_EQ(code, 2);

    char buf[4096];
    TL_ASSERT_TRUE(slurp(out_path, buf, sizeof(buf)));
    TL_EXPECT_TRUE(strstr(buf, "TL_FATAL origin=TL_CHECK ") != nullptr);
    TL_EXPECT_TRUE(strstr(buf, ": 1 == 2\n") != nullptr);
}

// And the trigger tests themselves are inert without the env var - the property that keeps a bare
// `tl_tests` run alive. Calling the body directly is the point: no child process, no exit(2).
TL_TEST(fatal_triggers_are_inert_without_the_env_var, "foundation") {
    TL_EXPECT_TRUE(getenv(TL_FATAL_PROBE_ENV) == nullptr);
    test_tl_assert_forced_fatal_trigger(t);
    test_tl_assert_forced_check_trigger(t);
    TL_EXPECT_TRUE(true);   // reaching this line is the assertion
}
