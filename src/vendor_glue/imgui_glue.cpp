// imgui_glue.h - ImGui's two allocator hooks over pool_vendor. Spec: docs/MEMORY.md §8.6.
#include "vendor_glue/imgui_glue.h"
#include "vendor_glue/pool_vendor.h"
#include "foundation/tl_assert.h"

#include <imgui.h>

namespace {

void* tl_imgui_alloc(size_t size, void* /*user_data*/) {
    void* p = pool_alloc(pool_vendor(), (u64)size);
    // ImGui does not check IM_ALLOC's return before writing through it (unlike ENet's
    // ENetCallbacks, which take a dedicated no_memory hook) - a silent NULL here is a null-deref
    // deep inside ImGui's own code, not at this call site (docs/MEMORY.md §8.6).
    if (!p) TL_FATAL("imgui: pool_vendor budget exhausted");
    return p;
}

void tl_imgui_free(void* ptr, void* /*user_data*/) {
    pool_free(pool_vendor(), ptr);
}

}  // namespace

void vendor_glue_imgui_install(void) {
    ImGui::SetAllocatorFunctions(tl_imgui_alloc, tl_imgui_free);
}
