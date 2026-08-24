// file.cpp - the headless FileApi (docs/PLATFORM.md §9.4 "file, clock, thread": real, OS-direct,
// #ifdef _WIN32 branches in one TU, no SDL). write_atomic delegates to the shared
// os_write_atomic (docs/PLATFORM.md §9.1: "both impls point file.write_atomic here").
#include "platform/impl_headless/headless_apis.h"
#include "platform/os_path.h"
#include "platform/os_file_atomic.h"

#include "foundation/tl_assert.h"

#include <string.h>   // memcmp - the sorted-enumerate compare (docs/CPP-SUBSET.md §1)
#include <stdio.h>    // snprintf - building the stat() path in enumerate

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
// NOMINMAX before EVERY <windows.h> in the tree (ruled 2026-08-24, TODO.md R6; checked by
// tools/audit/includes.py). windows.h's raw min/max macros mangle fx.h's free functions of
// the same name in any TU that reaches both, and the failure reads as "too many arguments
// to function-like macro invocation" on an fx declaration, not as a min/max collision.
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#endif

namespace {

// docs/PLATFORM.md §9.6 read_all_contract: "arena `used` grows by exactly align16(len+1)" -
// arena_push aligns the START of a push, not its SIZE (docs/MEMORY.md §8.2), so the size pushed
// here must itself be rounded up, or a non-multiple-of-16 file leaves `used` unaligned.
u64 align16(u64 v) { return (v + 15u) & ~(u64)15u; }

Result<Span<u8>> hf_read_all(void* ctx, StrView path, VMemArena* arena) {
    HeadlessState* s = (HeadlessState*)ctx;
    (void)s;
    char path_buf[OS_PATH_MAX];
    if (!os_path_to_cstr(path, path_buf, sizeof(path_buf))) {
        return Result<Span<u8>>{ Span<u8>{}, (ErrCode)ERR_PLATFORM_PATH_TOO_LONG };
    }
#ifdef _WIN32
    wchar_t path_w[OS_PATH_MAX];
    if (!os_path_to_wide(path, path_w, (u32)(sizeof(path_w) / sizeof(path_w[0])))) {
        return Result<Span<u8>>{ Span<u8>{}, (ErrCode)ERR_PLATFORM_PATH_TOO_LONG };
    }
    HANDLE h = CreateFileW(path_w, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return Result<Span<u8>>{ Span<u8>{}, (ErrCode)ERR_PLATFORM_FILE_NOT_FOUND };
    }
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart > (LONGLONG)(1ull << 30)) {
        CloseHandle(h);
        return Result<Span<u8>>{ Span<u8>{}, (ErrCode)ERR_PLATFORM_FILE_TOO_LARGE };
    }
    const u64 len = (u64)li.QuadPart;
    u8* buf = (u8*)arena_push(arena, align16(len + 1u), 16u);
    u64 got = 0u;
    bool ok = true;
    while (got < len) {
        DWORD chunk = (DWORD)((len - got) > 0x10000000ull ? 0x10000000ull : (len - got));
        DWORD read_now = 0;
        if (!ReadFile(h, buf + got, chunk, &read_now, nullptr) || read_now == 0u) { ok = false; break; }
        got += read_now;
    }
    CloseHandle(h);
    if (!ok) { return Result<Span<u8>>{ Span<u8>{}, (ErrCode)ERR_PLATFORM_FILE_IO }; }
    buf[len] = 0u;
    return Result<Span<u8>>{ Span<u8>{ buf, (u32)len }, ERR_OK };
#else
    int fd = open(path_buf, O_RDONLY);
    if (fd < 0) { return Result<Span<u8>>{ Span<u8>{}, (ErrCode)ERR_PLATFORM_FILE_NOT_FOUND }; }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0 || (u64)size > (1ull << 30)) {
        close(fd);
        return Result<Span<u8>>{ Span<u8>{}, (ErrCode)ERR_PLATFORM_FILE_TOO_LARGE };
    }
    (void)lseek(fd, 0, SEEK_SET);
    const u64 len = (u64)size;
    u8* buf = (u8*)arena_push(arena, align16(len + 1u), 16u);
    u64 got = 0u;
    bool ok = true;
    while (got < len) {
        ssize_t r = read(fd, buf + got, (size_t)(len - got));
        if (r < 0) { if (errno == EINTR) { continue; } ok = false; break; }
        // A 0-byte read before `len` means the file shrank underneath us. The Windows branch above
        // already calls that FILE_IO; returning ERR_OK here with count == len would hand the
        // caller a span whose tail was never read - the same input, two different contracts,
        // and only one of the two OSes has ever executed. FILE_IO on both.
        if (r == 0) { ok = false; break; }
        got += (u64)r;
    }
    close(fd);
    if (!ok) { return Result<Span<u8>>{ Span<u8>{}, (ErrCode)ERR_PLATFORM_FILE_IO }; }
    buf[len] = 0u;
    return Result<Span<u8>>{ Span<u8>{ buf, (u32)len }, ERR_OK };
#endif
}

ErrCode raw_write(const char* path, Span<const u8> data, bool append) {
#ifdef _WIN32
    wchar_t path_w[OS_PATH_MAX];
    if (!os_path_to_wide(StrView{ path, (u32)strlen(path) }, path_w, (u32)(sizeof(path_w) / sizeof(path_w[0])))) {
        return (ErrCode)ERR_PLATFORM_PATH_TOO_LONG;
    }
    HANDLE h = CreateFileW(path_w, GENERIC_WRITE, 0, nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return (ErrCode)ERR_PLATFORM_FILE_IO; }
    if (append) { LARGE_INTEGER z{}; SetFilePointerEx(h, z, nullptr, FILE_END); }
    const u8* p = data.data; u64 left = (u64)data.count; bool ok = true;
    while (left > 0u) {
        DWORD chunk = (DWORD)(left > 0x10000000ull ? 0x10000000ull : left);
        DWORD wrote = 0;
        if (!WriteFile(h, p, chunk, &wrote, nullptr) || wrote != chunk) { ok = false; break; }
        p += chunk; left -= chunk;
    }
    CloseHandle(h);
    return ok ? ERR_OK : (ErrCode)ERR_PLATFORM_FILE_IO;
#else
    int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0) { return (ErrCode)ERR_PLATFORM_FILE_IO; }
    const u8* p = data.data; u64 left = (u64)data.count; bool ok = true;
    while (left > 0u) {
        ssize_t wrote = write(fd, p, (size_t)left);
        if (wrote < 0) { if (errno == EINTR) { continue; } ok = false; break; }
        p += wrote; left -= (u64)wrote;
    }
    close(fd);
    return ok ? ERR_OK : (ErrCode)ERR_PLATFORM_FILE_IO;
#endif
}

ErrCode hf_write_all(void*, StrView path, Span<const u8> data) {
    char buf[OS_PATH_MAX];
    if (!os_path_to_cstr(path, buf, sizeof(buf))) { return (ErrCode)ERR_PLATFORM_PATH_TOO_LONG; }
    return raw_write(buf, data, false);
}

ErrCode hf_write_atomic(void*, StrView path, Span<const u8> data) {
    return os_write_atomic(path, data);
}

ErrCode hf_append(void*, StrView path, Span<const u8> data) {
    char buf[OS_PATH_MAX];
    if (!os_path_to_cstr(path, buf, sizeof(buf))) { return (ErrCode)ERR_PLATFORM_PATH_TOO_LONG; }
    return raw_write(buf, data, true);
}

u8 hf_exists(void*, StrView path) {
    char buf[OS_PATH_MAX];
    if (!os_path_to_cstr(path, buf, sizeof(buf))) { return 0u; }
#ifdef _WIN32
    wchar_t path_w[OS_PATH_MAX];
    if (!os_path_to_wide(path, path_w, (u32)(sizeof(path_w) / sizeof(path_w[0])))) { return 0u; }
    return GetFileAttributesW(path_w) != INVALID_FILE_ATTRIBUTES ? 1u : 0u;
#else
    return access(buf, F_OK) == 0 ? 1u : 0u;
#endif
}

// Insertion sort on the <= cap entries by bytewise name comparison (tools-grade, bounded -
// docs/PLATFORM.md §9.3 "file").
void sort_entries(FileEntry* out, u32 count) {
    for (u32 i = 1; i < count; ++i) {
        FileEntry key = out[i];
        u32 j = i;
        while (j > 0u && memcmp(out[j - 1u].name, key.name, sizeof(key.name)) > 0) {
            out[j] = out[j - 1u];
            --j;
        }
        out[j] = key;
    }
}

Result<u32> hf_enumerate(void*, StrView dir, FileEntry* out, u32 cap) {
    char dir_buf[OS_PATH_MAX];
    if (!os_path_to_cstr(dir, dir_buf, sizeof(dir_buf))) {
        return Result<u32>{ 0u, (ErrCode)ERR_PLATFORM_PATH_TOO_LONG };
    }
    u32 count = 0u;
#ifdef _WIN32
    // "<dir>/*", built in UTF-8 and widened once. FindFirstFileA would decode the pattern in the
    // process ANSI code page AND hand cFileName back transcoded through it, which makes the
    // bytewise sort below a function of the machine's locale rather than of the directory
    // (docs/PLATFORM.md §9.3 "file": paths UTF-8; §9.2: "sorted bytewise by name").
    char pattern[OS_PATH_MAX + 2];
    const u32 n = (u32)strlen(dir_buf);
    for (u32 i = 0; i < n; ++i) { pattern[i] = dir_buf[i]; }
    pattern[n] = '/'; pattern[n + 1u] = '*'; pattern[n + 2u] = 0;
    wchar_t pattern_w[OS_PATH_MAX + 4];
    if (!os_path_to_wide(StrView{ pattern, n + 2u }, pattern_w,
                         (u32)(sizeof(pattern_w) / sizeof(pattern_w[0])))) {
        return Result<u32>{ 0u, (ErrCode)ERR_PLATFORM_PATH_TOO_LONG };
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern_w, &fd);
    if (h == INVALID_HANDLE_VALUE) { return Result<u32>{ 0u, (ErrCode)ERR_PLATFORM_FILE_NOT_FOUND }; }
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) { continue; }
        if (count >= cap) { FindClose(h); return Result<u32>{ count + 1u, (ErrCode)ERR_PLATFORM_FILE_TOO_LARGE }; }
        FileEntry& e = out[count];
        for (u32 i = 0; i < sizeof(e.name); ++i) { e.name[i] = 0; }
        // A name that does not fit FileEntry::name - or is not well-formed UTF-16 (an unpaired
        // surrogate is legal on NTFS) - is SKIPPED, not truncated: a truncated UTF-8 name is
        // invalid UTF-8 and no longer identifies the file it came from.
        if (os_path_from_wide(fd.cFileName, e.name, (u32)sizeof(e.name)) == 0u) { continue; }
        e.size = (u32)fd.nFileSizeLow;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1u : 0u;
        e._pad0 = 0u; e._pad1 = 0u;
        ++count;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir_buf);
    if (d == nullptr) { return Result<u32>{ 0u, (ErrCode)ERR_PLATFORM_FILE_NOT_FOUND }; }
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) { continue; }
        if (count >= cap) { closedir(d); return Result<u32>{ count + 1u, (ErrCode)ERR_PLATFORM_FILE_TOO_LARGE }; }
        FileEntry& e = out[count];
        u32 nl = (u32)strlen(ent->d_name);
        if (nl >= sizeof(e.name)) { nl = (u32)sizeof(e.name) - 1u; }
        for (u32 i = 0; i < nl; ++i) { e.name[i] = ent->d_name[i]; }
        for (u32 i = nl; i < sizeof(e.name); ++i) { e.name[i] = 0; }
        char full[OS_PATH_MAX * 2];
        int wn = snprintf(full, sizeof(full), "%s/%s", dir_buf, ent->d_name);
        struct stat st{};
        e.is_dir = 0u; e.size = 0u;
        if (wn > 0 && (usize)wn < sizeof(full) && stat(full, &st) == 0) {
            e.is_dir = S_ISDIR(st.st_mode) ? 1u : 0u;
            e.size = (u32)st.st_size;
        }
        e._pad0 = 0u; e._pad1 = 0u;
        ++count;
    }
    closedir(d);
#endif
    sort_entries(out, count);
    return Result<u32>{ count, ERR_OK };
}

StrView hf_base_path(void* ctx) { return ((HeadlessState*)ctx)->base_path; }
StrView hf_pref_path(void* ctx) { return ((HeadlessState*)ctx)->pref_path; }

Result<WatchHandle> hf_watch(void*, StrView, WatchFn, void*) {
    return Result<WatchHandle>{ WatchHandle{}, (ErrCode)ERR_PLATFORM_UNSUPPORTED };
}
void hf_unwatch(void*, WatchHandle) {}

}  // namespace

FileApi headless_file_api(HeadlessState* s) {
    return FileApi{ s, hf_read_all, hf_write_all, hf_write_atomic, hf_append, hf_exists,
                    hf_enumerate, hf_base_path, hf_pref_path, hf_watch, hf_unwatch };
}

void headless_init_paths(HeadlessState* s) {
    char cwd[OS_PATH_MAX];
#ifdef _WIN32
    // Wide, then transcoded to UTF-8 here: GetCurrentDirectoryA would hand back the ANSI-code-page
    // rendering of a cwd that may not be representable in it at all (docs/PLATFORM.md §9.3 "file").
    wchar_t cwd_w[OS_PATH_MAX];
    const DWORD wn = GetCurrentDirectoryW((DWORD)(sizeof(cwd_w) / sizeof(cwd_w[0])), cwd_w);
    TL_CHECK(wn > 0u && wn < sizeof(cwd_w) / sizeof(cwd_w[0]));
    // Win32 answers in backslashes; the contract is '/' separators (docs/PLATFORM.md §9.3 "file").
    for (DWORD i = 0; i < wn; ++i) { if (cwd_w[i] == L'\\') { cwd_w[i] = L'/'; } }
    u32 n = os_path_from_wide(cwd_w, cwd, (u32)sizeof(cwd) - 2u);
    TL_CHECK(n > 0u);
#else
    TL_CHECK(getcwd(cwd, sizeof(cwd) - 2u) != nullptr);
    u32 n = (u32)strlen(cwd);
#endif
    if (n == 0u || cwd[n - 1u] != '/') { cwd[n] = '/'; ++n; }
    cwd[n] = 0;
    char* stored = (char*)arena_push(&s->arena, (u64)n, 1u);
    for (u32 i = 0; i < n; ++i) { stored[i] = cwd[i]; }
    s->base_path = StrView{ stored, n };
    s->pref_path = s->base_path;
}
