#pragma once
// ---------------------------------------------------------------------------------------------
// tl_log.h - TL_LOG_{TRACE,DEBUG,INFO,WARN,ERR}, the ring + stderr sink.
//
// Spec: docs/TOOLING.md §4, §9.1 (macro text), §9.2 (LogRecord/LogState); docs/CPP-SUBSET.md §7b
//   (tier table), §9 R-4 (RR-7 - the tooling-plane io/state exemption this file relies on).
// Purpose: every level macro formats through `tl_log_write` into a fixed ring (overwrite-oldest,
//   4096 slots) and stderr. The file sink and the tick-stamped `tick` field wait for
//   `PlatformApi.file.append` and `FRAME-LOOP.md`'s tick counter respectively - both `TODO.md`
//   items, not built here; `LogRecord.tick` reads 0 until that lane wires it.
// Invariants: a level below the tier's `TL_LOG_MIN` compiles to `((void)0)` - the argument list is
//   never evaluated, so a call site with a side effect in its arguments is a bug (the two tiers
//   would run different programs). `TL_LOG_ERR` is never compiled out.
// Determinism: never hashed, never snapshotted, never part of a world's registered arena
//   (docs/CPP-SUBSET.md §9 R-4) - a log line records but never feeds sim state.
// Threading: `tl_log_write` is not synchronized; today's only caller is the main thread (the
//   panic path and `TL_LOG_*` call sites). A worker-safe ring is `JOBS.md`'s job when parallel
//   systems land.
// Includes: foundation/tl_types.h only.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

enum LogLevel : u8 { LOG_TRACE = 0, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERR };

enum { TL_LOG_MSG_CAP = 240 };   // ring/stderr message capacity, including the NUL

// One ring slot. Simplified from docs/TOOLING.md §9.2's LogRecord (drops wall_ms/ClockApi, which
// wait for PLATFORM.md) - reconciled once that lane lands (TODO.md).
struct LogRecord {
    u64 tick;       // FRAME-LOOP.md's tick counter; 0 until that lane wires tick_ptr (TODO.md)
    const char* file;
    u32 line;
    u8 level;
    u8 len;         // bytes used in msg, excluding the NUL
    u16 _pad0;
    char msg[TL_LOG_MSG_CAP];
};

// Formats `fmt`/varargs (stb_sprintf once vendored - TODO.md; `vsnprintf` today, non-det plane
// only, never hashed) into a ring slot and stderr. Runs at any level the tier compiled in;
// `TL_LOG_ERR` always reaches here. May run inside a tick - it touches only the tooling plane's
// own state, never sim state.
extern "C" void tl_log_write(u8 level, const char* file, u32 line, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

#if TL_LOG_MIN <= 0
#  define TL_LOG_TRACE(...) tl_log_write(LOG_TRACE, __FILE__, (u32)__LINE__, __VA_ARGS__)
#else
#  define TL_LOG_TRACE(...) ((void)0)
#endif
#if TL_LOG_MIN <= 1
#  define TL_LOG_DEBUG(...) tl_log_write(LOG_DEBUG, __FILE__, (u32)__LINE__, __VA_ARGS__)
#else
#  define TL_LOG_DEBUG(...) ((void)0)
#endif
#if TL_LOG_MIN <= 2
#  define TL_LOG_INFO(...) tl_log_write(LOG_INFO, __FILE__, (u32)__LINE__, __VA_ARGS__)
#else
#  define TL_LOG_INFO(...) ((void)0)
#endif
#if TL_LOG_MIN <= 3
#  define TL_LOG_WARN(...) tl_log_write(LOG_WARN, __FILE__, (u32)__LINE__, __VA_ARGS__)
#else
#  define TL_LOG_WARN(...) ((void)0)
#endif
// Always on (docs/CANON.md "Cvars"/TOOLING.md §4 macro table: "TL_LOG_ERR always on").
#define TL_LOG_ERR(...) tl_log_write(LOG_ERR, __FILE__, (u32)__LINE__, __VA_ARGS__)

#if TL_DEV
// Test-only introspection into the ring (docs/TESTING.md's runner links against these; never
// called from src/ outside tests). Guarded by TL_DEV so no symbol exists in netcode/ship.
// Number of live records, capped at 4096 once the ring has wrapped at least once.
u32 tl_log_test_ring_count(void);
// Index the NEXT write lands at (the oldest live record once the ring has wrapped).
u32 tl_log_test_ring_head(void);
// Record at `slot`, in write order. Precondition: slot < tl_log_test_ring_count().
const LogRecord* tl_log_test_ring_at(u32 slot);
// Clears the ring to its zero-initialised state. Tests only - never called from a live path.
void tl_log_test_reset(void);
#endif
