// runner_timeout.test.cpp - the per-child timeout ruled on 2026-08-24 (TODO.md; docs/TESTING.md
// §9.1 `--timeout-ms`, §6's lane values). A test that hangs - or a fatal-expected test whose
// assert does not fire and whose body loops - used to stall the PR lane forever with no output,
// because both wait paths were unbounded (`WaitForMultipleObjects(..., INFINITE)` /
// `waitpid(pid, &status, 0)`).
//
// The shape is the one tl_assert.test.cpp established: an env-var-gated TRIGGER that really does
// hang, plus a CHECKER that relaunches this same tl_tests binary filtered to it. A tag is not
// enough of a gate - a bare `tl_tests` selects every test whatever its tags - so the env var is
// what keeps the trigger inert in an ordinary run. tests/ carries the io/process exemption of
// docs/TESTING.md §8 R-2.
#include "runner/tl_test.h"
#include "foundation/tl_assert.h"   // TL_FATAL, for the fatal-expected trigger below

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#define TL_HANG_PROBE_ENV "TL_HANG_PROBE"

// The trigger. Without the env var it SKIPs - it does not silently record nothing: a body that
// runs zero checks is scored FAIL by tl_ctx_verdict (runner_core.h), which is why TL_SKIP and not
// a bare `return` (TODO.md carries the two tl_assert.test.cpp triggers that still do the latter
// and therefore fail a bare run).
TL_TEST(runner_timeout_hang_trigger, "runner,slow") {
    if (getenv(TL_HANG_PROBE_ENV) == nullptr) {
        TL_SKIP("inert without " TL_HANG_PROBE_ENV "; runner_timeout_ms_kills_the_child sets it");
    }
    // `volatile`, not an empty loop: a side-effect-free infinite loop is undefined behaviour in
    // C++20 (the forward-progress guarantee), and clang is entitled to delete it - which would
    // turn this trigger into a fast clean exit and the checker below into a vacuous pass.
    volatile u64 spin = 0u;
    for (;;) { spin = spin + 1u; }
}

// The SERIAL wait path's trigger, and the exact scenario the ruling was filed for: "a
// fatal-expected test whose assert does not fire and whose body loops". Without --isolate the
// runner spawns a child only for fatal-expected rows and waits on it with the single-child path
// (WaitForSingleObject / the single-pid waitpid loop), which is a different wait from the pool's.
// With the env var set it hangs; without it, it fatals as a fatal-expected row is supposed to, so
// an ordinary run scores it PASS rather than dragging a permanent FAIL through the suite.
TL_TEST_EXPECT_FATAL(runner_timeout_fatal_expected_hang_trigger, "runner,slow") {
    // Tier-live since the wave merge: tl_child_verdict judges fatal-expected rows on every tier
    // (runner_core.h), so the old TL_DEV gate here was a stale skip - the same class 088da07
    // removed from registry/vmem_arena and missed here (sweep D3, 2026-08-25). TL_FATAL is live
    // in every tier.
    (void)t;
    if (getenv(TL_HANG_PROBE_ENV) != nullptr) {
        volatile u64 spin = 0u;
        for (;;) { spin = spin + 1u; }   // the assert that should have fired, didn't
    }
    TL_FATAL("runner_timeout_fatal_expected_hang_trigger, inert path");
}

namespace {

// Runs tl_tests with `args`, both streams redirected to `out_path`, and returns its exit code.
int run_probe(const char* args, const char* out_path, bool set_hang_env) {
    if (set_hang_env) {
#ifdef _WIN32
        _putenv(TL_HANG_PROBE_ENV "=1");
#else
        setenv(TL_HANG_PROBE_ENV, "1", 1);
#endif
    }
    char cmd[1024];
#ifdef _WIN32
    // cmd.exe's `/c` strips only the first and last quote of a command line that starts with
    // one, so the whole thing needs an extra wrapping pair or the redirect's quotes corrupt the
    // exe path (the quirk tl_assert.test.cpp documents; /bin/sh -c has no such rule).
    snprintf(cmd, sizeof(cmd), "\"\"%s\" %s > \"%s\" 2>&1\"", TL_TESTS_EXE, args, out_path);
#else
    snprintf(cmd, sizeof(cmd), "\"%s\" %s > \"%s\" 2>&1", TL_TESTS_EXE, args, out_path);
#endif
    const int rc = system(cmd);
    if (set_hang_env) {
#ifdef _WIN32
        _putenv(TL_HANG_PROBE_ENV "=");
#else
        unsetenv(TL_HANG_PROBE_ENV);
#endif
    }
#ifdef _WIN32
    return rc;   // no wait-status encoding on Windows - the return value IS the exit code
#else
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
}

// Reads the whole file into `buf`, NUL-terminated, and removes it.
bool slurp(const char* path, char* buf, usize cap) {
    FILE* f = fopen(path, "rb");
    if (!f) { return false; }
    const size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    remove(path);
    return true;
}

}  // namespace

TL_TEST(runner_timeout_ms_kills_the_child_and_reports_TIMEOUT, "runner") {
    // The whole contract in one run: the hung child is killed, TIMEOUT is its own status in the
    // TSV and the summary, the run FAILS, the test is NAMED, and docs/TESTING.md §6's P0-flake
    // line is printed. Without --timeout-ms this same command never returns, which is the
    // defect the ruling closes - so reaching the assertions at all is part of the evidence.
    const char* log_path = "tl_timeout_probe.log.tmp";
    const char* tsv_path = "tl_timeout_probe.tsv.tmp";
    char args[512];
    snprintf(args, sizeof(args),
             "--filter runner_timeout_hang_trigger --isolate --timeout-ms 1000 --report %s", tsv_path);
    const int code = run_probe(args, log_path, true);
    TL_EXPECT_EQ(code, 1);   // a timeout fails the run

    char log[8192];
    TL_ASSERT_TRUE(slurp(log_path, log, sizeof(log)));
    TL_EXPECT_TRUE(strstr(log, "TIMEOUT") != nullptr);                        // its own status
    TL_EXPECT_TRUE(strstr(log, "runner_timeout_hang_trigger TIMED OUT") != nullptr);   // named
    TL_EXPECT_TRUE(strstr(log, "1 timed out") != nullptr);                    // its own counter
    TL_EXPECT_TRUE(strstr(log, "determinism-gate flake") != nullptr);         // §6's P0 line
    TL_EXPECT_TRUE(strstr(log, "section 6") != nullptr);
    // Not miscounted as an ordinary failure, and never as a pass or a skip.
    TL_EXPECT_TRUE(strstr(log, "0 passed") != nullptr);
    TL_EXPECT_TRUE(strstr(log, "0 failed") != nullptr);

    char tsv[4096];
    TL_ASSERT_TRUE(slurp(tsv_path, tsv, sizeof(tsv)));
    TL_EXPECT_TRUE(strstr(tsv, "runner\trunner_timeout_hang_trigger\tTIMEOUT\t") != nullptr);
}

TL_TEST(runner_timeout_ms_does_not_shoot_healthy_children, "runner") {
    // The other half: a generous timeout must change nothing, and 0 must really mean off. Both
    // over a real (fast) test in a real --isolate child, not over the flag parser.
    const char* log_path = "tl_timeout_ok.log.tmp";
    char log[8192];

    const int with_timeout = run_probe("--filter runner_glob_match --isolate --timeout-ms 60000", log_path, false);
    TL_EXPECT_EQ(with_timeout, 0);
    TL_ASSERT_TRUE(slurp(log_path, log, sizeof(log)));
    TL_EXPECT_TRUE(strstr(log, "1 passed") != nullptr);
    TL_EXPECT_TRUE(strstr(log, "0 timed out") != nullptr);

    const int off = run_probe("--filter runner_glob_match --isolate --timeout-ms 0", log_path, false);
    TL_EXPECT_EQ(off, 0);
    TL_ASSERT_TRUE(slurp(log_path, log, sizeof(log)));
    TL_EXPECT_TRUE(strstr(log, "1 passed") != nullptr);

    // A malformed value is a loud refusal, not a silently disarmed timeout: --workers taught
    // this lesson once already, where a negative count became (u32)(-1).
    const int bad = run_probe("--filter runner_glob_match --timeout-ms -1", log_path, false);
    TL_EXPECT_EQ(bad, 1);
    TL_ASSERT_TRUE(slurp(log_path, log, sizeof(log)));
    TL_EXPECT_TRUE(strstr(log, "--timeout-ms must be 0 (off) or a positive") != nullptr);

    // And non-numeric garbage is the same refusal, never atoll's silent 0=off (sweep D1,
    // 2026-08-25): "12x000" parsed as 12 ms would shoot healthy children; "abc" as 0 would
    // disarm the net entirely. Both classes ride on one strict parser (rc_parse_u63).
    const int garbage = run_probe("--filter runner_glob_match --timeout-ms 12x000", log_path, false);
    TL_EXPECT_EQ(garbage, 1);
    TL_ASSERT_TRUE(slurp(log_path, log, sizeof(log)));
    TL_EXPECT_TRUE(strstr(log, "--timeout-ms must be 0 (off) or a positive") != nullptr);
}

TL_TEST(runner_timeout_ms_bounds_the_serial_wait_path_too, "runner") {
    // The pool's WaitForMultipleObjects/waitpid(-1) is not the only unbounded wait: a
    // fatal-expected row spawns a child even without --isolate, and the runner waits on it with
    // the single-child path. The ruling names both, so both are covered. Tier-live since the
    // wave merge (the trigger's stale TL_DEV gate fell with it - sweep D3, 2026-08-25).
    const char* log_path = "tl_timeout_serial.log.tmp";
    const int code = run_probe("--filter runner_timeout_fatal_expected_hang_trigger --timeout-ms 1000",
                               log_path, true);
    TL_EXPECT_EQ(code, 1);
    char log[8192];
    TL_ASSERT_TRUE(slurp(log_path, log, sizeof(log)));
    TL_EXPECT_TRUE(strstr(log, "TIMEOUT") != nullptr);
    TL_EXPECT_TRUE(strstr(log, "runner_timeout_fatal_expected_hang_trigger TIMED OUT") != nullptr);
    TL_EXPECT_TRUE(strstr(log, "1 timed out") != nullptr);
    // The critical one: a hung fatal-expected child killed by the runner must NOT read as a
    // satisfied fatal expectation. TIMEOUT beats expect_fatal in tl_child_verdict for exactly
    // this reason (the kill code is the runner's own).
    TL_EXPECT_TRUE(strstr(log, "0 passed") != nullptr);
}
