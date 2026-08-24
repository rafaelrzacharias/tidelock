#pragma once
// ---------------------------------------------------------------------------------------------
// runner_core.h - the runner's pure decision functions, split out of main.cpp so they have
//   tests. Spec: docs/TESTING.md §1, §9.1.
// Why it exists: W1 runner review 1 - main.cpp held the glob matcher, the tag matcher, the suite
//   parser, the property seed and the pass/fail predicate as file-static functions in a TU with
//   `main`, so none of them could be reached from a test. "An audit is worth what its negative
//   test is worth" (LESSONS.md); the test infrastructure is the only determinism safety net
//   (docs/TESTING.md §1), so its own decisions are the last place that may go untested.
// Invariants: every function here is pure - no io, no allocation, no static state, no clock. The
//   tier-dependent branch is a PARAMETER (`dev_tier`), never a #if, so one build can test both
//   tiers' verdicts (main.cpp passes TL_DEV).
// Determinism: tl_seed_for is a pure function of (--seed, row index) - never wall-clock, never
//   the completion order of a child. Report order is the order of the selected index list, which
//   is test_list.inc's order, which is the sorted tree (cmake/testlist.cmake).
// Threading: none - callers are single-threaded.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

#include <string.h>   // strlen/strncmp/strchr/memcpy - the io-exempt half of docs/TESTING.md §8 R-2

// --- process exit codes, one home (docs/TESTING.md §9.1) -------------------------------------
// 0 = every selected test passed. 1 = a real failure (a test, or the runner refusing to report a
// vacuous green). 2 is RESERVED: it is the fatal contract's code, produced by tl_fatal's crash
// writer (src/foundation/crash.cpp on w1-tooling-rt), so the runner must never mint it itself.
// 3 is RESERVED across the tl_* family: it is tl_driver's divergence code (docs/TESTING.md §9.2)
// and a CI job that reads exit codes from both binaries must not have to know which it ran.
constexpr int TL_EXIT_OK   = 0;
constexpr int TL_EXIT_FAIL = 1;
constexpr int TL_EXIT_SKIP = 4;   // the child ran and declared itself skipped (TL_SKIP)
constexpr int TL_EXIT_FATAL = 2;  // the real tl_fatal's controlled exit (src/foundation/crash.cpp)

// How a child process terminated. `spawned` is the one that must be checked first: a spawn that
// never happened is not evidence of anything, least of all of an expected fatal.
struct ChildResult {
    bool spawned;
    bool abnormal;   // terminated other than by returning from main() (crash/signal/trap)
    int  exit_code;  // meaningful only when spawned && !abnormal
};

enum TestVerdict : u8 { VERDICT_PASS = 0, VERDICT_FAIL = 1, VERDICT_SKIP = 2 };

// --- selection --------------------------------------------------------------------------------

// '*' matches any run (including empty), '?' exactly one character. No character classes
// (docs/TESTING.md §1). An empty pattern matches only an empty string.
inline bool tl_glob_match(const char* pat, const char* s) {
    const char* star = nullptr;
    const char* mark = nullptr;
    while (*s) {
        if (*pat == '?' || *pat == *s) { ++pat; ++s; }
        else if (*pat == '*') { star = pat++; mark = s; }
        else if (star) { pat = star + 1; s = ++mark; }
        else { return false; }
    }
    while (*pat == '*') { ++pat; }
    return *pat == 0;
}

// True iff the comma-separated list `tags` contains `tag` as a whole element. Whole-element, not
// substring: --tag fast must not match a test tagged "fastpath".
inline bool tl_has_tag(const char* tags, const char* tag) {
    const usize n = strlen(tag);
    if (n == 0) { return false; }
    for (const char* p = tags; *p;) {
        const char* end = strchr(p, ',');
        const usize len = end ? (usize)(end - p) : strlen(p);
        if (len == n && strncmp(p, tag, n) == 0) { return true; }
        if (!end) { break; }
        p = end + 1;
    }
    return false;
}

// Suite = the last directory component of the test file ("tests/foundation/x.test.cpp" ->
// "foundation"). A path with no directory part is its own suite. Truncates into `cap` rather
// than overrunning; `cap` must be >= 1.
inline void tl_suite_of(const char* file, char* out, usize cap) {
    const char* last = nullptr;
    const char* prev = nullptr;
    for (const char* q = file; *q; ++q) {
        if (*q == '/' || *q == 0x5c) { prev = last; last = q; }   // 0x5c is the backslash
    }
    const char* begin = last ? (prev ? prev + 1 : file) : file;
    usize n = last ? (usize)(last - begin) : strlen(file);
    if (n >= cap) { n = cap - 1; }
    memcpy(out, begin, n);
    out[n] = 0;
}

// --- property-test seeding --------------------------------------------------------------------

// A seed that is a pure function of (--seed, row index) - deterministic, never wall-clock
// (docs/TESTING.md §1). The same (seed, index) must produce the same value in the parent and in
// an --isolate child, which is why the parent passes --seed down the child command line.
inline u32 tl_seed_for(u32 global_seed, u32 index) {
    u32 h = global_seed ^ 0x9e3779b9u;
    h ^= index * 0x85ebca6bu;
    h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    return h;
}

// --- verdicts ---------------------------------------------------------------------------------

// The verdict for a test that ran IN THIS PROCESS. A test that recorded no checks at all is a
// FAIL, not a pass: a body whose every check sits inside an #if that this tier did not take
// (docs/LESSONS.md's tier-conditional pattern), or a body someone emptied, would otherwise report
// green having verified nothing - the disarmed-tripwire class docs/TESTING.md §1 exists to stop.
// The sanctioned way to run no checks is TL_SKIP, which is reported as SKIP, not as PASS.
inline TestVerdict tl_ctx_verdict(u32 failures, u32 checks, bool skipped) {
    if (failures) { return VERDICT_FAIL; }
    if (skipped)  { return VERDICT_SKIP; }
    if (checks == 0) { return VERDICT_FAIL; }
    return VERDICT_PASS;
}

// The verdict for a test that ran in a CHILD process. `dev_tier` is TL_DEV at the call site.
//
// A fatal-expected test (docs/TESTING.md §9.1) is judged as a fatal expectation only on a tier
// where TL_ASSERT is compiled in; on netcode/ship the call under test cannot fatal, so the body
// is written to TL_SKIP and is judged as an ordinary child.
//
// KNOWN GAP (TODO.md): the contract is exit code 2 + the TL_FATAL_MARKER stderr line, but the
// runner does not capture the child's stderr yet and this tree's tl_fatal is still the trap stub.
// Until both land, "terminated abnormally" stands in for it. What is NOT deferred: a child that
// never spawned is a FAIL on every path - the earlier code returned "expected fatal, PASS" for a
// failed CreateProcess, so a broken exe path turned every fatal-expected test green (review 1).
inline TestVerdict tl_child_verdict(bool expect_fatal, bool dev_tier, const ChildResult& cr) {
    if (!cr.spawned) { return VERDICT_FAIL; }
    // Since the wave merge linked the real tl_fatal (exit(2) + stderr marker), a controlled
    // fatal is a NORMAL exit with TL_EXIT_FATAL; an abnormal exit is an UNcontrolled crash
    // (segfault, stack overflow) and fails. The marker + file:line half of the tightening still
    // needs child-stderr capture (TODO.md, "the TL_TEST_EXPECT_FATAL tightening").
    if (expect_fatal && dev_tier) { return (!cr.abnormal && cr.exit_code == TL_EXIT_FATAL) ? VERDICT_PASS : VERDICT_FAIL; }
    if (cr.abnormal) { return VERDICT_FAIL; }
    if (cr.exit_code == TL_EXIT_OK)   { return VERDICT_PASS; }
    if (cr.exit_code == TL_EXIT_SKIP) { return VERDICT_SKIP; }
    return VERDICT_FAIL;
}
