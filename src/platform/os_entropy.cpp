// os_entropy.cpp - the shared EntropyApi (docs/PLATFORM.md §5, §9.3 "entropy"). One TU, both
// OSes: BCryptGenRandom on Windows, getrandom(2) on POSIX. Failure is TL_FATAL - no weak fallback.
#include "platform/os_entropy.h"

#include "foundation/tl_assert.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
// NOMINMAX before EVERY <windows.h> in the tree (ruled 2026-08-24, TODO.md R6; checked by
// tools/audit/includes.py). windows.h's raw min/max macros mangle fx.h's free functions of
// the same name in any TU that reaches both, and the failure reads as "too many arguments
// to function-like macro invocation" on an fx declaration, not as a min/max collision.
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>
#include <errno.h>
#endif

namespace {

void oe_fill(void*, void* buf, u32 n) {
#ifdef _WIN32
    NTSTATUS st = BCryptGenRandom(nullptr, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st != 0 /* STATUS_SUCCESS */) {
        TL_FATAL("entropy unavailable (BCryptGenRandom)");
    }
#else
    u8* p = (u8*)buf;
    u32 left = n;
    while (left > 0u) {
        ssize_t got = getrandom(p, (size_t)left, 0);
        if (got < 0) {
            if (errno == EINTR) { continue; }
            TL_FATAL("entropy unavailable (getrandom)");
        }
        p += got;
        left -= (u32)got;
    }
#endif
}

}  // namespace

void os_entropy_fill_table(EntropyApi* out) {
    TL_ASSERT(out != nullptr);
    out->ctx = nullptr;
    out->fill = oe_fill;
}
