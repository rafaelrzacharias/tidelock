// fmt.cpp - fmt_buf over vendored stb_sprintf. Spec: docs/CONTAINERS.md §8.6/§8.6b.
//
// Implemented under a narrow, additive exception to cone discipline (steward-relayed ruling,
// Rafael, 2026-08-27, PR #16): fmt.h's own stub named the unblock condition ("the body is filled
// in the day vendor/stb_sprintf/ lands"), that day came, CONTAINERS.md's home lane (w1-containers)
// is merged and closed, and this lane (editor) is the blocked real consumer
// (docs/ROADMAP.md's "pulled in by a real consumer, never pushed on spec" - trace_export.cpp,
// TOOLING.md §9.3.2, needs fmt_buf for its Chrome-trace JSON). Strictly additive: only this
// file's TL_FATAL stub body is replaced, nothing else in foundation/ changes; the exception is
// named here rather than assumed.
#include "foundation/fmt.h"

#include <stb_sprintf.h>

#include <stdarg.h>

u32 fmt_buf(Span<char> out, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    // stbsp_vsnprintf's contract (vendor/stb/stb_sprintf.h): always NUL-terminates within `out`
    // (unlike Windows' non-standard _snprintf) and returns the length that WOULD have been
    // written, excluding the NUL - fmt.h's own contract, verbatim. `count` is `int`; Span<char>'s
    // `count` is u32, so a caller-supplied buffer past INT_MAX would be a real truncation, not
    // just a cast concern - no caller in this tree approaches that size (logs/CSV/editor text),
    // so this is a documented limit, not a guarded one.
    const int n = stbsp_vsnprintf(out.data, (int)out.count, fmt, ap);
    va_end(ap);
    return (n < 0) ? 0u : (u32)n;
}
