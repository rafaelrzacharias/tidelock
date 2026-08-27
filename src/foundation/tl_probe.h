#pragma once
// ---------------------------------------------------------------------------------------------
// tl_probe.h - TL_PROBE_{LOG,LOG_FX,ON_CHANGE,MARK,ASSERT}, the tick-throttled TSV sink.
//
// Spec: docs/TOOLING.md §5, §9.1 (macro text, verbatim), §9.3.3 (the throttle algorithm);
//   docs/CPP-SUBSET.md §7b (tier table), §9 R-4 (RR-7 - the tooling-plane exemption probe.cpp
//   relies on).
// Purpose: integer-only at the call site (a `fx` row's raw bits + its FRAC, never a float) so a
//   sim TU stays float-free even though it calls into probe.cpp, which does the fixed->f64
//   conversion for the TSV row (docs/FX-PALETTE.md's float bridge equivalent, scoped to probe).
// Invariants: outside `TL_DEV` every macro compiles to `((void)0)`, the argument list unevaluated.
//   `tl_probe_log`'s real signature takes a plain `u64` key today, not `NameHash`
//   (`foundation/hash.h`, not yet built) - the macros below are transcribed from `TOOLING.md`
//   §9.1 verbatim and need `"lit"_id` (the `NameHash` literal) to compile at a call site, so they
//   are not yet callable; probe.cpp's functions are, directly, with a caller-computed key
//   (`tests/foundation/tl_probe.test.cpp` does exactly this). Reconciled the day hash.h lands
//   (`TODO.md`) - `NameHash` is `u64` (`CANON.md`), so nothing about probe.cpp's logic changes.
// Determinism: never hashed, never snapshotted (docs/CPP-SUBSET.md §9 R-4). Throttled by TICK
//   COUNT only, never wall-clock (docs/TOOLING.md §5) - `probe.cpp`'s `tick` reads 0 until
//   `FRAME-LOOP.md`'s tick counter is wired in (`TODO.md`; the same simplification `tl_log.h`
//   documents). With the tick pinned at 0 the throttle is NOT a no-op and does not degrade to
//   "every call": the first call rows, and every later call with `n > 0` is suppressed forever,
//   because no tick ever advances past `last_tick`. `n == 0` rows every call. Tests drive it
//   through `tl_probe_test_set_tick`; the algorithm starts working unchanged the day a real tick
//   feeds it. A tick that moves BACKWARDS (a replay scrub seek, docs/TOOLING.md §9.3.10) throttles
//   rather than wrapping the u64 difference into an enormous gap.
// Threading: none - the probe key table is not worker-partitioned (unlike the profiler); a
//   parallel caller is `JOBS.md`'s job when parallel systems land.
// Includes: foundation/tl_types.h only.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

#if TL_DEV
#  define TL_PROBE_LOG(lit, v, n)         tl_probe_log(lit##_id, lit, (i64)(v), 0, (u32)(n))
#  define TL_PROBE_LOG_FX(lit, fx, n)     tl_probe_log(lit##_id, lit, (i64)(fx).v, (u8)decltype(fx)::FRAC, (u32)(n))
#  define TL_PROBE_ON_CHANGE(lit, v, eps) tl_probe_on_change(lit##_id, lit, (i64)(v), (i64)(eps))
#  define TL_PROBE_MARK(lit)              tl_probe_mark(lit##_id, lit)
#  define TL_PROBE_ASSERT(lit, v, lo, hi) tl_probe_assert(lit##_id, lit, (i64)(v), (i64)(lo), (i64)(hi))
#else
#  define TL_PROBE_LOG(lit, v, n)         ((void)0)
#  define TL_PROBE_LOG_FX(lit, fx, n)     ((void)0)
#  define TL_PROBE_ON_CHANGE(lit, v, eps) ((void)0)
#  define TL_PROBE_MARK(lit)              ((void)0)
#  define TL_PROBE_ASSERT(lit, v, lo, hi) ((void)0)
#endif

enum ProbeKind : u8 { PROBE_LOG = 0, PROBE_ON_CHANGE, PROBE_MARK, PROBE_ASSERT };

// One probe's running stats (docs/TOOLING.md §9.2's ProbeKey, `NameHash` read as `u64` - see this
// header's Invariants note). `frac_bits` is the fx row's FRAC (0 for a plain integer probe).
struct ProbeKey {
    u64 key;
    const char* name;
    u64 count, changes;
    f64 min, max, sum, first, last;
    i64 last_raw;
    u64 last_tick;
    u8 enabled, frac_bits, kind;
};

// Registers (or finds) `key`, throttled by tick count: a row is emitted iff this is the first
// call or at least `n` ticks passed since the last one. Disabled keys (`enabled == 0`) are one
// branch - no stats update, no row. `frac` is the fx row's FRAC bit count (0 = plain integer).
void tl_probe_log(u64 key, const char* name, i64 raw, u8 frac, u32 n);
// A row whenever `|raw - last_raw| > eps`, or on the key's first call. Fatal if `eps < 0`.
void tl_probe_on_change(u64 key, const char* name, i64 raw, i64 eps);
// A row on every call, unconditionally.
void tl_probe_mark(u64 key, const char* name);
// A row plus `TL_LOG_ERR` when `raw` falls outside `[lo, hi]`; fatal when the key is enabled AND
// out of range AND the (not-yet-built) `probe_assert_fatal` cvar default would say so - today
// this always logs and never escalates to fatal (the cvar table is core/, not built - `TODO.md`).
void tl_probe_assert(u64 key, const char* name, i64 raw, i64 lo, i64 hi);

// Appends the "#summary" line and one row per key, registration order (docs/TOOLING.md §9.2).
// Called at shutdown once something owns that hook (`app/`, not built yet); tests call it directly.
void tl_probe_write_summary(void);

#if TL_DEV
// Key introspection (guarded by TL_DEV). Originally test-only; promoted to a real production API
// the day editor/probes_panel.cpp became its first non-test caller (docs/TOOLING.md §9.6 build
// order item 5), matching tl_log.h's tl_log_ring_count/_at and tl_prof.h's tl_prof_ring_count/_at
// promotions the day their own first panel needed them. Every other accessor below this pair
// stays test-scoped: nothing on a live path formats the TSV staging buffer, rewinds the tick, or
// force-sets a key's enabled flag outside a test (the real toggle is console/cvar-routed,
// TOOLING.md §3 - not yet wired to a probe command, TODO.md; the Probes panel is read-only at v0).
// Number of registered keys, in registration order.
u32 tl_probe_key_count(void);
// Key at `slot`, in registration order. Fatal if slot >= tl_probe_key_count().
const ProbeKey* tl_probe_key_at(u32 slot);
// Test-only introspection (docs/TESTING.md's runner links against these). Guarded by TL_DEV.
// The formatted TSV rows written so far (docs/TOOLING.md §9.2's `staging` buffer), NUL-terminated.
const char* tl_probe_test_staging(void);
// Clears every key and the staging buffer to their zero-initialised state.
void tl_probe_test_reset(void);
// Sets the tick probe.cpp stamps rows with. FRAME-LOOP.md's real tick counter is not wired yet
// (this header's Determinism note) - tests use this to exercise the throttle algorithm today.
void tl_probe_test_set_tick(u64 tick);
// Registers `key` if new, then sets its enabled flag. The real toggle is console/cvar-routed
// (`TOOLING.md` §3, `editor/console.cpp` + `core/cvar.h` - built, but not yet wired to a probe
// toggle command) - tests use this directly until that wiring lands.
void tl_probe_test_set_enabled(u64 key, const char* name, u8 enabled);
#endif
