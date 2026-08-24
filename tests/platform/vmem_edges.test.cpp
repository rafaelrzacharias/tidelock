// vmem_edges.test.cpp - the VMemApi edge matrix §9.6's vmem_reserve_commit row does not reach.
//
// That row covers the happy path plus the non-page-multiple fatal. It never asks what a reserve of
// 0 does, what a commit past the end of a reservation does, or whether `release`'s `bytes`
// argument is checked at all - and the last of those is the dangerous one on this seam:
// VirtualFree(MEM_RELEASE) ignores `bytes` by Win32 contract while munmap uses it, so a caller
// passing the wrong extent succeeds on Windows and unmaps the wrong pages on Linux. The POSIX half
// of os_*_vmem.cpp has never executed on any machine (no Linux host in this lane); an asymmetry
// that only Linux can observe is exactly what a Windows-only run will ship.
#include "runner/tl_test.h"
#include "platform_test_util.h"

TL_TEST(vmem_reserve_zero_is_refused, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const VMemApi& vm = api->vmem;

    // Both OSes refuse it (VirtualAlloc returns null; mmap returns MAP_FAILED with EINVAL), which
    // is the answer vmem_arena_init's ERR_MEM_BAD_ARG already assumes it will get.
    TL_EXPECT_NULL(vm.reserve(vm.ctx, 0u));

    platform_test_shutdown(api);
}

TL_TEST(vmem_commit_past_reserve_is_an_error_not_a_fatal, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const VMemApi& vm = api->vmem;
    const u64 page = (u64)vm.page_size;
    const u64 span = 64u * 1024u;

    void* base = vm.reserve(vm.ctx, span);
    TL_ASSERT_NOT_NULL(base);

    // Page-aligned and page-sized, so the TL_CHECK is satisfied - the extent is simply outside the
    // reservation. That is an OS refusal (ERR_PLATFORM_VMEM), not a contract violation: the arena
    // is the layer that turns an over-budget push into a TL_FATAL (docs/MEMORY.md §1.1), and it
    // can only do that if the table below it reports rather than dies.
    TL_EXPECT_EQ(vm.commit(vm.ctx, (u8*)base + span, page), (ErrCode)ERR_PLATFORM_VMEM);
    // The last legal page still commits afterwards - the failed call left nothing broken.
    TL_EXPECT_EQ(vm.commit(vm.ctx, (u8*)base + span - page, page), ERR_OK);

    vm.release(vm.ctx, base, span);
    platform_test_shutdown(api);
}

TL_TEST_EXPECT_FATAL(vmem_release_non_page_multiple_is_fatal, "platform,slow") {
    (void)t;
    const PlatformApi* api = platform_test_init();
    const VMemApi& vm = api->vmem;
    void* base = vm.reserve(vm.ctx, 64u * 1024u);
    // `bytes` is unused by VirtualFree(MEM_RELEASE) and load-bearing for munmap. Checked on BOTH,
    // so a Windows-only run cannot ship a value that unmaps the wrong extent on Linux.
    vm.release(vm.ctx, base, 1u);
}

TL_TEST(read_all_of_a_file_that_is_not_a_file, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const FileApi& fa = api->file;

    // A directory is not FILE_NOT_FOUND and not readable either; both OSes must land on an error
    // rather than on a span of garbage. (Windows CreateFileW refuses to open a directory without
    // FILE_FLAG_BACKUP_SEMANTICS; POSIX open() succeeds and read() then fails with EISDIR - two
    // routes, one required outcome.)
    Result<Span<u8>> r = fa.read_all(fa.ctx, sv("."), nullptr);
    TL_EXPECT_NE(r.err, ERR_OK);

    platform_test_shutdown(api);
}
