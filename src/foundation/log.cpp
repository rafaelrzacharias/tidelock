// log.cpp - the ring + stderr sink behind TL_LOG_* (docs/TOOLING.md §9.2, §9.3.1's log half).
// Tooling plane (RR-7, docs/CPP-SUBSET.md §9 R-4): real io and namespace-scope mutable state are
// sanctioned here - see foundation/tl_log.h's contract block for the current simplifications
// (no RingBuffer<T>/ClockApi/FileApi yet) and what reconciles them.
#include "foundation/tl_log.h"

#include "foundation/tl_assert.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

struct LogRing {
    LogRecord slot[4096];   // overwrite-oldest; RingBuffer<T> (CONTAINERS.md) replaces this array
    u32 head;
    u32 count;
};
LogRing g_log = {};

const char* level_name(u8 level) {
    switch (level) {
        case LOG_TRACE: return "TRACE";
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        default:        return "ERR";
    }
}

}  // namespace

void tl_log_write(u8 level, const char* file, u32 line, const char* fmt, ...) {
    char buf[TL_LOG_MSG_CAP];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const u8 len = (u8)((n < 0) ? 0 : (n >= (int)sizeof(buf) ? (int)sizeof(buf) - 1 : n));

    LogRecord& r = g_log.slot[g_log.head];
    r.tick = 0;   // FRAME-LOOP.md's tick counter is not wired yet (TODO.md)
    r.file = file;
    r.line = line;
    r.level = level;
    r.len = len;
    r._pad0 = 0;
    memcpy(r.msg, buf, len);
    r.msg[len] = 0;
    g_log.head = (g_log.head + 1u) % 4096u;
    if (g_log.count < 4096u) { g_log.count += 1u; }

    fprintf(stderr, "%-5s %s:%u: %s\n", level_name(level), file, line, buf);
}

#if TL_DEV
u32 tl_log_ring_count(void) { return g_log.count; }
u32 tl_log_ring_head(void) { return g_log.head; }
const LogRecord* tl_log_ring_at(u32 slot) {
    TL_CHECK(slot < g_log.count);
    // Write order, not ring order. `&g_log.slot[slot]` agreed with the header's "in write order"
    // only until the ring wrapped, and the wrap test could not tell the two apart because it
    // scanned every slot rather than indexing one.
    const u32 base = (g_log.count == 4096u) ? g_log.head : 0u;
    return &g_log.slot[(base + slot) % 4096u];
}
void tl_log_test_reset(void) { g_log = LogRing{}; }
#endif
