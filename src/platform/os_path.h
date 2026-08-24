#pragma once
// ---------------------------------------------------------------------------------------------
// os_path.h - StrView path -> NUL-terminated stack buffer, the one conversion every FileApi call
//   needs before it can reach a real OS call.
//
// Spec: docs/PLATFORM.md §9.3 "file" ("paths UTF-8, / separators, <= 1024 B (else
//   PATH_TOO_LONG), copied to a stack buffer with a NUL").
// Purpose: shared between os_file_atomic.cpp and every impl's file.cpp so the 1024-byte limit and
//   the NUL-termination rule are written once.
// Invariants: `cap` must be > path.len (room for the NUL); a path of exactly 1024 B or longer is
//   refused, never silently truncated.
// Determinism: not part of the public contract - platform.h never includes this.
// Threading: none - a pure copy into caller-owned storage.
// Includes: foundation/{tl_types,strview}.h only - NOT <windows.h>: this is a src/platform/*.h,
//   not one of docs/PLATFORM.md section 9.1's os_*.cpp TUs, so the raw-OS-header ban applies
//   (tools/audit/includes.py). The Windows wide-path conversion is therefore declared here and
//   implemented in os_path_win.cpp, which does carry the exemption.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/strview.h"

enum : u32 { OS_PATH_MAX = 1024 };

// Copies `path` into `buf[0..cap)`, NUL-terminated. Returns false (buf untouched) when
// `path.len >= OS_PATH_MAX` or `path.len >= cap` - the caller reports ERR_PLATFORM_PATH_TOO_LONG.
inline bool os_path_to_cstr(StrView path, char* buf, u32 cap) {
    if (path.len >= OS_PATH_MAX || path.len >= cap) { return false; }
    for (u32 i = 0; i < path.len; ++i) { buf[i] = path.ptr[i]; }
    buf[path.len] = 0;
    return true;
}

#ifdef _WIN32
// docs/PLATFORM.md section 9.3 "file": paths are UTF-8. The Win32 *A entry points decode their
// argument in the process ANSI code page, NOT UTF-8, so every non-ASCII path they are handed
// names a different file (or none) - and the name a *A directory walk hands BACK is transcoded
// out of UTF-16 through that same code page, which makes `enumerate`'s bytewise sort a function
// of the machine's locale rather than of the directory. Section 9.4 names the W entry points for
// exactly this reason. These two convert at the boundary instead, through the OS's own UTF-8
// codec, so no code page is ever consulted.
//
// Both refuse rather than substitute: malformed input and an over-long result are failures, never
// a U+FFFD that would silently name a different file. `wchar_t` is a builtin type, so declaring
// them needs no OS header here.

// UTF-8 `path` -> NUL-terminated UTF-16 in buf[0..cap_units). False (buf untouched) when the path
// is >= OS_PATH_MAX bytes, when the result does not fit, or when the bytes are not valid UTF-8.
bool os_path_to_wide(StrView path, wchar_t* buf, u32 cap_units);

// NUL-terminated UTF-16 `w` -> UTF-8 in buf[0..cap), NUL-terminated. Returns the byte length
// written, or 0 when the result does not fit or the input is not well-formed UTF-16 (an unpaired
// surrogate - legal on NTFS, not representable as UTF-8 - is refused, not replaced).
u32 os_path_from_wide(const wchar_t* w, char* buf, u32 cap);
#endif
