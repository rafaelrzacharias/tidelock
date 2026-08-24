#pragma once
// ---------------------------------------------------------------------------------------------
// os_file_atomic.h - the shared write_atomic implementation both impls point FileApi at.
//
// Spec: docs/PLATFORM.md §9.3 "write_atomic", §9.1 ("both impls point file.write_atomic here").
// Purpose: tmp-write -> fsync -> rename, so a killed process never leaves a torn file: the target
//   is either the old content or the new content, never a partial write. SDL_IOStream has no
//   fsync, hence a raw Win32/POSIX TU instead of routing through the platform's own read/write.
// Invariants: on any failure the temp file is deleted and the target is untouched; on success no
//   `.tmp.*` file survives.
// Determinism: not part of the public contract - platform.h never includes this.
// Threading: as thread-safe as the OS calls behind it; two callers racing the same path can still
//   interleave (no locking here, matching write_all/append).
// Includes: foundation/{tl_types,strview,span}.h only.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/strview.h"
#include "foundation/span.h"

// tmp = path + ".tmp." + pid; write data; fsync; rename over path. Returns ERR_PLATFORM_PATH_TOO_LONG
// for an over-length path, ERR_PLATFORM_FILE_IO on any step's failure (temp deleted, target untouched).
ErrCode os_write_atomic(StrView path, Span<const u8> data);
