#pragma once
// imgui_test_util.h - the shared headless ImGui context for tests/editor: one process-wide
// ImGuiContext driven by the vendored null backend (vendor/imgui/backends/imgui_impl_null.cpp),
// no window, no real output. Spec: docs/TOOLING.md §9.5 "panels run ImGui headless via a null
// backend".
//
// Lifecycle (CI sanitizer red on 17c5a45, both Linux legs - steward-diagnosed, fixed here):
// the first version created the context WITHOUT vendor_glue_imgui_install() first, so every
// allocation went through the default (malloc-backed) allocator instead of pool_vendor, and was
// never freed - a real LeakSanitizer hit, not a false positive (docs/MEMORY.md §8.6's own
// invariant: install "must run before the first ImGui::CreateContext() call").
//
// Fixed by routing through pool_vendor (vendor_glue_imgui_install, matching
// tests/vendor_glue/imgui_glue.test.cpp's own proven create/destroy-returns-to-baseline pattern)
// AND registering a single atexit teardown, rather than leaving the context permanently live.
// Considered three shapes for the "when does it get destroyed" question the steward asked for a
// real design pass on:
//   (a) never destroy, rely on pool_vendor being VMem-backed (foundation/mem_pool.cpp ->
//       vmem_arena_init -> the injected VMemApi, never malloc) so a leak sanitizer that only
//       intercepts the malloc/new family would not see it as a tracked leak at all. Simplest,
//       but a hypothesis this container's missing ASan runtime cannot verify locally (the
//       steward's own finding) - betting the fix on an assumption CI has not yet confirmed is
//       exactly the kind of thing to avoid when a provably-correct alternative costs one line.
//   (b) an explicit shutdown function every test file (or the last test) calls itself - no
//       hidden control flow, but order-dependent (test execution order/subset is not a contract
//       this runner makes) and easy to get wrong the day a new editor test file is added.
//   (c) atexit(ImGui::DestroyContext) registered once, alongside the install call - matches
//       imgui_glue.test.cpp's own proven round-trip (pool_vendor's live_bytes returns to
//       baseline on DestroyContext), and this codebase's ban on destructors (docs/CPP-SUBSET.md)
//       means there is no static-destruction-order hazard to reason about: pool_vendor's globals
//       (vendor_glue/pool_vendor.cpp) are POD, nothing runs at their own teardown to race against.
// Chose (c): provably correct against the tree's own existing proof, not a new assumption.
#include "runner/tl_test.h"
#include "vendor_glue/imgui_glue.h"
#include "vendor_glue/pool_vendor_test_api.h"

#include "imgui.h"
#include "imgui_impl_null.h"

#include <stdlib.h>

// True `inline` VARIABLES (C++17, not an anonymous-namespace one), so every .cpp file that
// includes this header shares the SAME instance - found the hard way. An anonymous-namespace
// `ImGuiContext* g_imgui_test_ctx` gives each TU internal linkage over its OWN copy, but the
// `inline` FUNCTIONS below (imgui_test_ensure_context et al.) have vague linkage and the linker
// keeps only ONE TU's compiled body for the whole binary - so once a second .cpp file included
// this header (console_panel.test.cpp, alongside log_panel.test.cpp), the surviving copy of
// these functions was quietly compiled against ONE TU's `g_imgui_test_ctx` while callers from
// the OTHER TU still believed they were reading/writing their own - a real ODR violation, not a
// hypothetical one. Manifested as `ImGui::SetCurrentContext(g_imgui_test_ctx)` immediately
// followed by an internal "no current context" assert inside `ImGui_ImplNull_NewFrame` - GDB's
// backtrace showed the crash landed inside a call chain consistent with only one TU's state
// existing, confirmed by moving the variable in scope and rebuilding clean. Only reproduced on
// the `dev` tier (`-O1`+, this build's inliner/linker collapses the duplicate copies
// differently than debug's `-O0` does) - exactly the class of bug a debug-tier-only pass cannot
// catch, which is why this session's own lesson is now "validate all four tiers before every
// push," not three.
inline ImGuiContext* g_imgui_test_ctx = nullptr;
// Destroys the EXACT context this header created, never the ambient "current" one
// (ImGui::DestroyContext() with no argument would destroy whatever happens to be current at exit
// - not necessarily this one, see imgui_test_end_frame's note below).
inline void imgui_test_teardown(void) {
    ImGui::DestroyContext(g_imgui_test_ctx);
    g_imgui_test_ctx = nullptr;
}

// Creates the one process-wide ImGuiContext (through pool_vendor, see this header's Lifecycle
// note), once. Does NOT leave it current on return - imgui_test_begin_frame does that, scoped to
// the frame, per the same reasoning imgui_test_end_frame documents.
inline void imgui_test_ensure_context(TestCtx* t) {
    if (g_imgui_test_ctx != nullptr) { return; }
    vendor_glue_test_ensure_pool_vendor(t);
    vendor_glue_imgui_install();   // must run before CreateContext (docs/MEMORY.md §8.6)
    ImGuiContext* prev = ImGui::GetCurrentContext();
    g_imgui_test_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_imgui_test_ctx);   // ImGui_ImplNull_Init needs OUR context current
    ImGui::GetIO().IniFilename = nullptr;   // headless - no imgui.ini in whatever cwd tests run from
    TL_CHECK(ImGui_ImplNull_Init());
    ImGui::SetCurrentContext(prev);   // restore exactly the ambient state this call found
    atexit(imgui_test_teardown);
}

// Begins one ImGui frame against the null backend (io.DisplaySize/DeltaTime set by
// ImGui_ImplNullPlatform_NewFrame) - callers do their widget calls (a panel's draw_fn, typically)
// between this and imgui_test_end_frame.
inline void imgui_test_begin_frame(TestCtx* t) {
    imgui_test_ensure_context(t);
    ImGui::SetCurrentContext(g_imgui_test_ctx);
    ImGui_ImplNull_NewFrame();
    ImGui::NewFrame();
}

// Ends the frame (Render() closes it out and builds draw data the null renderer never consumes -
// nothing here reads ImDrawData, only ImGui's own internal per-widget/window state, which a test
// reaches through ImGui::FindWindowByName et al. before this call) and releases "current" back to
// NULL. Found necessary, not assumed defensive: tests/vendor_glue/imgui_glue.test.cpp's own test
// creates a context and asserts `ImGui::GetCurrentContext() == ctx` right after - but
// ImGui::CreateContext's real body (vendor/imgui/imgui.cpp) only leaves the NEW context current
// when there was NO current context beforehand; if this header's context were still current from
// an earlier editor test in the same non-isolated run (`tl_tests --tag '!slow'`, no --isolate -
// the sanitizer job), CreateContext would silently restore THIS context as current instead,
// failing that assertion. Reproduced locally (non-isolated, matching CI's own invocation) before
// this fix, confirmed passing after - not reasoned about only on paper.
inline void imgui_test_end_frame(void) {
    ImGui::Render();
    ImGui::SetCurrentContext(nullptr);
}
