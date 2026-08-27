// probes_panel.test.cpp - probes_panel_draw runs headless (docs/TOOLING.md §9.5) across
// zero-key, populated, and disabled-key state; probes_panel_register wires into Editor's panel
// table.
// Spec: docs/TOOLING.md §9.1 (probes_panel.cpp's row), §9.3.3, §9.4, §9.6 build order item 5.
//
// TL_TEST's generated signature is `(TestCtx* t)`.
//
// editor/probes_panel.cpp is compiled ONLY on the debug/dev tiers (src/editor/CMakeLists.txt
// links an empty INTERFACE tl_editor on netcode/ship), so every call site here is behind
// `#if TL_DEV`, matching this session's log_panel/console_panel/inspector/profiler_panel test
// precedent. The panel is read-only at v0 (probes_panel.h's own Invariants note - no real toggle
// exists yet), so there is no write path to drive directly the way the other panels' tests do.
#include "runner/tl_test.h"

#if TL_DEV
#include "editor/editor.h"
#include "editor/probes_panel.h"
#include "foundation/tl_probe.h"
#include "foundation/vmem_test_api.h"
#include "imgui_test_util.h"
#include "imgui_internal.h"

#include <string.h>

namespace {
bool make_editor(Editor* ed) {
    VMemApi api = test_vmem_api();
    return editor_init(ed, &api, 0u) == ERR_OK;
}
}  // namespace
#endif  // TL_DEV

#define TL_PROBES_PANEL_SKIP TL_SKIP("editor/probes_panel.cpp is dev-only (TOOLING.md section 9.1); no symbol in this tier")

TL_TEST(probes_panel_draw_no_keys_no_crash, "editor,probes_panel,fast") {
#if TL_DEV
    tl_probe_test_reset();
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    imgui_test_begin_frame(t);
    probes_panel_draw(&ed, nullptr);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Probes") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
    tl_probe_test_reset();
#else
    TL_PROBES_PANEL_SKIP;
#endif
}

TL_TEST(probes_panel_register_wires_into_editor_panel_table, "editor,probes_panel,fast") {
#if TL_DEV
    tl_probe_test_reset();
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    probes_panel_register(&ed);
    TL_ASSERT_EQ(ed.panel_count, 1u);
    TL_EXPECT_EQ(strcmp(ed.panels[0].name, "Probes"), 0);
    TL_EXPECT_TRUE(ed.panels[0].default_open);
    TL_ASSERT_TRUE(ed.panels[0].draw_fn != nullptr);

    imgui_test_begin_frame(t);
    ed.panels[0].draw_fn(&ed, nullptr);   // exactly what editor_frame will do for every open panel
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Probes") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
    tl_probe_test_reset();
#else
    TL_PROBES_PANEL_SKIP;
#endif
}

TL_TEST(probes_panel_draw_every_kind_no_crash, "editor,probes_panel,fast") {
#if TL_DEV
    tl_probe_test_reset();
    tl_probe_test_set_tick(0);
    tl_probe_log("k.log"_id, "k.log", 42, 0, 1u);
    tl_probe_log("k.fx"_id, "k.fx", (1 << 18), 18u, 1u);   // pos_t-shaped: shows as 1.0
    tl_probe_on_change("k.onchange"_id, "k.onchange", 7, 0);
    tl_probe_mark("k.mark"_id, "k.mark");
    tl_probe_assert("k.assert"_id, "k.assert", 5, 0, 10);   // in range, no ERR log
    TL_ASSERT_EQ(tl_probe_key_count(), 5u);

    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    imgui_test_begin_frame(t);
    probes_panel_draw(&ed, nullptr);
    ImGuiWindow* win = ImGui::FindWindowByName("Probes");
    TL_ASSERT_TRUE(win != nullptr);
    TL_EXPECT_TRUE(win->DrawList->VtxBuffer.Size > 0);
    imgui_test_end_frame();
    editor_shutdown(&ed);
    tl_probe_test_reset();
#else
    TL_PROBES_PANEL_SKIP;
#endif
}

TL_TEST(probes_panel_draw_disabled_key_shown_no_crash, "editor,probes_panel,fast") {
#if TL_DEV
    tl_probe_test_reset();
    tl_probe_test_set_enabled(999u, "k.off", 0);
    TL_ASSERT_EQ(tl_probe_key_count(), 1u);
    TL_EXPECT_TRUE(!tl_probe_key_at(0)->enabled);

    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    imgui_test_begin_frame(t);
    probes_panel_draw(&ed, nullptr);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Probes") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
    tl_probe_test_reset();
#else
    TL_PROBES_PANEL_SKIP;
#endif
}
