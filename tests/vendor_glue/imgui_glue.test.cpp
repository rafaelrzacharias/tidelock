// imgui_glue.h - proves ImGui's context alloc/free round-trips through pool_vendor.
// Spec: docs/MEMORY.md §8.6, docs/PLATFORM.md §9.5. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "vendor_glue/imgui_glue.h"
#include "pool_vendor_test_api.h"

#include <imgui.h>

TL_TEST(imgui_glue_create_destroy_context_through_pool_vendor, "vendor_glue,imgui,smoke") {
    vendor_glue_test_ensure_pool_vendor(t);
    vendor_glue_imgui_install();   // must run before CreateContext (docs/MEMORY.md §8.6)

    ImGuiContext* ctx = ImGui::CreateContext();
    TL_ASSERT_TRUE(ctx != nullptr);
    TL_EXPECT_TRUE(ImGui::GetCurrentContext() == ctx);
    ImGui::DestroyContext(ctx);
}
