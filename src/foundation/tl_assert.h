#pragma once
// ---------------------------------------------------------------------------------------------
// tl_assert.h - the three assertion tiers and the panic ABI.
//
// Spec: docs/TOOLING.md §9 (the macro text, verbatim), docs/CPP-SUBSET.md §3 (the error model),
//   §7b (tier behaviour) and §9 R-3 (the panic ABI - the ONE sanctioned callgraph out of sim code).
// Purpose: TL_ASSERT (debug/dev only), TL_CHECK (every tier, slim), TL_FATAL (every tier). All
//   three resolve to the closed symbol set {tl_fatal, tl_check_failed, tl_assert_failed}, which
//   tools/audit/allow.txt allowlists by name; they are defined in tl_foundation (the non-det
//   half) and only referenced from the audited libs.
// Invariants: a failed check terminates the process - never returned, never swallowed. Because
//   the panic path ends the process it never executes inside a deterministic tick; if it ever
//   does, determinism has already ended (docs/CPP-SUBSET.md §9 R-3).
// Determinism: __FILE__/__LINE__ are deterministic given the tree and never feed sim state
//   (docs/CPP-SUBSET.md §5). TL_ASSERT compiles to ((void)0) in netcode/ship, so a condition with
//   side effects is a bug: the two tiers would run different programs.
// Threading: none - header-only. The runtime (tl_assert.cpp) logs then calls the crash-writer
//   seam (foundation/crash.h, docs/TOOLING.md §9.3.9); the seam's built-in fallback prints the
//   stderr marker docs/TESTING.md §9.1's fatal-expected tests match on and exits(2), until
//   platform/ installs the real OS-level writer.
// Includes: foundation/tl_types.h only.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

// The panic ABI (docs/CPP-SUBSET.md §9 R-3): extern "C" so the symbol audit can match the
// names. `msg` is a message literal - `const char*` is the sanctioned spelling for those
// (docs/CPP-SUBSET.md §5) and the only place the token appears in a sim TU. Every one of the
// three writes the crash report and aborts; none returns.
extern "C" {
// Unconditional fatal: prints file:line + msg, writes the crash report, aborts. Never returns.
[[noreturn]] void tl_fatal(const char* file, u32 line, const char* msg);
// A failed TL_CHECK (all tiers). Same path as tl_fatal; the name tells the report which tier fired.
[[noreturn]] void tl_check_failed(const char* file, u32 line, const char* expr);
// A failed TL_ASSERT (debug/dev tiers only). Same path as tl_fatal.
[[noreturn]] void tl_assert_failed(const char* file, u32 line, const char* expr);
}

// docs/TOOLING.md §9, with each tier routed to its own R-3 symbol so the crash report names the
// tier that fired. TL_FATAL takes a message literal; TL_CHECK/TL_ASSERT stringise the condition.
// All three are void expressions, so they are legal inside constexpr functions: a failing
// condition under constant evaluation is a compile error (the non-constexpr call is reached),
// which is the static range check fx_int/fx_lit rely on.
#define TL_FATAL(msg)   tl_fatal(__FILE__, (u32)__LINE__, (msg))
#define TL_CHECK(c)     ((c) ? (void)0 : tl_check_failed(__FILE__, (u32)__LINE__, #c))
#if TL_DEV
#  define TL_ASSERT(c)  ((c) ? (void)0 : tl_assert_failed(__FILE__, (u32)__LINE__, #c))
#else
#  define TL_ASSERT(c)  ((void)0)
#endif
