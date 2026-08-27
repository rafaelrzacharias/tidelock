// world_panel.test.cpp - world_panel_draw runs headless (docs/TOOLING.md §9.5) across an empty
// world, a populated one with live and dead (destroyed) entity slots, and the arena-rehash write
// path driven directly.
// Spec: docs/TOOLING.md §9.1 (world_panel.cpp's row), §9.4, §9.6 build order item 5.
//
// TL_TEST's generated signature is `(TestCtx* t)`.
//
// editor/world_panel.cpp is compiled ONLY on the debug/dev tiers (src/editor/CMakeLists.txt links
// an empty INTERFACE tl_editor on netcode/ship), so every call site here is behind `#if TL_DEV`,
// matching this session's log_panel/console_panel/inspector/profiler_panel/probes_panel test
// precedent. An entity row's "select" button can't be simulated (nothing in this tree can drive
// a real click against the null ImGui backend), so this file checks `ed->sel` directly by
// spawning a known entity and setting `sel` itself, the same as every other panel's "drive the
// write path directly" pattern - `world_panel_rehash_arenas` (the "rehash arenas" button's own
// body) is exposed for exactly this reason and called directly below.
#include "runner/tl_test.h"

#if TL_DEV
#include "core/world_test_util.h"
#include "editor/world_panel.h"
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

#define TL_WORLD_PANEL_SKIP TL_SKIP("editor/world_panel.cpp is dev-only (TOOLING.md section 9.1); no symbol in this tier")

TL_TEST(world_panel_draw_empty_world_no_crash, "editor,world_panel,fast") {
#if TL_DEV
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 21u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);

    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    imgui_test_begin_frame(t);
    world_panel_draw(&ed, &f.w);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("World") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_WORLD_PANEL_SKIP;
#endif
}

TL_TEST(world_panel_draw_null_world_no_crash, "editor,world_panel,fast") {
#if TL_DEV
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    imgui_test_begin_frame(t);
    world_panel_draw(&ed, nullptr);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("World") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_WORLD_PANEL_SKIP;
#endif
}

TL_TEST(world_panel_register_wires_into_editor_panel_table, "editor,world_panel,fast") {
#if TL_DEV
    WorldFixture& f = *wt_fixture(1u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 22u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);

    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    world_panel_register(&ed);
    TL_ASSERT_EQ(ed.panel_count, 1u);
    TL_EXPECT_EQ(strcmp(ed.panels[0].name, "World"), 0);
    TL_EXPECT_TRUE(ed.panels[0].default_open);
    TL_ASSERT_TRUE(ed.panels[0].draw_fn != nullptr);

    imgui_test_begin_frame(t);
    ed.panels[0].draw_fn(&ed, &f.w);   // exactly what editor_frame will do for every open panel
    TL_EXPECT_TRUE(ImGui::FindWindowByName("World") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_WORLD_PANEL_SKIP;
#endif
}

TL_TEST(world_panel_draw_live_and_dead_slots_and_singleton_no_crash, "editor,world_panel,fast") {
#if TL_DEV
    WorldFixture& f = *wt_fixture(2u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 23u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);

    Entity a = world_spawn(&f.w);
    Entity b = world_spawn(&f.w);
    Entity c = world_spawn(&f.w);
    WPos pos{ 1, 2 };
    world_add<WPos>(&f.w, a, pos);
    world_flush(&f.w);
    world_destroy(&f.w, b);   // b becomes a dead slot inside the walked range
    world_flush(&f.w);
    WCfg cfg{ 9, 0, { 0, 0, 0, 0 } };
    world_singleton_set_cmd(&f.w, world_component_id<WCfg>(&f.w), &cfg);
    world_flush(&f.w);

    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    ed.sel = a;   // what the row's own "select" button would set
    imgui_test_begin_frame(t);
    world_panel_draw(&ed, &f.w);
    ImGuiWindow* win = ImGui::FindWindowByName("World");
    TL_ASSERT_TRUE(win != nullptr);
    TL_EXPECT_TRUE(win->DrawList->VtxBuffer.Size > 0);
    imgui_test_end_frame();
    editor_shutdown(&ed);
    TL_EXPECT_EQ(ed.sel.bits, a.bits);
    (void)c;
#else
    TL_WORLD_PANEL_SKIP;
#endif
}

TL_TEST(world_panel_rehash_arenas_computes_and_flags_changes, "editor,world_panel,fast") {
#if TL_DEV
    WorldFixture& f = *wt_fixture(3u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 24u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);
    TL_ASSERT_TRUE(f.w.registry->count > 0u);   // world_init/register_component already added arenas
    registry_seal(f.w.registry);   // app/'s job in real usage (not built) - this test's own fixture
                                     // registry is a synthesized stand-in, same shape as ins_fixture's
                                     // synthesized Interner elsewhere in this session's own tests

    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    TL_EXPECT_TRUE(!ed.world_arena_hash_valid);

    world_panel_rehash_arenas(&ed, &f.w);
    TL_ASSERT_TRUE(ed.world_arena_hash_valid);
    TL_EXPECT_TRUE(!ed.world_arena_hash_have_prev);   // first click: nothing to diff against yet
    const u32 n = f.w.registry->count;
    u64 first[MAX_ARENAS];
    memcpy(first, ed.world_arena_hash_cur, sizeof(u64) * n);

    // Mutate registered state between clicks so at least one HASHED arena's [base,used) actually
    // changes - WCfg is a singleton, so its write lands in WCfg's own dense_arena (`world.cpp`'s
    // COMP_SINGLETON branch registers ONE arena per singleton component, ARENA_HASHED |
    // ARENA_SNAPSHOT), a distinct, later-registered arena from "world.singletons" (which only
    // holds `WorldTickState` - this test does not assume a specific index, only that SOME index
    // moved, since the exact registration order is `world_init`'s/`world_register_component`'s
    // business, not this panel's).
    WCfg cfg{ 5, 0, { 0, 0, 0, 0 } };
    world_singleton_set_cmd(&f.w, world_component_id<WCfg>(&f.w), &cfg);
    world_flush(&f.w);

    world_panel_rehash_arenas(&ed, &f.w);
    TL_ASSERT_TRUE(ed.world_arena_hash_have_prev);
    u32 changed = 0u;
    for (u32 i = 0; i < n; ++i) {
        TL_EXPECT_EQ(ed.world_arena_hash_prev[i], first[i]);
        if (ed.world_arena_hash_cur[i] != first[i]) { changed += 1u; }
    }
    TL_EXPECT_TRUE(changed > 0u);   // at least one arena's hash moved between the two clicks

    imgui_test_begin_frame(t);
    world_panel_draw(&ed, &f.w);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("World") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_WORLD_PANEL_SKIP;
#endif
}

TL_TEST(world_panel_rehash_arenas_noops_on_unsealed_registry, "editor,world_panel,fast") {
#if TL_DEV
    // world_fixture_init/world_fixture_register_std never call registry_seal (sealing is app/'s
    // job, not built - world_panel.h's own Invariants note) - this is the REAL shape every world
    // this lane's editor can reach today has, so the panel must not TL_FATAL over it.
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 25u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);
    TL_ASSERT_EQ(f.w.registry->sealed, (u8)0u);

    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    world_panel_rehash_arenas(&ed, &f.w);   // must return quietly, not TL_FATAL
    TL_EXPECT_TRUE(!ed.world_arena_hash_valid);

    imgui_test_begin_frame(t);
    world_panel_draw(&ed, &f.w);   // the button itself must not even be reachable here
    TL_EXPECT_TRUE(ImGui::FindWindowByName("World") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_WORLD_PANEL_SKIP;
#endif
}
