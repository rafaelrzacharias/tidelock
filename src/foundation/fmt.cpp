// ---------------------------------------------------------------------------------------------
// fmt.cpp - fmt_buf STUB. See fmt.h's contract block: blocked on vendor/stb_sprintf (W1 platform
// lane, not yet landed). TL_FATAL rather than a hand-rolled formatter or silent no-op - CLAUDE.md
// "fail loudly and explicitly; no silent fallbacks or workarounds".
// ---------------------------------------------------------------------------------------------
#include "foundation/fmt.h"
#include "foundation/tl_assert.h"

u32 fmt_buf(Span<char> /*out*/, const char* /*fmt*/, ...) {
    TL_FATAL("fmt_buf: unimplemented - blocked on vendor/stb_sprintf (W1 platform lane), see fmt.h");
}
