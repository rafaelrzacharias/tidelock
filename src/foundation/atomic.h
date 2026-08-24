#pragma once
// ---------------------------------------------------------------------------------------------
// atomic.h - the compiler's atomic builtins, wrapped. NON-DET: never reachable from sim code.
//
// Spec: docs/JOBS.md §6.1 (this header's owner); docs/PLATFORM.md §9.2 lists the same names
//   because threads are that seam; docs/CPP-SUBSET.md §1 (no <atomic>: it is STL, and `volatile`
//   is not an atomic).
// Purpose: the two counters and the epoch docs/JOBS.md §6.3's pool runs on, and nothing else.
//   Every verb is a `static inline` over a `__atomic_*_n` builtin with a FIXED memory order -
//   ACQUIRE on loads, RELEASE on stores, ACQ_REL on read-modify-write - so a call site cannot
//   weaken one by passing the wrong constant. docs/JOBS.md §6.3's pseudocode spells the chunk
//   counter RELAXED; this API gives it ACQ_REL, which is stronger, correct, and free on x86-64.
// Invariants: `p` must be NATURALLY aligned (4 bytes for the 32-bit verbs, 8 for the 64-bit
//   ones) - a misaligned atomic is UB, and on aarch64 it is a fault, not a slow path. Every
//   holder of one of these is expected to static_assert its own offsets (docs/JOBS.md §6.2 does).
// Determinism: NOTHING here may feed sim-visible ordering. An atomic decides only WHICH worker
//   runs a chunk; results are keyed by chunk id (docs/JOBS.md §0, docs/DETERMINISM.md §2 rule 5).
//   The #error below is the enforcement, not a convention: tools/audit/allow.txt names
//   `__aarch64_*` (outline atomics) as the tripwire for atomics inside det code, but on x86-64 a
//   32-bit fetch-add inlines to `lock xadd` and emits NO symbol at all, so that tripwire exists
//   only on the Pi leg - where the symbol audit does not run. A compile error runs everywhere.
// Threading: this IS the threading primitive; every verb is safe from any thread by definition.
// Includes: foundation/tl_types.h.
// ---------------------------------------------------------------------------------------------
#if defined(TL_SIM_TU)
#error "foundation/atomic.h is non-det (docs/JOBS.md section 6.1) and must not be reached from a sim or det TU: an atomic in det code is worker-observable ordering (docs/DETERMINISM.md section 2 rule 5) and pulls aarch64 outline atomics past tools/audit/allow.txt. If a det TU needs this, that is a ruling, not an include."
#endif

#include "foundation/tl_types.h"

// Reads *p with ACQUIRE ordering: everything the releasing writer did before its store is
// visible after this returns. `p` must be 4-byte aligned.
static inline u32 atomic_load32(const u32* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
// As atomic_load32, 64-bit. `p` must be 8-byte aligned.
static inline u64 atomic_load64(const u64* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

// Writes v to *p with RELEASE ordering: every write this thread made before it is visible to a
// thread that acquire-loads *p afterwards. `p` must be 4-byte aligned.
static inline void atomic_store32(u32* p, u32 v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
// As atomic_store32, 64-bit. `p` must be 8-byte aligned.
static inline void atomic_store64(u64* p, u64 v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

// *p += v, ACQ_REL. Returns the PREVIOUS value - the claim ticket docs/JOBS.md §6.3 runs on.
// Wraps on overflow (u32 arithmetic is modular; that is defined, not UB). 4-byte aligned.
static inline u32 atomic_add32(u32* p, u32 v) { return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL); }
// As atomic_add32, 64-bit. 8-byte aligned.
static inline u64 atomic_add64(u64* p, u64 v) { return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL); }

// *p -= v, ACQ_REL. Returns the PREVIOUS value, so `atomic_sub32(&n, 1) == 1` is exactly "I took
// the countdown to zero" - the one caller that must act. Wraps on underflow. 4-byte aligned.
static inline u32 atomic_sub32(u32* p, u32 v) { return __atomic_fetch_sub(p, v, __ATOMIC_ACQ_REL); }
// As atomic_sub32, 64-bit. 8-byte aligned.
static inline u64 atomic_sub64(u64* p, u64 v) { return __atomic_fetch_sub(p, v, __ATOMIC_ACQ_REL); }

// Strong compare-and-swap: if *p == expect, store desire and return 1; else return 0. ACQ_REL on
// success, ACQUIRE on failure (a failure order may not be RELEASE). The observed value is NOT
// reported on failure - by design, since `expect` is taken by value; re-load and retry.
// Never spuriously fails (weak = false). 4-byte aligned.
static inline u8 atomic_cas32(u32* p, u32 expect, u32 desire) {
    return (u8)__atomic_compare_exchange_n(p, &expect, desire, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
// As atomic_cas32, 64-bit. 8-byte aligned.
static inline u8 atomic_cas64(u64* p, u64 expect, u64 desire) {
    return (u8)__atomic_compare_exchange_n(p, &expect, desire, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

// Stores v into *p and returns the previous value, ACQ_REL. 4-byte aligned.
static inline u32 atomic_exchange32(u32* p, u32 v) { return __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL); }
// As atomic_exchange32, 64-bit. 8-byte aligned.
static inline u64 atomic_exchange64(u64* p, u64 v) { return __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL); }

// A standalone ACQUIRE fence: no load or store after it may be reordered before a prior load.
static inline void atomic_fence_acquire() { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
// A standalone RELEASE fence: no prior load or store may be reordered after it.
static inline void atomic_fence_release() { __atomic_thread_fence(__ATOMIC_RELEASE); }
// A standalone sequentially-consistent fence: a single total order across every SEQ_CST op.
static inline void atomic_fence_seq_cst() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
