#pragma once
// vmem_test_api.h - a real OS-backed VMemApi for foundation tests, hermetic to tests/.
// Spec: docs/PLATFORM.md §9.3 "vmem" (the same calls the platform lane will make); the seam
// takes fn-ptrs, so a test-owned table IS the seam working as designed - the tests do not wait
// on the platform lane. OS headers are legal here: tests/ is not src/ (docs/TESTING.md §8 R-2;
// the platform-include grep covers src/ only).
#include "foundation/vmem_api.h"
#include "foundation/vmem_arena.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static void* tv_reserve(void*, u64 bytes) {
    return VirtualAlloc(nullptr, (SIZE_T)bytes, MEM_RESERVE, PAGE_NOACCESS);
}
static ErrCode tv_commit(void*, void* base, u64 bytes) {
    return VirtualAlloc(base, (SIZE_T)bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr ? ERR_OK : ERR_MEM_OOM;
}
static ErrCode tv_decommit(void*, void* base, u64 bytes) {
    return VirtualFree(base, (SIZE_T)bytes, MEM_DECOMMIT) != 0 ? ERR_OK : ERR_MEM_BAD_ARG;
}
static void tv_release(void*, void* base, u64) {
    (void)VirtualFree(base, 0, MEM_RELEASE);
}
static u32 tv_page_size() {
    SYSTEM_INFO si; GetSystemInfo(&si);
    return (u32)si.dwPageSize;
}

#else
#include <sys/mman.h>
#include <unistd.h>

static void* tv_reserve(void*, u64 bytes) {
    void* p = mmap(nullptr, (size_t)bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
static ErrCode tv_commit(void*, void* base, u64 bytes) {
    return mprotect(base, (size_t)bytes, PROT_READ | PROT_WRITE) == 0 ? ERR_OK : ERR_MEM_OOM;
}
static ErrCode tv_decommit(void*, void* base, u64 bytes) {
    // DONTNEED discards private anonymous pages (they re-read as zero); PROT_NONE restores the
    // reserve-state protection so a stale read faults like it does on Windows.
    if (madvise(base, (size_t)bytes, MADV_DONTNEED) != 0) { return ERR_MEM_BAD_ARG; }
    return mprotect(base, (size_t)bytes, PROT_NONE) == 0 ? ERR_OK : ERR_MEM_BAD_ARG;
}
static void tv_release(void*, void* base, u64 bytes) {
    (void)munmap(base, (size_t)bytes);
}
static u32 tv_page_size() {
    return (u32)sysconf(_SC_PAGESIZE);
}
#endif

// The table under test-fixture ownership; call once per test (cheap, stateless).
static VMemApi test_vmem_api() {
    VMemApi api = {};
    api.ctx = nullptr;
    api.reserve = tv_reserve;
    api.commit = tv_commit;
    api.decommit = tv_decommit;
    api.release = tv_release;
    api.page_size = tv_page_size();
    api._pad0 = 0;
    return api;
}
