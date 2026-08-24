// os_file_atomic.cpp - write_atomic, shared by both impls (docs/PLATFORM.md §9.3). One TU, both
// OSes: SDL_IOStream has no fsync, so this goes straight to Win32/POSIX.
//
// Dev-only crash-safety hook (docs/PLATFORM.md §9.6 write_atomic_crash_safety): when
// TL_TEST_ATOMIC_KILL_AT is "1"/"2"/"3", this self-terminates at the matching instrumented point
// (after temp write / after fsync / before rename) instead of continuing. A real external kill
// would race the same three points; self-termination is deterministic to test and leaves the
// filesystem in the identical observable state (the process stops running, mid-function, with no
// further bytes touched) - the test's own comment records the simplification.
#include "platform/os_file_atomic.h"
#include "platform/os_path.h"
#include "platform/platform.h"

#include "foundation/tl_assert.h"

#include <stdlib.h>   // getenv, atoi - dev-only test hook
#include <string.h>   // strlen - both branches

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
// NOMINMAX before EVERY <windows.h> in the tree (ruled 2026-08-24, TODO.md R6; checked by
// tools/audit/includes.py). windows.h's raw min/max macros mangle fx.h's free functions of
// the same name in any TU that reaches both, and the failure reads as "too many arguments
// to function-like macro invocation" on an fx declaration, not as a min/max collision.
#define NOMINMAX
#include <windows.h>
#else
#include <stdio.h>    // rename - the atomic-replace verb of the POSIX branch
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#endif

namespace {

#if TL_DEV
void maybe_kill(int point) {
    const char* v = getenv("TL_TEST_ATOMIC_KILL_AT");
    if (v != nullptr && atoi(v) == point) {
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 0xDEADu);
#else
        _exit(137);   // 128 + SIGKILL, the conventional shell-visible status for it
#endif
    }
}
#else
void maybe_kill(int) {}
#endif

// path + ".tmp." + pid, into buf[cap]. Returns false on overflow (buf untouched).
bool make_tmp_name(const char* path, char* buf, u32 cap) {
    u64 pid;
#ifdef _WIN32
    pid = (u64)GetCurrentProcessId();
#else
    pid = (u64)getpid();
#endif
    char pid_buf[24];
    u32 pid_len = 0;
    { u64 v = pid; char rev[24]; u32 n = 0; do { rev[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v != 0u);
      while (n > 0u) { pid_buf[pid_len++] = rev[--n]; } }
    const u32 base_len = (u32)strlen(path);
    const u32 need = base_len + 5u /* ".tmp." */ + pid_len + 1u;
    if (need > cap) { return false; }
    u32 w = 0;
    for (u32 i = 0; i < base_len; ++i) { buf[w++] = path[i]; }
    const char* suf = ".tmp.";
    for (u32 i = 0; i < 5u; ++i) { buf[w++] = suf[i]; }
    for (u32 i = 0; i < pid_len; ++i) { buf[w++] = pid_buf[i]; }
    buf[w] = 0;
    return true;
}

}  // namespace

ErrCode os_write_atomic(StrView path, Span<const u8> data) {
    char path_buf[OS_PATH_MAX];
    if (!os_path_to_cstr(path, path_buf, sizeof(path_buf))) {
        return (ErrCode)ERR_PLATFORM_PATH_TOO_LONG;
    }
    char tmp_buf[OS_PATH_MAX + 32];
    if (!make_tmp_name(path_buf, tmp_buf, sizeof(tmp_buf))) {
        return (ErrCode)ERR_PLATFORM_PATH_TOO_LONG;
    }

#ifdef _WIN32
    // UTF-16 at the boundary, never the *A entry points: those decode in the process ANSI code
    // page, not UTF-8 (docs/PLATFORM.md §9.3 "file", §9.4 "CreateFileW"). tmp_buf/path_buf are
    // still UTF-8 c-strings - only the OS call is wide.
    wchar_t tmp_w[OS_PATH_MAX + 32];
    wchar_t path_w[OS_PATH_MAX];
    if (!os_path_to_wide(StrView{ tmp_buf, (u32)strlen(tmp_buf) }, tmp_w, (u32)(sizeof(tmp_w) / sizeof(tmp_w[0]))) ||
        !os_path_to_wide(path, path_w, (u32)(sizeof(path_w) / sizeof(path_w[0])))) {
        return (ErrCode)ERR_PLATFORM_PATH_TOO_LONG;
    }
    HANDLE h = CreateFileW(tmp_w, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return (ErrCode)ERR_PLATFORM_FILE_IO; }
    const u8* p = data.data; u64 left = (u64)data.count; bool write_ok = true;
    while (left > 0u) {
        DWORD chunk = (DWORD)(left > 0x10000000ull ? 0x10000000ull : left);
        DWORD wrote = 0;
        if (!WriteFile(h, p, chunk, &wrote, nullptr) || wrote != chunk) { write_ok = false; break; }
        p += chunk; left -= chunk;
    }
    if (!write_ok) { CloseHandle(h); DeleteFileW(tmp_w); return (ErrCode)ERR_PLATFORM_FILE_IO; }
    maybe_kill(1);
    const bool flush_ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    if (!flush_ok) { DeleteFileW(tmp_w); return (ErrCode)ERR_PLATFORM_FILE_IO; }
    maybe_kill(2);
    maybe_kill(3);
    if (!MoveFileExW(tmp_w, path_w, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_w);
        return (ErrCode)ERR_PLATFORM_FILE_IO;
    }
    return ERR_OK;
#else
    int fd = open(tmp_buf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { return (ErrCode)ERR_PLATFORM_FILE_IO; }
    const u8* p = data.data; u64 left = (u64)data.count; bool write_ok = true;
    while (left > 0u) {
        ssize_t wrote = write(fd, p, (size_t)left);
        if (wrote < 0) {
            if (errno == EINTR) { continue; }
            write_ok = false; break;
        }
        p += wrote; left -= (u64)wrote;
    }
    if (!write_ok) { close(fd); unlink(tmp_buf); return (ErrCode)ERR_PLATFORM_FILE_IO; }
    maybe_kill(1);
    const bool sync_ok = fsync(fd) == 0;
    close(fd);
    if (!sync_ok) { unlink(tmp_buf); return (ErrCode)ERR_PLATFORM_FILE_IO; }
    maybe_kill(2);
    maybe_kill(3);
    if (rename(tmp_buf, path_buf) != 0) { unlink(tmp_buf); return (ErrCode)ERR_PLATFORM_FILE_IO; }
    // fsync the parent directory so the rename itself survives a crash, not just the data.
    char dir_buf[OS_PATH_MAX];
    { u32 n = (u32)strlen(path_buf); if (n >= sizeof(dir_buf)) { n = (u32)sizeof(dir_buf) - 1u; }
      for (u32 i = 0; i < n; ++i) { dir_buf[i] = path_buf[i]; } dir_buf[n] = 0; }
    char* dir = dirname(dir_buf);
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) { (void)fsync(dfd); close(dfd); }
    return ERR_OK;
#endif
}
