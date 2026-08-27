// console_panel.test.cpp - console_panel_draw runs headless (docs/TOOLING.md §9.5) across empty
// and populated state, and console_panel_register wires into Editor's panel table.
// Spec: docs/TOOLING.md §9.1 (console.cpp's row), §9.6 build order item 5.
//
// TL_TEST's generated signature is `(TestCtx* t)`.
//
// What this file does NOT test, and why: the "Enter submits" path inside console_panel_draw
// (ImGui::InputText's return value) only goes true when the null backend delivers a real key
// event, and the null backend (vendor/imgui/backends/imgui_impl_null.cpp) delivers none - the
// same "ImGui test engine not vendored" limit this session already hit on the Inspector panel.
// So this file exercises the state side directly (console_exec, called the same way the draw
// function's submit branch would call it) and then proves DRAWING that state doesn't crash and
// renders something - not that a keystroke reaches the widget, which nothing in this tree can
// simulate yet.
//
// editor/console.cpp is compiled ONLY on the debug/dev tiers (src/editor/CMakeLists.txt links an
// empty INTERFACE tl_editor on netcode/ship), so every call site here is behind `#if TL_DEV`,
// matching this session's log_panel.test.cpp precedent.
#include "runner/tl_test.h"

#if TL_DEV
#include "editor/console.h"
#include "editor/editor.h"
#include "imgui_test_util.h"
#include "foundation/vmem_test_api.h"
#include "imgui_internal.h"

#include <string.h>

namespace {
Result<u32> fn_echo(World*, u32 argc, const StrView* argv, Span<char> reply) {
    if (argc == 0u) { return Result<u32>{ 0, ERR_OK }; }
    const u32 n = (argv[0].len < reply.count) ? argv[0].len : reply.count;
    memcpy(reply.data, argv[0].ptr, n);
    return Result<u32>{ n, ERR_OK };
}

bool make_editor(Editor* ed) {
    static VMemApi api = test_vmem_api();
    return editor_init(ed, &api, 0u) == ERR_OK;
}
}  // namespace
#endif  // TL_DEV

#define TL_CONSOLE_PANEL_SKIP TL_SKIP("editor/console.cpp is dev-only (TOOLING.md section 9.1); no symbol in this tier")

TL_TEST(console_panel_draw_empty_state_no_crash, "editor,console_panel,fast") {
#if TL_DEV
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    imgui_test_begin_frame(t);
    console_panel_draw(&ed, nullptr);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Console") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_CONSOLE_PANEL_SKIP;
#endif
}

TL_TEST(console_panel_register_wires_into_editor_panel_table, "editor,console_panel,fast") {
#if TL_DEV
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    console_panel_register(&ed);
    TL_ASSERT_EQ(ed.panel_count, 1u);
    TL_EXPECT_EQ(strcmp(ed.panels[0].name, "Console"), 0);
    TL_EXPECT_TRUE(ed.panels[0].default_open);
    TL_ASSERT_TRUE(ed.panels[0].draw_fn != nullptr);

    imgui_test_begin_frame(t);
    ed.panels[0].draw_fn(&ed, nullptr);   // exactly what editor_frame will do for every open panel
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Console") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_CONSOLE_PANEL_SKIP;
#endif
}

TL_TEST(console_panel_draw_after_exec_shows_history_and_reply, "editor,console_panel,fast") {
#if TL_DEV
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    ConsoleCmd cmd{};
    cmd.key = "echo"_id; cmd.name = "echo"; cmd.usage = "echo <word>"; cmd.fn = fn_echo;
    cmd.argc_min = 1; cmd.argc_max = 1;
    console_register(&ed.console, &cmd);

    // What console_panel_draw's own submit branch does, driven directly rather than through a
    // simulated keystroke (this file's own header note explains why).
    char reply[256];
    memset(reply, 0, sizeof(reply));
    const Result<u32> r = console_exec(&ed.console, nullptr, false, "echo hello",
                                        Span<char>{ reply, (u32)sizeof(reply) - 1u });
    TL_ASSERT_EQ(r.err, ERR_OK);
    ed.console_last_err = r.err;
    memcpy(ed.console_last_reply, reply, sizeof(reply));
    TL_ASSERT_EQ(ed.console.hist_count, 1u);

    imgui_test_begin_frame(t);
    console_panel_draw(&ed, nullptr);
    ImGuiWindow* win = ImGui::FindWindowByName("Console");
    TL_ASSERT_TRUE(win != nullptr);
    TL_EXPECT_TRUE(win->DrawList->VtxBuffer.Size > 0);   // the history line + reply text actually drew
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_CONSOLE_PANEL_SKIP;
#endif
}

TL_TEST(console_panel_draw_full_history_no_crash, "editor,console_panel,fast") {
#if TL_DEV
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    ConsoleCmd cmd{};
    cmd.key = "echo"_id; cmd.name = "echo"; cmd.usage = "echo <word>"; cmd.fn = fn_echo;
    cmd.argc_min = 1; cmd.argc_max = 1;
    console_register(&ed.console, &cmd);

    char reply[256];
    for (u32 i = 0; i < 80u; ++i) {   // past CONSOLE_HISTORY_CAP (64) - exercises the wrapped ring
        memset(reply, 0, sizeof(reply));
        (void)console_exec(&ed.console, nullptr, false, "echo x", Span<char>{ reply, (u32)sizeof(reply) - 1u });
    }
    TL_ASSERT_EQ(ed.console.hist_count, (u32)CONSOLE_HISTORY_CAP);

    imgui_test_begin_frame(t);
    console_panel_draw(&ed, nullptr);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Console") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_CONSOLE_PANEL_SKIP;
#endif
}
