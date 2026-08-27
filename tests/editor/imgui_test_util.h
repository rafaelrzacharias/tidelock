#pragma once
// imgui_test_util.h - the shared headless ImGui context for tests/editor: one process-wide
// ImGuiContext driven by the vendored null backend (vendor/imgui/backends/imgui_impl_null.cpp),
// no window, no real output. Spec: docs/TOOLING.md §9.5 "panels run ImGui headless via a null
// backend".
#include "runner/tl_test.h"

#include "imgui.h"
#include "imgui_impl_null.h"

// Creates the one process-wide ImGuiContext and inits the null backend, once. Every later call
// is a no-op (matching world_test_util.h's "statics, re-init is idempotent" shape) - ImGui itself
// has no supported "destroy and recreate mid-run" path that tests need here.
inline void imgui_test_ensure_context(void) {
    static u8 done = 0;
    if (done) { return; }
    ImGui::CreateContext();
    TL_CHECK(ImGui_ImplNull_Init());
    done = 1;
}

// Begins one ImGui frame against the null backend (io.DisplaySize/DeltaTime set by
// ImGui_ImplNullPlatform_NewFrame) - callers do their widget calls (a panel's draw_fn, typically)
// between this and imgui_test_end_frame.
inline void imgui_test_begin_frame(void) {
    imgui_test_ensure_context();
    ImGui_ImplNull_NewFrame();
    ImGui::NewFrame();
}

// Ends the frame (Render() closes it out and builds draw data the null renderer never consumes -
// nothing here reads ImDrawData, only ImGui's own internal per-widget/window state, which a test
// reaches through ImGui::FindWindowByName et al.).
inline void imgui_test_end_frame(void) { ImGui::Render(); }
