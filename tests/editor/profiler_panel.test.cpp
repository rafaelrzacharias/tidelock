// profiler_panel.test.cpp - profiler_panel_draw runs headless (docs/TOOLING.md §9.5) across
// empty-ring, one-frame, and paused-view-frame state, including B-1's acceptance test (a paused
// panel holds the same ABSOLUTE frame while the ring advances underneath it); profiler_panel_
// register wires into Editor's panel table.
// Spec: docs/TOOLING.md §9.1 (profiler_panel.cpp's row), §9.3.1, §9.4, §9.6 build order item 5.
//
// TL_TEST's generated signature is `(TestCtx* t)`.
//
// editor/profiler_panel.cpp is compiled ONLY on the debug/dev tiers (src/editor/CMakeLists.txt
// links an empty INTERFACE tl_editor on netcode/ship), so every call site here is behind
// `#if TL_DEV`, matching this session's log_panel/console_panel/inspector test precedent. The
// pause checkbox's own transition (headless has no way to simulate the click that flips it) is
// not exercised directly - this file drives `ed->prof_paused`/`prof_view_frame`/
// `prof_view_frame_valid` as persisted state instead (exactly what the checkbox's write-back
// would leave behind), and proves DRAWING and re-drawing that state does what the panel promises:
// no crash, renders something, and - the B-1 fix - keeps showing the SAME frame across draws
// while `tl_prof_frame_end` keeps advancing the ring underneath it.
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
    static VMemApi api = test_vmem_api();
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

// B-3 (M5): FindWindowByName != nullptr and VtxBuffer.Size > 0 hold even if the counter table is
// never drawn (a real mutation this session watched survive against the pre-B-3 tests) - and
// VtxBuffer.Size itself turned out too noisy to threshold reliably (window border/scrollbar
// vertices fluctuate a few dozen either way between draws, independent of content). Nothing in
// this tree can read rendered TEXT back out of an ImDrawList (it rasterizes glyphs, it does not
// keep strings), but `ImGuiWindow::ContentSize.y` - the auto-fit height ImGui computes from that
// frame's actual cursor travel at `End()` - is exact: two more lines (a "counters" header + one
// row) is a fixed, measured height addition, not a fuzzy vertex count. Isolates the counter
// section's own contribution by comparing it with and without a counter present, everything else
// (the same recorded frame, same node tree) held equal between the two draws.
TL_TEST(profiler_panel_draw_renders_the_counter_table, "editor,profiler_panel,fast") {
#if TL_DEV
    tl_prof_test_reset();
    tl_prof_begin(0, "s"_id, "s", 0xFFFFFFFFu);
    tl_prof_end(0);
    tl_prof_frame_end(1u);
    TL_ASSERT_EQ(tl_prof_ring_count(), 1u);
    TL_ASSERT_EQ(tl_prof_counter_count(), 0u);

    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));

    // Two draws per condition: ImGui's own window auto-resize/border metrics only settle from the
    // SECOND frame a window is drawn (this file's own established note, profiler_panel_draw_one_
    // frame_with_nested_scopes_and_counter above). Warm up, THEN measure, for both the no-counter
    // and the with-counter condition.
    imgui_test_begin_frame(t);
    profiler_panel_draw(&ed, nullptr);
    imgui_test_end_frame();
    imgui_test_begin_frame(t);
    profiler_panel_draw(&ed, nullptr);
    ImGuiWindow* win_before = ImGui::FindWindowByName("Profiler");
    TL_ASSERT_TRUE(win_before != nullptr);
    const float content_h_before = win_before->ContentSize.y;
    imgui_test_end_frame();

    tl_prof_counter("draw_calls"_id, "draw_calls", 61, 0u);   // registered, not re-recorded into a
                                                                // new frame - tl_prof_counter_count/
                                                                // _at read the live registered set
                                                                // directly (prof.cpp), not a
                                                                // per-frame snapshot
    TL_ASSERT_EQ(tl_prof_counter_count(), 1u);

    imgui_test_begin_frame(t);
    profiler_panel_draw(&ed, nullptr);
    imgui_test_end_frame();
    imgui_test_begin_frame(t);
    profiler_panel_draw(&ed, nullptr);
    ImGuiWindow* win_after = ImGui::FindWindowByName("Profiler");
    TL_ASSERT_TRUE(win_after != nullptr);
    const float content_h_after = win_after->ContentSize.y;
    imgui_test_end_frame();

    // Measured: unfixed (counter table skipped) leaves content height UNCHANGED (75.0 -> 75.0);
    // correct code adds the "counters" header + one row (75.0 -> 114.0, +39). See PR comment.
    TL_EXPECT_TRUE(content_h_after > content_h_before);

    editor_shutdown(&ed);
    tl_prof_test_reset();
#else
    TL_PROFILER_PANEL_SKIP;
#endif
}

TL_TEST(profiler_panel_draw_paused_view_frame_no_crash, "editor,profiler_panel,fast") {
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
    // What an already-paused panel's persisted state looks like, driven directly (this file's own
    // header note explains why - no simulated checkbox/widget drag against the null backend).
    // frame 0 is the OLDEST of the three (ProfFrame::frame is absolute, assigned in record order).
    ed.prof_paused = 1u;
    ed.prof_view_frame = 0u;
    ed.prof_view_frame_valid = 1u;

    imgui_test_begin_frame(t);
    profiler_panel_draw(&ed, nullptr);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Profiler") != nullptr);
    imgui_test_end_frame();
    TL_EXPECT_EQ(ed.prof_view_slot, 2u);   // frame 0 is 2 slots back from the newest (frame 2)
    editor_shutdown(&ed);
    tl_prof_test_reset();
#else
    TL_PROFILER_PANEL_SKIP;
#endif
}

TL_TEST(profiler_panel_pause_holds_the_same_frame_while_ring_advances, "editor,profiler_panel,fast") {
#if TL_DEV
    // The B-1 acceptance test: a paused panel must keep showing the SAME frame while the ring
    // advances underneath it (prof.cpp has no pause concept of its own - nothing stops it). Before
    // the fix, the panel tracked a `slots_back` OFFSET, which named a different frame every draw;
    // watched this exact test fail against that code (see PR comment for the real output).
    tl_prof_test_reset();
    tl_prof_begin(0, "s"_id, "s", 0xFFFFFFFFu);
    tl_prof_end(0);
    tl_prof_frame_end(102u);
    TL_ASSERT_EQ(tl_prof_ring_count(), 1u);

    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    ed.prof_paused = 1u;
    ed.prof_view_frame_valid = 0u;   // freshly paused - the panel must pin fresh on this draw

    imgui_test_begin_frame(t);
    profiler_panel_draw(&ed, nullptr);
    imgui_test_end_frame();
    TL_ASSERT_TRUE(ed.prof_view_frame_valid != 0u);
    const u64 pinned_frame = ed.prof_view_frame;
    TL_ASSERT_EQ(ed.prof_view_slot, 0u);
    TL_EXPECT_EQ(tl_prof_ring_at(ed.prof_view_slot)->tick, 102u);

    // The ring advances underneath the still-paused panel.
    tl_prof_begin(0, "s"_id, "s", 0xFFFFFFFFu);
    tl_prof_end(0);
    tl_prof_frame_end(999u);
    TL_ASSERT_EQ(tl_prof_ring_count(), 2u);

    imgui_test_begin_frame(t);
    profiler_panel_draw(&ed, nullptr);
    imgui_test_end_frame();
    TL_EXPECT_EQ(ed.prof_view_frame, pinned_frame);   // still the SAME absolute frame
    TL_EXPECT_EQ(ed.prof_view_slot, 1u);              // now one further back in the ring
    TL_EXPECT_EQ(tl_prof_ring_at(ed.prof_view_slot)->tick, 102u);   // still showing tick 102, not 999

    editor_shutdown(&ed);
    tl_prof_test_reset();
#else
    TL_PROFILER_PANEL_SKIP;
#endif
}

TL_TEST(profiler_panel_draw_paused_frame_aged_out_falls_back_to_oldest_live, "editor,profiler_panel,fast") {
#if TL_DEV
    // A pinned frame that no longer exists in the ring (evicted past PROF_RING_FRAMES, or -
    // exercised here - simply never recorded) must not be handed to tl_prof_ring_at as a raw
    // index: this exercises profiler_panel_draw's fallback-to-oldest-live-frame path.
    tl_prof_test_reset();
    tl_prof_begin(0, "solo"_id, "solo", 0xFFFFFFFFu);
    tl_prof_end(0);
    tl_prof_frame_end(1u);
    TL_ASSERT_EQ(tl_prof_ring_count(), 1u);

    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    ed.prof_paused = 1u;
    ed.prof_view_frame = 99u;   // a frame number that was never recorded
    ed.prof_view_frame_valid = 1u;

    imgui_test_begin_frame(t);
    profiler_panel_draw(&ed, nullptr);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Profiler") != nullptr);
    imgui_test_end_frame();
    TL_EXPECT_EQ(ed.prof_view_slot, 0u);       // fell back to the oldest (only) live frame
    TL_EXPECT_EQ(ed.prof_view_frame, 0u);      // re-pinned to that frame's real absolute number
    editor_shutdown(&ed);
    tl_prof_test_reset();
#else
    TL_PROFILER_PANEL_SKIP;
#endif
}
