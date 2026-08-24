// tl_assert.cpp - the panic ABI's runtime (docs/CPP-SUBSET.md §9 R-3). Tooling plane (RR-7, §9
// R-4): real io and the crash-writer seam (foundation/crash.h) are sanctioned here. Each tier
// routes to its own R-3 symbol so the report names which one fired (docs/TOOLING.md §9.1).
#include "foundation/tl_assert.h"
#include "foundation/crash.h"
#include "foundation/tl_log.h"

namespace {
// TOOLING.md §9.3.9's chain is TL_FATAL -> tl_fatal -> TL_LOG_ERR -> crash.raise_fatal. Not the
// TL_LOG_ERR macro itself: it would bind __FILE__/__LINE__ to this TU, not the caller's site.
[[noreturn]] void log_then_crash(const char* origin, const char* file, u32 line, const char* msg) {
    tl_log_write(LOG_ERR, file, line, "%s", msg);
    tl_crash_raise(CRASH_FATAL, origin, file, line, msg);
}
}  // namespace

extern "C" {
void tl_fatal(const char* file, u32 line, const char* msg) { log_then_crash("TL_FATAL", file, line, msg); }
void tl_check_failed(const char* file, u32 line, const char* expr) { log_then_crash("TL_CHECK", file, line, expr); }
void tl_assert_failed(const char* file, u32 line, const char* expr) { log_then_crash("TL_ASSERT", file, line, expr); }
}
