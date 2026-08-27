// no_stray_alloc.test.cpp - docs/TOOLING.md §9.6's v0 done criterion, third clause: "zero heap
// allocation per frame outside pool_vendor". Runs every registered panel's real draw_fn
// repeatedly, across several imgui frames, over populated state, with NO vendor heap installed,
// and confirms the process does not crash.
//
// Why "did not crash" is the real signal, not a weaker stand-in: src/vendor_glue/vendor_new.cpp's
// own comment records a MEASURED fact about this exact binary's link line - `operator new`/
// `delete` resolve to vendor_new.cpp's pool-backed replacements (foundation/alloc_shim_ops.o is
// never even pulled into tl_tests, because vendor_new.cpp's occurrence in libtl_vendor_glue.a is
// scanned before foundation's first occurrence in every current link line). With no pool
// installed (`vendor_heap_current() == nullptr`, checked before and after every panel below),
// `vendor_alloc`/`vendor_free` TL_FATAL on ANY global `new`/`delete` - "the same message shape [a
// stray new from src/ code] dies exactly as before" (vendor_new.cpp's own words). So a clean run
// here is not "nothing happened to allocate" by luck; it is the same tripwire w3-render2d's own
// lane notes already relied on for the identical clause (TODO.md, "W3 render2d — lane notes",
// 2026-08-26: "every render test in this lane ran the real pipeline repeatedly with no tripwire
// fatal - the mechanism that exists is satisfied; there is no live counter to assert a number
// against"). ImGui itself never reaches this path at all - `vendor_glue/imgui_glue.cpp` routes
// every ImGui allocation through `pool_vendor()` directly via `ImGui::SetAllocatorFunctions`, the
// clause's own named exemption - so this file is checking exactly what "outside pool_vendor"
// means: no editor panel `.cpp` reaches for a plain `new`/`delete` on any of its own code paths.
//
// TL_TEST's generated signature is `(TestCtx* t)`.
//
// Every editor/*_panel.cpp file is compiled ONLY on the debug/dev tiers (src/editor/
// CMakeLists.txt links an empty INTERFACE tl_editor on netcode/ship), so this whole file is
// `#if TL_DEV`, matching this session's every other editor test file's precedent.
#include "runner/tl_test.h"

#if TL_DEV
#include "core/world_test_util.h"
#include "editor/inspector.h"
#include "editor/log_panel.h"
#include "editor/probes_panel.h"
#include "editor/profiler_panel.h"
#include "editor/world_panel.h"
#include "foundation/tl_log.h"
#include "foundation/tl_prof.h"
#include "foundation/tl_probe.h"
#include "foundation/vmem_test_api.h"
#include "imgui_test_util.h"
#include "vendor_glue/vendor_new.h"

// editor/console.h/.cpp's own panel lives in that header (console_panel_register/_draw), unlike
// the other five (each in its own <name>_panel.h) - console.h is included through editor.h
// transitively via editor/inspector.h's own include chain, but pulled in explicitly here too for
// clarity (matching console_panel.test.cpp's own include list).
#include "editor/console.h"
#endif  // TL_DEV

#define TL_NO_STRAY_ALLOC_SKIP TL_SKIP("editor panels are dev-only (TOOLING.md section 9.1); no symbol in this tier")

TL_TEST(editor_panels_draw_repeatedly_with_no_vendor_heap_installed, "editor,alloc,fast") {
#if TL_DEV
    TL_ASSERT_TRUE(vendor_heap_current() == nullptr);   // the precondition this test's whole
                                                          // argument depends on - see file header

    tl_log_test_reset();
    tl_prof_test_reset();
    tl_probe_test_reset();

    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 31u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);

    Entity e = world_spawn(&f.w);
    WPos pos{ 1, 2 };
    world_add<WPos>(&f.w, e, pos);
    world_flush(&f.w);
    WCfg cfg{ 7, 0, { 0, 0, 0, 0 } };
    world_singleton_set_cmd(&f.w, world_component_id<WCfg>(&f.w), &cfg);
    world_flush(&f.w);

    TL_LOG_ERR("no_stray_alloc: seed record");
    tl_prof_begin(0, "outer"_id, "outer", 0xFFFFFFFFu);
    tl_prof_end(0);
    tl_prof_frame_end(1u);
    tl_probe_mark("seed"_id, "seed");

    // B-9 (2026-08-27): a plain local, not `static` - `api` and `ed` both die at this body's
    // closing brace (RR-41's own carve-out, vmem_arena.h: "a plain local inside one test body,
    // whose arena dies at the same closing brace, is fine" - unlike the six make_editor()
    // helpers, which correctly stay `static` because THEY return while the arena lives on).
    VMemApi api = test_vmem_api();
    Editor ed;
    TL_ASSERT_TRUE(editor_init(&ed, &api, 0u) == ERR_OK);
    log_panel_register(&ed);
    console_panel_register(&ed);
    inspector_panel_register(&ed);
    profiler_panel_register(&ed);
    probes_panel_register(&ed);
    world_panel_register(&ed);
    ed.sel = e;

    for (u32 frame = 0; frame < 20u; ++frame) {
        imgui_test_begin_frame(t);
        for (u32 p = 0; p < ed.panel_count; ++p) { ed.panels[p].draw_fn(&ed, &f.w); }
        imgui_test_end_frame();
    }

    TL_EXPECT_TRUE(vendor_heap_current() == nullptr);   // nothing installed one mid-run either
    editor_shutdown(&ed);
    tl_log_test_reset();
    tl_prof_test_reset();
    tl_probe_test_reset();
#else
    TL_NO_STRAY_ALLOC_SKIP;
#endif
}
