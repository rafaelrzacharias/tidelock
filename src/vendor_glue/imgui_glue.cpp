// imgui_glue.h - ImGui's two allocator hooks over pool_vendor. Spec: docs/MEMORY.md §8.6.
#include "vendor_glue/imgui_glue.h"
#include "vendor_glue/pool_vendor.h"

#include <imgui.h>

namespace {

void* tl_imgui_alloc(size_t size, void* /*user_data*/) {
    return pool_alloc(pool_vendor(), (u64)size);
}

void tl_imgui_free(void* ptr, void* /*user_data*/) {
    pool_free(pool_vendor(), ptr);
}

}  // namespace

void vendor_glue_imgui_install(void) {
    ImGui::SetAllocatorFunctions(tl_imgui_alloc, tl_imgui_free);
}
