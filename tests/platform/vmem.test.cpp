// vmem.test.cpp - docs/PLATFORM.md §9.6 vmem_reserve_commit, vmem_page_size.
#include "runner/tl_test.h"
#include "platform_test_util.h"

TL_TEST(vmem_page_size, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const u32 page = api->vmem.page_size;
    TL_EXPECT_GE(page, 4096u);
    TL_EXPECT_EQ(page & (page - 1u), 0u);   // power of two
    platform_test_shutdown(api);
}

TL_TEST(vmem_reserve_commit, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const VMemApi& vm = api->vmem;
    const u64 GB = 1ull << 30;

    void* base = vm.reserve(vm.ctx, GB);
    TL_ASSERT_NOT_NULL(base);

    // commit the first page and the last page
    const u64 page = (u64)vm.page_size;
    TL_EXPECT_EQ(vm.commit(vm.ctx, base, page), ERR_OK);
    u8* last_page = (u8*)base + (GB - page);
    TL_EXPECT_EQ(vm.commit(vm.ctx, last_page, page), ERR_OK);

    // every byte of a committed page reads 0
    u8* first = (u8*)base;
    bool all_zero = true;
    for (u64 i = 0; i < page; ++i) { if (first[i] != 0u) { all_zero = false; break; } }
    TL_EXPECT_TRUE(all_zero);

    // write, decommit, re-commit -> reads 0 again
    first[0] = 0xAB; first[page - 1u] = 0xCD;
    TL_EXPECT_EQ(vm.decommit(vm.ctx, base, page), ERR_OK);
    TL_EXPECT_EQ(vm.commit(vm.ctx, base, page), ERR_OK);
    all_zero = true;
    for (u64 i = 0; i < page; ++i) { if (first[i] != 0u) { all_zero = false; break; } }
    TL_EXPECT_TRUE(all_zero);

    // release then reserve again at any address
    vm.release(vm.ctx, base, GB);
    void* base2 = vm.reserve(vm.ctx, GB);
    TL_EXPECT_NOT_NULL(base2);
    if (base2 != nullptr) { vm.release(vm.ctx, base2, GB); }

    platform_test_shutdown(api);
}

TL_TEST_EXPECT_FATAL(vmem_commit_non_page_multiple_is_fatal, "platform,slow") {
    const PlatformApi* api = platform_test_init();
    const VMemApi& vm = api->vmem;
    void* base = vm.reserve(vm.ctx, 1ull << 20);
    (void)t;
    (void)vm.commit(vm.ctx, base, 1u);   // 1 byte: never a page multiple - TL_CHECK fatal
}
