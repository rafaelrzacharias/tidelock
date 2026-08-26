#pragma once
// ---------------------------------------------------------------------------------------------
// imgui_glue.h - hooks Dear ImGui's allocator to pool_vendor.
//
// Spec: docs/MEMORY.md §8.6 (`tl_imgui_alloc/free`, `ImGui::SetAllocatorFunctions` before
//   `CreateContext`); docs/PLATFORM.md §9.5 (ImGui -> pool_vendor).
// Purpose: the ONE call site of ImGui::SetAllocatorFunctions in the tree, so src/editor installs
//   ImGui's allocator hookup by calling this instead of reaching into pool_vendor.h itself.
// Invariants: must run before the first ImGui::CreateContext() call and after pool_vendor_init
//   (TL_ASSERT: pool_vendor() already enforces this).
// Determinism: none - ImGui's heap is never authoritative (docs/MEMORY.md §1.5).
// Threading: call once from the thread that owns the ImGui context (dev/editor only).
// Includes: foundation/tl_types.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

// Installs the pool_vendor-backed alloc/free pair as ImGui's allocator via
// ImGui::SetAllocatorFunctions. Call once, before the first ImGui::CreateContext() and after
// pool_vendor_init.
void vendor_glue_imgui_install(void);
