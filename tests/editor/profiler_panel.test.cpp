// profiler_panel.test.cpp - profiler_panel_draw runs headless (docs/TOOLING.md §9.5) across
// empty-ring, one-frame, and paused-view-slot state; profiler_panel_register wires into Editor's
// panel table.
// Spec: docs/TOOLING.md §9.1 (profiler_panel.cpp's row), §9.3.1, §9.4, §9.6 build order item 5.
//
// TL_TEST's generated signature is `(TestCtx* t)`.
//
// editor/profiler_panel.cpp is compiled ONLY on the debug/dev tiers (src/editor/CMakeLists.txt
// links an empty INTERFACE tl_editor on netcode/ship), so every call site here is behind
// `#if TL_DEV`, matching this session's log_panel/console_panel/inspector test precedent. The
// pause/slider interaction (SliderInt's return value) is not exercised the same way the "Enter
// submits" gap is noted in console_panel.test.cpp - nothing in this tree can simulate a real
// widget drag against the null ImGui backend, so this file drives `ed->prof_paused`/
// `prof_view_slot` directly (what the slider would write) and proves DRAWING that state doesn't
// crash and renders something.
#include "runner/tl_test.h"

#if TL_DEV
#include "editor/editor.h"
#include "editor/profiler_panel.h"
#include "foundation/tl_prof.h"
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

#define TL_PROFILER_PANEL_SKIP TL_SKIP("editor/profiler_panel.cpp is dev-only (TOOLING.md section 9.1); no symbol in this tier")

TL_TEST(profiler_panel_draw_empty_ring_no_crash, "editor,profiler_panel,fast") {
#if TL_DEV
    tl_prof_test_reset();
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    imgui_test_begin_frame(t);
    profiler_panel_draw(&ed, nullptr);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Profiler") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
    tl_prof_test_reset();
#else
    TL_PROFILER_PANEL_SKIP;
#endif
}

TL_TEST(profiler_panel_register_wires_into_editor_panel_table, "editor,profiler_panel,fast") {
#if TL_DEV
    tl_prof_test_reset();
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    profiler_panel_register(&ed);
    TL_ASSERT_EQ(ed.panel_count, 1u);
    TL_EXPECT_EQ(strcmp(ed.panels[0].name, "Profiler"), 0);
    TL_EXPECT_TRUE(ed.panels[0].default_open);
    TL_ASSERT_TRUE(ed.panels[0].draw_fn != nullptr);

    imgui_test_begin_frame(t);
    ed.panels[0].draw_fn(&ed, nullptr);   // exactly what editor_frame will do for every open panel
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Profiler") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
    tl_prof_test_reset();
#else
    TL_PROFILER_PANEL_SKIP;
#endif
}

TL_TEST(profiler_panel_draw_one_frame_with_nested_scopes_and_counter, "editor,profiler_panel,fast") {
#if TL_DEV
    tl_prof_test_reset();
    tl_prof_begin(0, "outer"_id, "outer", 0xFFFFFFFFu);
    tl_prof_begin(0, "inner"_id, "inner", 7u);
    tl_prof_end(0);
    tl_prof_end(0);
    tl_prof_counter("draw_calls"_id, "draw_calls", 61, 0u);
    tl_prof_frame_end(1234u);
    TL_ASSERT_EQ(tl_prof_ring_count(), 1u);

    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    imgui_test_begin_frame(t);
    profiler_panel_draw(&ed, nullptr);
    ImGuiWindow* win = ImGui::FindWindowByName("Profiler");
    TL_ASSERT_TRUE(win != nullptr);
    TL_EXPECT_TRUE(win->DrawList->VtxBuffer.Size > 0);
    imgui_test_end_frame();
    editor_shutdown(&ed);
    tl_prof_test_reset();
#else
    TL_PROFILER_PANEL_SKIP;
#endif
}

TL_TEST(profiler_panel_draw_paused_view_slot_no_crash, "editor,profiler_panel,fast") {
#if TL_DEV
    tl_prof_test_reset();
    for (u32 i = 0; i < 3u; ++i) {
        tl_prof_begin(0, "frame_scope"_id, "frame_scope", 0xFFFFFFFFu);
        tl_prof_end(0);
        tl_prof_frame_end((u64)i);
    }
    TL_ASSERT_EQ(tl_prof_ring_count(), 3u);

    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    // What the panel's own pause checkbox + slider would write, driven directly (this file's own
    // header note explains why - no simulated widget drag against the null backend).
    ed.prof_paused = 1u;
    ed.prof_view_slot = 2u;   // the oldest of the three ring frames

    imgui_test_begin_frame(t);
    profiler_panel_draw(&ed, nullptr);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Profiler") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
    tl_prof_test_reset();
#else
    TL_PROFILER_PANEL_SKIP;
#endif
}

TL_TEST(profiler_panel_draw_paused_slot_past_ring_count_clamped_no_crash, "editor,profiler_panel,fast") {
#if TL_DEV
    // A stale prof_view_slot (e.g. the ring shrank back to fewer live frames than a previous
    // pause point recorded - tl_prof_test_reset does exactly this) must not be handed to
    // tl_prof_ring_at as-is: this exercises profiler_panel_draw's own clamp-to-0 guard.
    tl_prof_test_reset();
    tl_prof_begin(0, "solo"_id, "solo", 0xFFFFFFFFu);
    tl_prof_end(0);
    tl_prof_frame_end(1u);
    TL_ASSERT_EQ(tl_prof_ring_count(), 1u);

    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    ed.prof_paused = 1u;
    ed.prof_view_slot = 99u;   // past the one live frame

    imgui_test_begin_frame(t);
    profiler_panel_draw(&ed, nullptr);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Profiler") != nullptr);
    imgui_test_end_frame();
    TL_EXPECT_EQ(ed.prof_view_slot, 0u);   // clamped, not left dangling past ring_count
    editor_shutdown(&ed);
    tl_prof_test_reset();
#else
    TL_PROFILER_PANEL_SKIP;
#endif
}
