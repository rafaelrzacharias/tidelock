// Shared entry point for the exes whose lanes have not run yet: tl_driver (docs/TESTING.md
// §9.2), tl_gate0 (docs/GATE0-BENCH.md), tl_hovel (docs/NETCODE.md §19). It fails loudly rather
// than pretending to run - no silent success (CLAUDE.md: fail loudly & explicitly).
#include "foundation/tl_types.h"

#include <stdio.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    fprintf(stderr, "%s: not implemented - W0 skeleton target only (docs/ROADMAP.md §2)\n", TL_STUB_EXE);
    return 70;   // EX_SOFTWARE: distinguishable from a real test failure (1) or fatal (2)
}
