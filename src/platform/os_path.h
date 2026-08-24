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
// Includes: foundation/{tl_types,strview}.h only.
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
