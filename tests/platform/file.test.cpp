// file.test.cpp - docs/PLATFORM.md §9.6 read_all_contract, enumerate_sorted.
#include "runner/tl_test.h"
#include "platform_test_util.h"
#include "foundation/vmem_arena.h"
#include "foundation/hash.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace {

// tests/ carries the filesystem exemption of docs/TESTING.md §8 R-2. A fixed relative directory,
// cleaned up at the end of each test - same convention as tl_assert.test.cpp's probe files.
const char* TEST_DIR = "tl_platform_file_test_tmp";

StrView sv_c(const char* s) { return StrView{ s, (u32)strlen(s) }; }

void raw_write_file(TestCtx* t, const char* path, const void* data, usize n) {
    FILE* f = fopen(path, "wb");
    TL_ASSERT_TRUE(f != nullptr);
    fwrite(data, 1, n, f);
    fclose(f);
}

}  // namespace

TL_TEST(read_all_contract, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const FileApi& fa = api->file;

    // missing -> FILE_NOT_FOUND
    Result<Span<u8>> missing = fa.read_all(fa.ctx, sv("tl_platform_file_test_missing.bin"), nullptr);
    TL_EXPECT_EQ(missing.err, (ErrCode)ERR_PLATFORM_FILE_NOT_FOUND);

    // 32 MB: holds the 10 MB write pattern AND its 10 MB read-back at once, plus the zero-byte
    // and over-long-path pushes, in the same arena.
    VMemArena arena;
    TL_ASSERT_EQ(vmem_arena_init(&arena, "read_all_test"_id, 32u * 1024u * 1024u, 0u, &api->vmem), ERR_OK);

    // 0-byte file -> count 0 and a NUL at [0]
    const char* zero_path = "tl_platform_file_test_zero.bin";
    raw_write_file(t, zero_path, "", 0);
    Result<Span<u8>> zero = fa.read_all(fa.ctx, sv_c(zero_path), &arena);
    TL_ASSERT_EQ(zero.err, ERR_OK);
    TL_EXPECT_EQ(zero.value.count, 0u);
    TL_EXPECT_EQ(zero.value.data[0], 0u);
    remove(zero_path);

    // 10 MB round-trip
    const char* big_path = "tl_platform_file_test_10mb.bin";
    enum { TEN_MB = 10u * 1024u * 1024u };
    u8* pattern = (u8*)arena_push(&arena, TEN_MB, 16u);
    for (u32 i = 0; i < TEN_MB; ++i) { pattern[i] = (u8)(i * 2654435761u); }
    raw_write_file(t, big_path, pattern, TEN_MB);
    const u64 used_before = arena_mark(&arena);
    Result<Span<u8>> big = fa.read_all(fa.ctx, sv_c(big_path), &arena);
    TL_ASSERT_EQ(big.err, ERR_OK);
    TL_ASSERT_EQ(big.value.count, (u32)TEN_MB);
    TL_EXPECT_EQ(memcmp(big.value.data, pattern, TEN_MB), 0);
    // arena `used` grows by exactly align16(len+1)
    const u64 expect_growth = ((u64)TEN_MB + 1u + 15u) & ~15ull;
    TL_EXPECT_EQ(arena_mark(&arena) - used_before, expect_growth);
    remove(big_path);

    // path > 1024 -> PATH_TOO_LONG
    char long_name[1100];
    memset(long_name, 'a', sizeof(long_name) - 1u);
    long_name[sizeof(long_name) - 1u] = 0;
    Result<Span<u8>> too_long = fa.read_all(fa.ctx, StrView{ long_name, (u32)strlen(long_name) }, &arena);
    TL_EXPECT_EQ(too_long.err, (ErrCode)ERR_PLATFORM_PATH_TOO_LONG);

    platform_test_shutdown(api);
}

TL_TEST(enumerate_sorted, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const FileApi& fa = api->file;

#ifdef _WIN32
    _mkdir(TEST_DIR);
#else
    mkdir(TEST_DIR, 0755);
#endif

    // 50 files in a scrambled creation order (interleaved names so mtime order != name order)
    enum { N = 50 };
    for (u32 pass = 0; pass < N; ++pass) {
        const u32 i = (pass * 37u) % N;   // a fixed permutation, not the identity
        char path[256];
        snprintf(path, sizeof(path), "%s/f_%02u.txt", TEST_DIR, i);
        raw_write_file(t, path, "x", 1);
    }

    FileEntry out[N];
    Result<u32> r = fa.enumerate(fa.ctx, sv_c(TEST_DIR), out, N);
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_ASSERT_EQ(r.value, (u32)N);
    bool sorted = true;
    for (u32 i = 1; i < N; ++i) {
        if (memcmp(out[i - 1u].name, out[i].name, sizeof(out[i].name)) > 0) { sorted = false; break; }
    }
    TL_EXPECT_TRUE(sorted);

    // cap smaller than count -> FILE_TOO_LARGE
    FileEntry small_out[10];
    Result<u32> too_small = fa.enumerate(fa.ctx, sv_c(TEST_DIR), small_out, 10u);
    TL_EXPECT_EQ(too_small.err, (ErrCode)ERR_PLATFORM_FILE_TOO_LARGE);

    for (u32 i = 0; i < N; ++i) {
        char path[256];
        snprintf(path, sizeof(path), "%s/f_%02u.txt", TEST_DIR, i);
        remove(path);
    }
#ifdef _WIN32
    _rmdir(TEST_DIR);
#else
    rmdir(TEST_DIR);
#endif

    platform_test_shutdown(api);
}
