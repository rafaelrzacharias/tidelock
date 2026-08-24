#pragma once
// ---------------------------------------------------------------------------------------------
// crash.h - the boot-installed seam the panic ABI terminates through.
//
// Spec: docs/TOOLING.md §9.3.9 (the crash pipeline), §9.1's file table ("crash" stem); the seam
//   itself is docs/CPP-SUBSET.md §9 R-4 (RR-7) - the tooling plane's one exception to R-3's
//   rejection of a boot-installed function pointer, because here it carries none of the reason
//   that rejection was made (never hashed, never snapshotted, not part of a registered arena).
// Purpose: `tl_crash_raise` is what `tl_fatal`/`tl_check_failed`/`tl_assert_failed` (tl_assert.cpp)
//   call to end the process. Until `platform/` exists to install the real OS-level writer
//   (SEH/signal handler, MiniDumpWriteDump, TOOLING.md §9.3.9), the built-in fallback here prints
//   the stderr marker docs/TESTING.md §9.1's fatal-expected tests match on and exits(2).
// Invariants: `tl_crash_raise` never returns. The marker line always starts with the literal
//   token "TL_FATAL", regardless of which of the three panic symbols fired - `origin` names the
//   symbol for a human, the fixed prefix is what the runner's child-process check greps for.
// Determinism: this plane is never hashed, snapshotted, or read back into a tick - the panic path
//   terminates the process, so it never runs inside a deterministic tick to begin with.
// Threading: `tl_crash_install` is boot-only (before any worker starts); `tl_crash_raise` may be
//   called from any thread, since a fatal ends the whole process regardless of which one called
//   it. Note the gap this opens with the step before it: `tl_assert.cpp` logs first, and
//   `foundation/tl_log.h`'s ring is unsynchronized, so a fatal raised off the main thread races
//   that ring the day `JOBS.md` starts a worker (`TODO.md`). No worker exists today.
// Includes: foundation/tl_types.h only.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

// Why a report is being written. SEH/SIGNAL are reserved for platform/'s installed handlers
// (docs/TOOLING.md §9.3.9); nothing in foundation/ raises them yet.
enum CrashReason : u8 { CRASH_FATAL = 1, CRASH_SEH = 2, CRASH_SIGNAL = 3 };

// The real writer platform/ installs once it exists (raw OS file write, SEH/signal tail,
// TOOLING.md §9.3.9). Called instead of the built-in fallback when non-null. May itself choose
// not to return (it is expected to terminate the process one way or another).
using CrashInstallFn = void (*)(u8 reason, const char* origin, const char* file, u32 line, const char* msg);

// Installs the real crash writer. Boot-only: called once from app/ wiring before any tick runs;
// calling it a second time silently replaces the previous installation (there is exactly one
// legitimate caller in the whole program, so this is not guarded further).
void tl_crash_install(CrashInstallFn fn);

// Writes a crash report (the installed writer if one exists, else the built-in stderr fallback)
// and terminates the process with exit code 2. Never returns. `origin` is one of "TL_FATAL",
// "TL_CHECK", "TL_ASSERT" (docs/CPP-SUBSET.md §9 R-3's three panic symbols) - purely diagnostic,
// never parsed by the fallback path itself.
[[noreturn]] void tl_crash_raise(u8 reason, const char* origin, const char* file, u32 line, const char* msg);
