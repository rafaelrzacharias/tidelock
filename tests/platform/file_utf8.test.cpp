// file_utf8.test.cpp - docs/PLATFORM.md §9.3 "file": "paths UTF-8". Not a §9.6 row; §9.6's file
// tests use ASCII names only, which is why the whole FileApi shipped on the Win32 *A entry points
// (CreateFileA/GetFileAttributesA/FindFirstFileA/GetCurrentDirectoryA) with 12 green tests.
//
// The *A entry points do not speak UTF-8. They decode their argument in the process ANSI code
// page - CP-1252 on a default Windows install - so a UTF-8 path with any byte >= 0x80 names a
// DIFFERENT file, or none. Worse for enumerate: cFileName comes back transcoded OUT of UTF-16
// through that same code page, so the §9.2 "sorted bytewise by name" order became a function of
// the machine's locale rather than of the directory. §9.4 names CreateFileW for exactly this
// reason.
//
// The names below are deliberately outside CP-1252 (Greek, CJK) and outside the BMP (an emoji,
// which is a UTF-16 surrogate PAIR - the case a naive widening gets wrong).
#include "runner/tl_test.h"
#include "platform_test_util.h"
#include "foundation/vmem_arena.h"
#include "foundation/hash.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <wchar.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

const char* UTF8_DIR = "tl_platform_utf8_tmp";
// Written as explicit UTF-8 bytes, not source literals: the source encoding must not decide what
// this test actually exercises.
const char* NAME_GREEK = "\xCE\xB1\xCE\xB2\xCE\xB3.txt";                 // U+03B1 U+03B2 U+03B3
const char* NAME_CJK   = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt";     // U+65E5 U+672C U+8A9E
const char* NAME_EMOJI = "\xF0\x9F\x8E\xAE.txt";                         // U+1F3AE, a surrogate pair

StrView sv_c(const char* s) { return StrView{ s, (u32)strlen(s) }; }

void join(char* out, u32 cap, const char* dir, const char* name) {
    snprintf(out, cap, "%s/%s", dir, name);
}

}  // namespace

TL_TEST(file_utf8_paths_round_trip, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const FileApi& fa = api->file;

#ifdef _WIN32
    _mkdir(UTF8_DIR);
#else
    mkdir(UTF8_DIR, 0755);
#endif

    VMemArena arena;
    TL_ASSERT_EQ(vmem_arena_init(&arena, "file_utf8_test"_id, 1u * 1024u * 1024u, 0u, &api->vmem), ERR_OK);

    const char* names[3] = { NAME_GREEK, NAME_CJK, NAME_EMOJI };
    const u8 body[5] = { 'h', 'e', 'l', 'l', 'o' };

    for (u32 i = 0; i < 3u; ++i) {
        char path[256];
        join(path, sizeof(path), UTF8_DIR, names[i]);

        // write_all -> exists -> read_all: all three must agree on WHICH file the bytes name.
        TL_ASSERT_EQ(fa.write_all(fa.ctx, sv_c(path), Span<const u8>{ body, 5u }), ERR_OK);
        TL_EXPECT_EQ(fa.exists(fa.ctx, sv_c(path)), 1u);
        Result<Span<u8>> back = fa.read_all(fa.ctx, sv_c(path), &arena);
        TL_ASSERT_EQ(back.err, ERR_OK);
        TL_ASSERT_EQ(back.value.count, 5u);
        TL_EXPECT_EQ(memcmp(back.value.data, body, 5u), 0);

        // write_atomic replaces the same file, not a code-page neighbour of it.
        const u8 body2[3] = { 'n', 'e', 'w' };
        TL_ASSERT_EQ(fa.write_atomic(fa.ctx, sv_c(path), Span<const u8>{ body2, 3u }), ERR_OK);
        Result<Span<u8>> back2 = fa.read_all(fa.ctx, sv_c(path), &arena);
        TL_ASSERT_EQ(back2.err, ERR_OK);
        TL_ASSERT_EQ(back2.value.count, 3u);
        TL_EXPECT_EQ(memcmp(back2.value.data, body2, 3u), 0);
    }

    // enumerate hands the names back as the SAME UTF-8 bytes that created them - the thing an
    // ANSI directory walk cannot do - and sorts on those bytes.
    FileEntry entries[16];
    Result<u32> r = fa.enumerate(fa.ctx, sv_c(UTF8_DIR), entries, 16u);
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_ASSERT_EQ(r.value, 3u);
    for (u32 i = 0; i < 3u; ++i) {
        bool found = false;
        for (u32 j = 0; j < r.value; ++j) {
            if (strcmp(entries[j].name, names[i]) == 0) { found = true; break; }
        }
        TL_EXPECT_TRUE(found);
    }
    bool sorted = true;
    for (u32 i = 1; i < r.value; ++i) {
        if (memcmp(entries[i - 1u].name, entries[i].name, sizeof(entries[i].name)) > 0) { sorted = false; break; }
    }
    TL_EXPECT_TRUE(sorted);

    // Teardown. The CRT's remove()/rmdir() are ANSI on Windows and cannot name these files at
    // all, so the wide CRT is used there - with the wide literals spelled independently of the
    // UTF-8 byte strings above, which is itself the last assertion: the two spellings must reach
    // the same file, and under the *A entry points they did not (the ANSI build left a mojibake
    // "Î±Î²Î³.txt" on disk next to the real "αβγ.txt", from the same input bytes).
#ifdef _WIN32
    const wchar_t* wnames[3] = { L"tl_platform_utf8_tmp/\u03B1\u03B2\u03B3.txt",
                                 L"tl_platform_utf8_tmp/\u65E5\u672C\u8A9E.txt",
                                 L"tl_platform_utf8_tmp/\U0001F3AE.txt" };
    for (u32 i = 0; i < 3u; ++i) { TL_EXPECT_EQ(_wremove(wnames[i]), 0); }
    TL_EXPECT_EQ(_wrmdir(L"tl_platform_utf8_tmp"), 0);
#else
    for (u32 i = 0; i < 3u; ++i) {
        char path[256];
        join(path, sizeof(path), UTF8_DIR, names[i]);
        TL_EXPECT_EQ(remove(path), 0);
    }
    TL_EXPECT_EQ(rmdir(UTF8_DIR), 0);
#endif
    platform_test_shutdown(api);
}

TL_TEST(enumerate_missing_dir_is_not_an_empty_dir, "platform") {
    const PlatformApi* api = platform_test_init();
    TL_ASSERT_NOT_NULL(api);
    const FileApi& fa = api->file;

    // Previously Result{0, ERR_OK} on both OSes: a directory that does not exist was
    // indistinguishable from one with no files in it - a silent fallback (CLAUDE.md: "fail loudly
    // and explicitly"), and the caller most likely to hit it is asset/mod discovery, where the
    // difference between "no mods" and "the mods folder is missing" is the whole diagnosis.
    FileEntry entries[4];
    Result<u32> r = fa.enumerate(fa.ctx, sv("tl_platform_no_such_directory_at_all"), entries, 4u);
    TL_EXPECT_EQ(r.err, (ErrCode)ERR_PLATFORM_FILE_NOT_FOUND);
    TL_EXPECT_EQ(r.value, 0u);

    platform_test_shutdown(api);
}
