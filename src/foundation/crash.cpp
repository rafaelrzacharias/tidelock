// crash.cpp - the panic path's terminal step (docs/TOOLING.md §9.3.9, docs/CPP-SUBSET.md §9 R-4).
// Tooling plane (RR-7): real io and a boot-installed callback are sanctioned here, unlike
// everywhere else in src/ - see foundation/crash.h's contract block for why.
#include "foundation/crash.h"

#include <stdio.h>
#include <stdlib.h>

namespace {
// The seam: null until platform/ installs the real OS-level writer. RR-7 names this exact shape
// - a boot-installed function pointer in the exempted tooling plane - as the one CPP-SUBSET.md §9
// R-3 rejected for the det-side panic ABI; here it carries none of that rejection's reasoning.
CrashInstallFn g_install = nullptr;
}  // namespace

void tl_crash_install(CrashInstallFn fn) {
    g_install = fn;
}

void tl_crash_raise(u8 reason, const char* origin, const char* file, u32 line, const char* msg) {
    if (g_install) {
        g_install(reason, origin, file, line, msg);
        // The installed writer is expected to terminate the process; if it somehow returns,
        // fall through to the built-in path rather than leaving the process in an undefined state.
    }
    // The built-in fallback (the only path today - platform/ has not landed). The literal prefix
    // "TL_FATAL" is fixed regardless of `origin`: docs/TESTING.md §9.1's fatal-expected tests grep
    // the child's stderr for it, and `origin` alone would miss the TL_CHECK/TL_ASSERT cases.
    fprintf(stderr, "TL_FATAL origin=%s %s:%u: %s\n", origin, file, (unsigned)line, msg);
    fflush(stderr);
    exit(2);
}
