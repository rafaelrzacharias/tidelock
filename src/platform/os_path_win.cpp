// os_path_win.cpp - the Windows UTF-8 <-> UTF-16 path boundary (docs/PLATFORM.md §9.3 "file":
// "paths UTF-8"; §9.4 names CreateFileW). Compiled only on Windows (src/platform/CMakeLists.txt).
//
// This TU exists so os_path.h can stay OS-free: it is a src/platform/*.h, and the raw-OS-header
// ban only exempts src/platform/os_*.cpp (tools/audit/includes.py's is_backend_free), which this
// file is. Same reason os_win_vmem.cpp is a TU rather than an inline header.
#include "platform/os_path.h"

#define WIN32_LEAN_AND_MEAN
// NOMINMAX before EVERY <windows.h> in the tree (ruled 2026-08-24, TODO.md R6; checked by
// tools/audit/includes.py). windows.h's raw min/max macros mangle fx.h's free functions of
// the same name in any TU that reaches both, and the failure reads as "too many arguments
// to function-like macro invocation" on an fx declaration, not as a min/max collision.
#define NOMINMAX
#include <windows.h>

// MB_ERR_INVALID_CHARS / WC_ERR_INVALID_CHARS are the "refuse, never substitute" flags: without
// them both codecs quietly emit U+FFFD for malformed input, which would turn a bad path into a
// DIFFERENT valid path rather than an error.
bool os_path_to_wide(StrView path, wchar_t* buf, u32 cap_units) {
    if (path.len >= OS_PATH_MAX || cap_units == 0u) { return false; }
    if (path.len == 0u) { buf[0] = L'\0'; return true; }
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.ptr, (int)path.len,
                                      nullptr, 0);
    if (n <= 0 || (u32)n >= cap_units) { return false; }
    const int wrote = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.ptr, (int)path.len,
                                          buf, n);
    if (wrote != n) { return false; }
    buf[n] = L'\0';
    return true;
}

u32 os_path_from_wide(const wchar_t* w, char* buf, u32 cap) {
    if (w == nullptr || cap == 0u) { return 0u; }
    if (w[0] == L'\0') { buf[0] = '\0'; return 0u; }
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, nullptr, 0,
                                      nullptr, nullptr);   // -1: includes the NUL in the count
    if (n <= 1 || (u32)n > cap) { return 0u; }
    const int wrote = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, buf, n,
                                          nullptr, nullptr);
    if (wrote != n) { return 0u; }
    return (u32)(n - 1);   // the count includes the NUL; the length does not
}
