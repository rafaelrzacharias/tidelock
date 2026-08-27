#pragma once
// ---------------------------------------------------------------------------------------------
// watch.h - Watch: a dot-path bound once, re-resolved only on a stale handle.
//
// Spec: docs/TOOLING.md §3 ("watch <path> renders a live overlay; bound through field tables,
//   not strings per frame"), §9.3.6 (the re-resolve rule this header implements).
// Purpose: `watch_init(w, "player.Transform.x")` resolves the path ONCE into a `PathRef`
//   (`dotpath.h`); `watch_refresh` re-reads it every frame through the cached tuple and only
//   calls `dotpath_resolve` again when the read comes back `ERR_PATH_NO_ENTITY` (a stale handle
//   - the entity was destroyed and its slot recycled, or never existed) - never the string.
// Invariants: the path string is copied (owned) at `watch_init` so the caller's buffer need not
//   outlive the call; `WATCH_PATH_CAP` (128) bytes, `TL_CHECK`ed to fit. A watch whose path
//   fails to resolve even after a refresh attempt keeps its LAST successfully read value and
//   `ok = false` (the overlay panel, not built yet, is expected to grey it out rather than show
//   a stale number as if it were live).
// Determinism: none of this touches sim state - a watch only reads (`dotpath_get_raw`); no
//   command is ever recorded by a watch.
// Threading: none - caller-owned, single-threaded (render-rate, matching every other panel).
// Includes: foundation/tl_types.h, foundation/strview.h, editor/dotpath.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/strview.h"
#include "editor/dotpath.h"

enum { WATCH_PATH_CAP = 128, WATCH_VALUE_CAP = 32 };

struct Watch {
    char    path[WATCH_PATH_CAP];
    u32     path_len;
    PathRef ref;
    u8      resolved;              // 0 until the first successful dotpath_resolve
    u8      ok;                    // 1 iff the last watch_refresh read succeeded
    u8      _pad0[2];
    u8      last_value[WATCH_VALUE_CAP];
    u32     last_value_len;
};

// Copies `path` (TL_CHECK: fits WATCH_PATH_CAP-1) and resolves it once. `resolved`/`ok` are 0
// until the caller's first `watch_refresh` (this door does not itself read the value - matching
// dotpath_resolve's own "pure resolution, no read" shape). Returns the resolve ErrCode (ERR_OK
// or one of dotpath.h's ERR_PATH_* codes) so a caller can surface an immediate bad-path message.
ErrCode watch_init(Watch* w, World* world, StrView path);

// Reads through the cached `ref`. On `ERR_PATH_NO_ENTITY` (the stale-handle signal - docs/
// TOOLING.md §9.3.6), re-runs `dotpath_resolve(world, path)` once and retries the read with the
// fresh tuple; any other failure (component/field gone - a hot-reload edge, not handled by a
// retry) leaves `ok = false` and the last value untouched. Updates `last_value`/`last_value_len`
// on success.
void watch_refresh(Watch* w, World* world);
