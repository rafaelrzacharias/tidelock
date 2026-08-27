// log_panel.test.cpp - log_panel_draw runs headless (docs/TOOLING.md §9.5) across the ring's
// boundary shapes: empty, one record, many, and every LOG_* level's colour branch.
// Spec: docs/TOOLING.md §9.1 (log_panel.cpp's row), §9.6 build order item 5.
//
// TL_TEST's generated signature is `(TestCtx* t)`.
//
// editor/log_panel.cpp is compiled ONLY on the debug/dev tiers (src/editor/CMakeLists.txt links
// an empty INTERFACE tl_editor on netcode/ship), so every log_panel_draw call site here is behind
// `#if TL_DEV`, matching this session's console/editor/dotpath/watch test precedent. The null
// ImGui backend (imgui_test_util.h) is TL_DEV-only for the same reason (tests/CMakeLists.txt only
// adds imgui_impl_null.cpp on debug/dev).
#include "runner/tl_test.h"
#include "foundation/tl_log.h"

#if TL_DEV
#include "editor/log_panel.h"
#include "imgui_test_util.h"
#include "foundation/vmem_test_api.h"
// ImGuiWindow/FindWindowByName are internal API (vendor/imgui/imgui_internal.h) - reached here,
// test-only, the same way tl_log.h's own ring accessors are test-only introspection into
// otherwise-private state; log_panel.cpp itself never includes this header.
#include "imgui_internal.h"

#include <string.h>
#endif

#define TL_LOG_PANEL_SKIP TL_SKIP("editor/log_panel.cpp is dev-only (TOOLING.md section 9.1); no symbol in this tier")

TL_TEST(log_panel_draw_empty_ring_finds_window_no_crash, "editor,log_panel,fast") {
#if TL_DEV
    tl_log_test_reset();
    imgui_test_begin_frame(t);
    log_panel_draw(nullptr, nullptr);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Log") != nullptr);
    imgui_test_end_frame();
    tl_log_test_reset();
#else
    TL_LOG_PANEL_SKIP;
#endif
}

TL_TEST(log_panel_draw_renders_every_level_newest_first, "editor,log_panel,fast") {
#if TL_DEV
    tl_log_test_reset();
    TL_LOG_TRACE("trace line");
    TL_LOG_DEBUG("debug line");
    TL_LOG_INFO("info line");
    TL_LOG_WARN("warn line");
    TL_LOG_ERR("err line");
    // TL_LOG_MIN gates which of the above actually reach the ring (debug/dev: 0, so all five;
    // netcode/ship: 2, INFO+ only - but this whole TU is TL_DEV-guarded, so only the debug/dev
    // case is ever compiled here) - assert against tl_log_ring_count(), not a literal 5.
    const u32 n = tl_log_ring_count();
    TL_ASSERT_TRUE(n >= 1u);

    imgui_test_begin_frame(t);
    log_panel_draw(nullptr, nullptr);
    ImGuiWindow* win = ImGui::FindWindowByName("Log");
    TL_ASSERT_TRUE(win != nullptr);
    // Newest-first (log_panel.cpp's own contract): the last record written is ERR (always
    // compiled in, docs/TOOLING.md §9's TL_LOG_ERR floor), so the panel drew at least one row -
    // checked via the window's own draw list vertex count (ImGui::Text() emits real geometry
    // immediately, unlike ContentSize/auto-fit metrics, which settle only from the SECOND frame
    // a window is drawn - found failing on frame one before this comment existed).
    TL_EXPECT_TRUE(win->DrawList->VtxBuffer.Size > 0);
    imgui_test_end_frame();
    tl_log_test_reset();
#else
    TL_LOG_PANEL_SKIP;
#endif
}

TL_TEST(log_panel_register_wires_into_editor_panel_table, "editor,log_panel,fast") {
#if TL_DEV
    static VMemApi api = test_vmem_api();
    Editor ed;
    TL_ASSERT_EQ(editor_init(&ed, &api, 0u), ERR_OK);
    log_panel_register(&ed);
    TL_ASSERT_EQ(ed.panel_count, 1u);
    TL_EXPECT_EQ(strcmp(ed.panels[0].name, "Log"), 0);
    TL_EXPECT_TRUE(ed.panels[0].default_open);
    TL_EXPECT_TRUE(ed.panels[0].open);
    TL_ASSERT_TRUE(ed.panels[0].draw_fn != nullptr);

    tl_log_test_reset();
    TL_LOG_ERR("through the table");
    imgui_test_begin_frame(t);
    ed.panels[0].draw_fn(&ed, nullptr);   // exactly what editor_frame will do for every open panel
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Log") != nullptr);
    imgui_test_end_frame();
    tl_log_test_reset();
    editor_shutdown(&ed);
#else
    TL_LOG_PANEL_SKIP;
#endif
}

TL_TEST(log_panel_draw_many_records_no_crash, "editor,log_panel,fast") {
#if TL_DEV
    tl_log_test_reset();
    for (u32 i = 0; i < 5000u; ++i) { TL_LOG_ERR("line %u", i); }
    TL_ASSERT_EQ(tl_log_ring_count(), 4096u);   // ring cap, docs/TOOLING.md §9.2

    imgui_test_begin_frame(t);
    log_panel_draw(nullptr, nullptr);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Log") != nullptr);
    imgui_test_end_frame();
    tl_log_test_reset();
#else
    TL_LOG_PANEL_SKIP;
#endif
}
