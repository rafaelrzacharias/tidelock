// editor.h - panel registry (editor_init/register_panel/toggle_panel/shutdown, all real, no
// platform dependency) and editor_frame (a stub - see editor.h's Status note: blocked on
// PlatformDevApi, docs/PLATFORM.md §9.7 step 5, not this lane's file).
#include "editor/editor.h"

#include "foundation/tl_assert.h"

#include <string.h>

namespace {
Panel* find_panel(Editor* ed, const char* name) {
    for (u32 i = 0; i < ed->panel_count; ++i) {
        if (strcmp(ed->panels[i].name, name) == 0) { return &ed->panels[i]; }
    }
    return nullptr;
}
}  // namespace

ErrCode editor_init(Editor* ed, const VMemApi* os, u64 dev_arena_reserve) {
    memset(ed, 0, sizeof(Editor));
    const u64 reserve = (dev_arena_reserve != 0u) ? dev_arena_reserve : (16u * 1024u * 1024u);
    const ErrCode err = vmem_arena_init(&ed->dev_arena, "editor.dev"_id, reserve, 0u, os);
    if (err != ERR_OK) { return err; }
    console_init(&ed->console);   // redundant after the memset above, but matches console.h's own
                                   // contract rather than relying on a caller knowing that
    ed->initialized = 1;
    return ERR_OK;
}

void editor_register_panel(Editor* ed, const char* name, PanelDrawFn draw_fn, bool default_open) {
    if (ed->panel_count >= EDITOR_MAX_PANELS) { TL_FATAL("editor_register_panel: EDITOR_MAX_PANELS exhausted"); }
    if (find_panel(ed, name) != nullptr) { TL_FATAL("editor_register_panel: duplicate panel name"); }

    Panel* p = &ed->panels[ed->panel_count];
    const usize len = strlen(name);
    TL_CHECK(len < (usize)EDITOR_PANEL_NAME_CAP);
    memcpy(p->name, name, len);
    p->name[len] = '\0';
    p->draw_fn = draw_fn;
    p->default_open = default_open;
    p->open = default_open;
    ed->panel_count += 1u;
}

void editor_toggle_panel(Editor* ed, const char* name) {
    Panel* p = find_panel(ed, name);
    TL_CHECK(p != nullptr);
    p->open = !p->open;
}

void editor_frame(Editor* /*ed*/, World* /*w*/) {
    TL_FATAL("editor_frame: unimplemented - blocked on PlatformDevApi (docs/PLATFORM.md section "
             "9.7 step 5, not built yet); see editor.h's Status note");
}

void editor_shutdown(Editor* ed) {
    // No ImGui context exists yet (editor_frame, which would create one, is unimplemented) - the
    // dev arena's own VMemApi-backed reserve is released by whatever owns the VMemApi's lifetime
    // (app/wiring.cpp, W4, not built); nothing to do here today beyond marking the instance dead.
    ed->initialized = 0;
}
