// os_win_vmem.cpp - the Windows half of the shared VMemApi (docs/PLATFORM.md §9.3 "vmem").
// Compiled only on Windows (src/platform/CMakeLists.txt); os_posix_vmem.cpp is the other half.
#include "platform/os_vmem.h"
#include "platform/platform.h"   // ERR_PLATFORM_VMEM

#include "foundation/tl_assert.h"

#define WIN32_LEAN_AND_MEAN
// NOMINMAX before EVERY <windows.h> in the tree (ruled 2026-08-24, TODO.md R6; checked by
// tools/audit/includes.py). windows.h's raw min/max macros mangle fx.h's free functions of
// the same name in any TU that reaches both, and the failure reads as "too many arguments
// to function-like macro invocation" on an fx declaration, not as a min/max collision.
#define NOMINMAX
#include <windows.h>

namespace {

// No stored page size (no static state, docs/CPP-SUBSET.md §1) - queried fresh each call. Not a
// hot path: commit/decommit happen once per COMMIT_GRANULE, not per allocation.
u32 query_page_size() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (u32)si.dwPageSize;
}

void* ov_reserve(void*, u64 bytes) {
    return VirtualAlloc(nullptr, (SIZE_T)bytes, MEM_RESERVE, PAGE_NOACCESS);
}

// base/bytes must be page multiples (docs/PLATFORM.md §9.3 "vmem": "the impl TL_CHECKs").
ErrCode ov_commit(void*, void* base, u64 bytes) {
    const u64 page = (u64)query_page_size();
    TL_CHECK(((u64)base) % page == 0u && bytes % page == 0u);
    return VirtualAlloc(base, (SIZE_T)bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr
               ? ERR_OK : (ErrCode)ERR_PLATFORM_VMEM;
}

// decommit returns the pages but keeps the reservation - matches VirtualFree(MEM_DECOMMIT), not
// MEM_RELEASE (docs/PLATFORM.md §9.3).
ErrCode ov_decommit(void*, void* base, u64 bytes) {
    const u64 page = (u64)query_page_size();
    TL_CHECK(((u64)base) % page == 0u && bytes % page == 0u);
    return VirtualFree(base, (SIZE_T)bytes, MEM_DECOMMIT) != 0 ? ERR_OK : (ErrCode)ERR_PLATFORM_VMEM;
}

// MEM_RELEASE takes size 0 by Win32 contract, so `bytes` is unused HERE - which is exactly why
// it is checked here: munmap on the POSIX side does use it, and a caller that passes a wrong
// `bytes` would unmap the wrong extent there while this side silently succeeded. The asymmetry
// is invisible on a Windows-only run, and the POSIX half of this seam has never executed.
void ov_release(void*, void* base, u64 bytes) {
    const u64 page = (u64)query_page_size();
    TL_CHECK(((u64)base) % page == 0u && bytes % page == 0u);
    (void)VirtualFree(base, 0, MEM_RELEASE);
}

}  // namespace

void os_vmem_fill_table(VMemApi* out) {
    TL_ASSERT(out != nullptr);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    out->ctx = nullptr;
    out->reserve = ov_reserve;
    out->commit = ov_commit;
    out->decommit = ov_decommit;
    out->release = ov_release;
    out->page_size = (u32)si.dwPageSize;
    out->_pad0 = 0;
}
