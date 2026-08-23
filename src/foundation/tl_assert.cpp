// tl_assert.cpp - the panic ABI's runtime (docs/CPP-SUBSET.md §9 R-3). Non-det half of foundation
// (src/foundation/CMakeLists.txt TL_FOUNDATION_NONDET), so it may reach io once the tooling-rt
// lane lands the crash writer (docs/TOOLING.md §9.3.9). Until then this is the header-first stub
// the W1 fx lane needed to link: every entry traps. The arguments are kept live in the frame so a
// debugger shows file:line:msg at the trap site; nothing is written because foundation has no
// sanctioned io path of its own (docs/CPP-SUBSET.md §1) - that path is the tooling lane's.
#include "foundation/tl_assert.h"

// Kept out of line and never inlined so the frame holding (file, line, msg) survives to the trap.
__attribute__((noinline)) static void tl_panic_trap(const char* file, u32 line, const char* msg) {
    (void)file; (void)line; (void)msg;
    __builtin_trap();
}

extern "C" {
void tl_fatal(const char* file, u32 line, const char* msg) { tl_panic_trap(file, line, msg); __builtin_unreachable(); }
void tl_check_failed(const char* file, u32 line, const char* expr) { tl_panic_trap(file, line, expr); __builtin_unreachable(); }
void tl_assert_failed(const char* file, u32 line, const char* expr) { tl_panic_trap(file, line, expr); __builtin_unreachable(); }
}
