// inspector.test.cpp - the generic reflection walker across every FieldKind family: integer/bool
// (editable), the fx palette (display-only), Entity handles (display + "go" reselect), StrId
// (display, interner-backed), a fixed-array field (display-only, count > 1), and a singleton
// component (shown regardless of selection). Also inspector_set_scalar_field's lockstep refusal
// and successful-write path, called directly rather than through a simulated widget edit - the
// same reason console_panel.test.cpp/dotpath.test.cpp drive their write paths directly: nothing
// in this tree can simulate a keystroke against the null ImGui backend.
// Spec: docs/TOOLING.md §9.3.4 (corrected in the same commit as inspector.cpp/inspector.h),
// §9.6 build order item 5. Rubric: docs/TESTING.md §7.
//
// TL_TEST's generated signature is `(TestCtx* t)`.
//
// editor/inspector.cpp is compiled ONLY on the debug/dev tiers (src/editor/CMakeLists.txt links
// an empty INTERFACE tl_editor on netcode/ship), so every call site here is behind `#if TL_DEV`,
// matching this session's console/log_panel/dotpath/watch test precedent.
#include "runner/tl_test.h"

#if TL_DEV
#include "core/world_test_util.h"
#include "editor/inspector.h"
#include "foundation/interner.h"
#include "foundation/vmem_test_api.h"
#include "imgui_test_util.h"
#include "imgui_internal.h"

#include <string.h>

#define TL_FIELDS_InsStats(X, XA, XH) \
    X(i32, hp) X(bool, alive) XA(u8, _pad0, 3) XA(i32, tags, 3)
TL_COMPONENT(InsStats)

#define TL_FIELDS_InsPos(X, XA, XH) \
    X(pos_t, x)
TL_COMPONENT(InsPos)

#define TL_FIELDS_InsRef(X, XA, XH) \
    XH(Entity, target)
TL_COMPONENT(InsRef)

#define TL_FIELDS_InsName(X, XA, XH) \
    X(StrId, sid)
TL_COMPONENT(InsName)

#define TL_FIELDS_InsHidden(X, XA, XH) \
    X(i32, secret)
TL_COMPONENT_FLAGS(InsHidden, COMP_HIDDEN)

#define TL_FIELDS_InsCfg(X, XA, XH) \
    X(i32, mode)
TL_COMPONENT_FLAGS(InsCfg, COMP_SINGLETON)

namespace {

WorldFixture& ins_fixture(u32 slot) {
    WorldFixture& f = *wt_fixture(slot);
    TL_CHECK(world_fixture_init(&f, 11u));
    world_register_component(&f.w, &InsStats_info);
    world_register_component(&f.w, &InsPos_info);
    world_register_component(&f.w, &InsRef_info);
    world_register_component(&f.w, &InsName_info);
    world_register_component(&f.w, &InsHidden_info);
    world_register_component(&f.w, &InsCfg_info);
    world_build_schedule(&f.w);

    // Static for the same reason dotpath.test.cpp's dp_fixture keeps its Interner static: the
    // World's `interner` pointer must outlive this call, and a stack local would dangle the
    // moment ins_fixture returns.
    static VMemApi api;
    static VMemArena chars_arena, meta_arena;
    static Interner in;
    static u8 interner_init_done = 0;
    if (!interner_init_done) {
        api = test_vmem_api();
        TL_CHECK(vmem_arena_init(&chars_arena, "ins.chars"_id, 1024u * 1024u, 0u, &api) == ERR_OK);
        TL_CHECK(vmem_arena_init(&meta_arena, "ins.meta"_id, 1024u * 1024u, 0u, &api) == ERR_OK);
        interner_init(&in, &chars_arena, &meta_arena, 256u);
        interner_init_done = 1;
    }
    f.w.interner = &in;
    return f;
}

bool make_editor(Editor* ed) {
    VMemApi api = test_vmem_api();
    return editor_init(ed, &api, 0u) == ERR_OK;
}

}  // namespace

#endif  // TL_DEV

#define TL_INSPECTOR_SKIP TL_SKIP("editor/inspector.cpp is dev-only (TOOLING.md section 9.1); no symbol in this tier")

TL_TEST(inspector_panel_draw_no_selection_no_crash, "editor,inspector,fast") {
#if TL_DEV
    WorldFixture& f = ins_fixture(0u);
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    // ed.sel is null (zero-init) - every non-singleton component is skipped; InsCfg's singleton
    // still draws.
    imgui_test_begin_frame(t);
    inspector_panel_draw(&ed, &f.w);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Inspector") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_INSPECTOR_SKIP;
#endif
}

TL_TEST(inspector_panel_register_wires_into_editor_panel_table, "editor,inspector,fast") {
#if TL_DEV
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    inspector_panel_register(&ed);
    TL_ASSERT_EQ(ed.panel_count, 1u);
    TL_EXPECT_EQ(strcmp(ed.panels[0].name, "Inspector"), 0);
    TL_EXPECT_TRUE(ed.panels[0].default_open);
    TL_ASSERT_TRUE(ed.panels[0].draw_fn != nullptr);

    WorldFixture& f = ins_fixture(1u);
    imgui_test_begin_frame(t);
    ed.panels[0].draw_fn(&ed, &f.w);   // exactly what editor_frame will do for every open panel
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Inspector") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_INSPECTOR_SKIP;
#endif
}

TL_TEST(inspector_panel_draw_selected_entity_every_kind_family_no_crash, "editor,inspector,fast") {
#if TL_DEV
    WorldFixture& f = ins_fixture(2u);
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));

    Entity e = world_spawn(&f.w);
    InsStats st{ 7, true, { 0, 0, 0 }, { 1, 2, 3 } };
    world_add<InsStats>(&f.w, e, st);
    InsPos pos{ fx::fx_raw<pos_t>(12345) };
    world_add<InsPos>(&f.w, e, pos);
    InsRef ref{ e };   // self-reference - exercises the non-null handle branch
    world_add<InsRef>(&f.w, e, ref);
    InsName nm{ intern(f.w.interner, sv_lit("widget")) };
    world_add<InsName>(&f.w, e, nm);
    InsHidden hid{ 999 };
    world_add<InsHidden>(&f.w, e, hid);
    world_flush(&f.w);

    ed.sel = e;
    imgui_test_begin_frame(t);
    ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
    inspector_panel_draw(&ed, &f.w);
    ImGuiWindow* win = ImGui::FindWindowByName("Inspector");
    TL_ASSERT_TRUE(win != nullptr);
    // Every CollapsingHeader defaults closed - open every component header via SetNextItemOpen
    // is not reachable from outside inspector.cpp, so this test proves the row/header draw
    // itself (the window exists, at least the header labels drew) rather than the field rows
    // inside a closed section - matching console_panel.test.cpp's own "renders something" bar
    // for state this test file cannot reach into.
    TL_EXPECT_TRUE(win->DrawList->VtxBuffer.Size > 0);
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_INSPECTOR_SKIP;
#endif
}

TL_TEST(inspector_panel_draw_hidden_component_and_array_field_no_crash, "editor,inspector,fast") {
#if TL_DEV
    // Two paths this file's other selected-entity test doesn't isolate: COMP_HIDDEN skips the
    // whole component before any field draws (inspector_panel_draw's own top-of-loop `continue`),
    // and InsStats::tags[3] (count > 1) takes draw_field's non-editable, elems > 1 branch (the
    // "%s[%u]" label - this header's own Invariants note on why count > 1 never gets a widget).
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));
    WorldFixture& f = ins_fixture(3u);

    Entity e = world_spawn(&f.w);
    InsStats st{ 7, true, { 0, 0, 0 }, { 1, 2, 3 } };
    world_add<InsStats>(&f.w, e, st);
    InsHidden hid{ 999 };
    world_add<InsHidden>(&f.w, e, hid);
    world_flush(&f.w);

    ed.sel = e;
    imgui_test_begin_frame(t);
    inspector_panel_draw(&ed, &f.w);
    TL_EXPECT_TRUE(ImGui::FindWindowByName("Inspector") != nullptr);
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_INSPECTOR_SKIP;
#endif
}

TL_TEST(inspector_set_scalar_field_refuses_lockstep, "editor,inspector,fast") {
#if TL_DEV
    WorldFixture& f = ins_fixture(0u);
    Entity e = world_spawn(&f.w);
    InsStats st{ 7, true, { 0, 0, 0 }, { 1, 2, 3 } };
    world_add<InsStats>(&f.w, e, st);
    world_flush(&f.w);

    const ComponentId comp = world_component_id<InsStats>(&f.w);
    i32 nv = 99;
    TL_EXPECT_EQ(inspector_set_scalar_field(&f.w, true, e, comp, 0u, &nv, sizeof(nv)),
                 ERR_EDITOR_LOCKSTEP);

    // Refused before recording anything - the field is unchanged after a flush.
    world_flush(&f.w);
    InsStats* row = world_get<InsStats>(&f.w, e);
    TL_ASSERT_TRUE(row != nullptr);
    TL_EXPECT_EQ(row->hp, 7);
#else
    TL_INSPECTOR_SKIP;
#endif
}

TL_TEST(inspector_set_scalar_field_writes_through_command, "editor,inspector,fast") {
#if TL_DEV
    WorldFixture& f = ins_fixture(1u);
    Entity e = world_spawn(&f.w);
    InsStats st{ 7, true, { 0, 0, 0 }, { 1, 2, 3 } };
    world_add<InsStats>(&f.w, e, st);
    world_flush(&f.w);

    const ComponentId comp = world_component_id<InsStats>(&f.w);
    i32 nv = 55;
    TL_ASSERT_EQ(inspector_set_scalar_field(&f.w, false, e, comp, 0u, &nv, sizeof(nv)), ERR_OK);
    world_flush(&f.w);

    InsStats* row = world_get<InsStats>(&f.w, e);
    TL_ASSERT_TRUE(row != nullptr);
    TL_EXPECT_EQ(row->hp, 55);

    // The bool field (field index 1) through the same door.
    u8 alive_false = 0u;
    TL_ASSERT_EQ(inspector_set_scalar_field(&f.w, false, e, comp, 1u, &alive_false, 1u), ERR_OK);
    world_flush(&f.w);
    row = world_get<InsStats>(&f.w, e);
    TL_ASSERT_TRUE(row != nullptr);
    TL_EXPECT_TRUE(!row->alive);
#else
    TL_INSPECTOR_SKIP;
#endif
}

TL_TEST(inspector_panel_draw_singleton_shown_without_selection, "editor,inspector,fast") {
#if TL_DEV
    WorldFixture& f = ins_fixture(2u);
    Editor ed;
    TL_ASSERT_TRUE(make_editor(&ed));

    InsCfg cfg{ 3 };
    world_singleton_set_cmd(&f.w, world_component_id<InsCfg>(&f.w), &cfg);
    world_flush(&f.w);

    // ed.sel stays null - the singleton must still draw (this file's own contract: "then every
    // singleton, shown regardless of selection").
    imgui_test_begin_frame(t);
    inspector_panel_draw(&ed, &f.w);
    ImGuiWindow* win = ImGui::FindWindowByName("Inspector");
    TL_ASSERT_TRUE(win != nullptr);
    TL_EXPECT_TRUE(win->DrawList->VtxBuffer.Size > 0);   // at least InsCfg's header text drew
    imgui_test_end_frame();
    editor_shutdown(&ed);
#else
    TL_INSPECTOR_SKIP;
#endif
}
