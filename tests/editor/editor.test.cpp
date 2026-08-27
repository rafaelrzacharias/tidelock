// editor.test.cpp - panel registry: register/find/toggle, default_open seeding, the duplicate/
// full-table fatal preconditions (documented, not TL_TEST_EXPECT_FATAL'd - see the test body).
// Spec: docs/TOOLING.md §1, §9.1. Rubric: docs/TESTING.md §7.
//
// TL_TEST's generated signature is `(TestCtx* t)` - every Editor local here is `ed`.
//
// editor.cpp is compiled ONLY on the debug/dev tiers (src/editor/CMakeLists.txt links an empty
// INTERFACE tl_editor on netcode/ship - docs/TOOLING.md §9.1's file layout table), so - matching
// tests/editor/console.test.cpp's precedent - every call site here, including the shared
// `make_editor()` helper, is behind `#if TL_DEV`.
#include "runner/tl_test.h"
#include "editor/editor.h"
#include "foundation/vmem_test_api.h"

#include <string.h>

#if TL_DEV

namespace {
void draw_noop(Editor*, World*) {}

bool make_editor(Editor* ed) {
    static VMemApi api = test_vmem_api();
    return editor_init(ed, &api, 0u) == ERR_OK;
}
}  // namespace

#endif  // TL_DEV

#define TL_EDITOR_SKIP TL_SKIP("editor.cpp is dev-only (TOOLING.md section 9.1); no symbol in this tier")

TL_TEST(editor_init_zeroes_and_reserves_dev_arena, "editor,fast") {
#if TL_DEV
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    TL_EXPECT_EQ(ed.panel_count, 0u);
    TL_EXPECT_EQ(ed.sel.bits, 0u);
    TL_EXPECT_EQ(ed.capture_mask, 0u);
    TL_EXPECT_EQ(ed.initialized, (u8)1);
    TL_EXPECT_NOT_NULL(ed.dev_arena.base);
    editor_shutdown(&ed);
    TL_EXPECT_EQ(ed.initialized, (u8)0);
#else
    TL_EDITOR_SKIP;
#endif
}

TL_TEST(editor_register_panel_seeds_open_from_default, "editor,fast") {
#if TL_DEV
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    editor_register_panel(&ed, "Log", draw_noop, true);
    editor_register_panel(&ed, "Console", draw_noop, false);
    TL_ASSERT_EQ(ed.panel_count, 2u);
    TL_EXPECT_EQ(strcmp(ed.panels[0].name, "Log"), 0);
    TL_EXPECT_TRUE(ed.panels[0].open);
    TL_EXPECT_TRUE(ed.panels[0].default_open);
    TL_EXPECT_EQ(strcmp(ed.panels[1].name, "Console"), 0);
    TL_EXPECT_FALSE(ed.panels[1].open);
#else
    TL_EDITOR_SKIP;
#endif
}

TL_TEST(editor_toggle_panel_flips_open_state, "editor,fast") {
#if TL_DEV
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    editor_register_panel(&ed, "Log", draw_noop, false);
    TL_EXPECT_FALSE(ed.panels[0].open);
    editor_toggle_panel(&ed, "Log");
    TL_EXPECT_TRUE(ed.panels[0].open);
    editor_toggle_panel(&ed, "Log");
    TL_EXPECT_FALSE(ed.panels[0].open);
#else
    TL_EDITOR_SKIP;
#endif
}

TL_TEST(editor_register_panel_registers_up_to_max, "editor,fast") {
#if TL_DEV
    // The fatal preconditions (duplicate name, EDITOR_MAX_PANELS exhausted - registration-time
    // misconfiguration, matching every other registration door in the tree) are documented in
    // editor.h rather than exercised via TL_TEST_EXPECT_FATAL here; this proves the success path
    // registers exactly EDITOR_MAX_PANELS distinct panels without hitting either fatal early.
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    char name[16];
    for (u32 i = 0; i < (u32)EDITOR_MAX_PANELS; ++i) {
        name[0] = 'P'; name[1] = (char)('A' + (i % 26)); name[2] = '\0';
        editor_register_panel(&ed, name, draw_noop, false);
    }
    TL_EXPECT_EQ(ed.panel_count, (u32)EDITOR_MAX_PANELS);
#else
    TL_EDITOR_SKIP;
#endif
}
