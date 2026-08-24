// os_posix_vmem.cpp - the POSIX half of the shared VMemApi (docs/PLATFORM.md §9.3 "vmem").
// Compiled only on non-Windows targets (src/platform/CMakeLists.txt); os_win_vmem.cpp is the
// other half.
#include "platform/os_vmem.h"
#include "platform/platform.h"   // ERR_PLATFORM_VMEM

#include "foundation/tl_assert.h"

#include <sys/mman.h>
#include <unistd.h>

namespace {

// No stored page size (no static state, docs/CPP-SUBSET.md §1) - queried fresh each call.
u32 query_page_size() {
    return (u32)sysconf(_SC_PAGESIZE);
}

void* ov_reserve(void*, u64 bytes) {
    void* p = mmap(nullptr, (size_t)bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

// base/bytes must be page multiples (docs/PLATFORM.md §9.3 "vmem": "the impl TL_CHECKs").
ErrCode ov_commit(void*, void* base, u64 bytes) {
    const u64 page = (u64)query_page_size();
    TL_CHECK(((u64)base) % page == 0u && bytes % page == 0u);
    return mprotect(base, (size_t)bytes, PROT_READ | PROT_WRITE) == 0 ? ERR_OK : (ErrCode)ERR_PLATFORM_VMEM;
}

// MADV_DONTNEED discards private anonymous pages (they re-read as OS-zero on next commit);
// PROT_NONE restores the reserve-state protection so a stale access faults, matching Windows'
// MEM_DECOMMIT behaviour (docs/PLATFORM.md §9.3).
ErrCode ov_decommit(void*, void* base, u64 bytes) {
    const u64 page = (u64)query_page_size();
    TL_CHECK(((u64)base) % page == 0u && bytes % page == 0u);
    if (madvise(base, (size_t)bytes, MADV_DONTNEED) != 0) { return (ErrCode)ERR_PLATFORM_VMEM; }
    return mprotect(base, (size_t)bytes, PROT_NONE) == 0 ? ERR_OK : (ErrCode)ERR_PLATFORM_VMEM;
}

// Same TL_CHECK as its Windows twin: munmap DOES use `bytes`, so a wrong extent here unmaps the
// wrong pages. The check lives on both sides so a Windows-only run cannot pass a value that only
// breaks on Linux (docs/PLATFORM.md §9.3 "vmem": "base/n must be page multiples (TL_CHECK)").
void ov_release(void*, void* base, u64 bytes) {
    const u64 page = (u64)query_page_size();
    TL_CHECK(((u64)base) % page == 0u && bytes % page == 0u);
    (void)munmap(base, (size_t)bytes);
}

}  // namespace

void os_vmem_fill_table(VMemApi* out) {
    TL_ASSERT(out != nullptr);
    out->ctx = nullptr;
    out->reserve = ov_reserve;
    out->commit = ov_commit;
    out->decommit = ov_decommit;
    out->release = ov_release;
    out->page_size = (u32)sysconf(_SC_PAGESIZE);
    out->_pad0 = 0;
}
