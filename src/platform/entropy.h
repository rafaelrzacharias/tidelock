#pragma once
// ---------------------------------------------------------------------------------------------
// entropy.h - EntropyApi: the one door to OS randomness, deliberately narrow.
//
// Spec: docs/PLATFORM.md §5; docs/DETERMINISM.md §2 (entropy never reaches a sim path).
// Purpose: `fill(buf, n)` over BCryptGenRandom / getrandom(2), for Monocypher keygen, session
//   nonces and commit/reveal only - never sim state, never gameplay RNG (that is the keyed
//   `rng_for` of docs/DETERMINISM.md §3).
// Invariants: failure is TL_FATAL - there is no fallback to a weaker source. Probed once at
//   platform init with a 32-byte fill (docs/PLATFORM.md §9.5 step 7).
// Determinism: this header is included ONLY from `src/net/` and `src/app/` (enforced by CI grep,
//   docs/PLATFORM.md §5) - it is absent from every sim lib's include path, and the symbol audit
//   fails on `BCryptGenRandom`/`getrandom` reachable from an audited lib. `platform.h` itself
//   never includes this header: it forward-declares `struct EntropyApi;` and holds only a
//   `const EntropyApi*`, so pulling in the contract never exposes the verb (docs/PLATFORM.md §9.2).
// Threading: `fill` may be called from any thread; the OS calls it wraps are thread-safe.
// Includes: foundation/tl_types.h only.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

struct EntropyApi {
    void* ctx;
    // Fills buf[0..n) with OS-sourced random bytes. TL_FATAL on failure - never a weak fallback.
    void (*fill)(void* ctx, void* buf, u32 n);
};
