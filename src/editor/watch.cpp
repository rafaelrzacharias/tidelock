// watch.h - bind once, re-resolve only on ERR_PATH_NO_ENTITY. Spec: docs/TOOLING.md §3, §9.3.6.
#include "editor/watch.h"

#include "foundation/tl_assert.h"

#include <string.h>

ErrCode watch_init(Watch* w, World* world, StrView path) {
    memset(w, 0, sizeof(Watch));
    TL_CHECK(path.len < (u32)WATCH_PATH_CAP);
    memcpy(w->path, path.ptr, path.len);
    w->path[path.len] = '\0';
    w->path_len = path.len;

    Result<PathRef> r = dotpath_resolve(world, path);
    if (r.err != ERR_OK) { return r.err; }
    w->ref = r.value;
    w->resolved = 1;
    return ERR_OK;
}

void watch_refresh(Watch* w, World* world) {
    if (!w->resolved) { w->ok = 0; return; }

    Result<u32> got = dotpath_get_raw(world, w->ref, w->last_value, (u32)WATCH_VALUE_CAP);
    if (got.err == ERR_PATH_NO_ENTITY) {
        Result<PathRef> r = dotpath_resolve(world, StrView{ w->path, w->path_len });
        if (r.err != ERR_OK) { w->ok = 0; return; }
        w->ref = r.value;
        got = dotpath_get_raw(world, w->ref, w->last_value, (u32)WATCH_VALUE_CAP);
    }
    if (got.err != ERR_OK) { w->ok = 0; return; }
    w->last_value_len = got.value;
    w->ok = 1;
}
