#pragma once
// ---------------------------------------------------------------------------------------------
// pool_vendor.h - the ONE mem_pool shared by SDL3, SDL_ttf, Dear ImGui and stb.
//
// Spec: docs/MEMORY.md §1.5 (the vendor-heap ruling) and §8.6 (mem_pool itself);
//   docs/PLATFORM.md §9.5 (the wiring table: pool_vendor is 64 MB, shared by those four libs -
//   ENet gets its own pool_enet, Luau one pool per VM).
// Purpose: gives every SDL3/SDL_ttf/ImGui/stb glue adaptor in this directory one MemPool to hook
//   its SetMemoryFunctions/SetAllocatorFunctions/STBI_MALLOC calls to, instead of each reserving
//   its own arena.
// Invariants: pool_vendor_init must run exactly once, before any hooked lib allocates (i.e.
//   before SDL_Init / ImGui::CreateContext / the first stb call); pool_vendor() before that is a
//   caller bug (TL_ASSERT).
// Determinism: none - this pool is never registered, hashed or snapshotted (docs/MEMORY.md §1.5).
// Threading: no locking of its own beyond mem_pool's; the hooked libs call from their own
//   documented threads only (docs/PLATFORM.md §9.5).
// Includes: foundation/mem_pool.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/mem_pool.h"

// Reserves and commits pool_vendor's 64 MB arena (docs/PLATFORM.md §9.5). Call once, before
// installing any of this pool's hooked libs. A second call is a caller bug (TL_ASSERT).
ErrCode pool_vendor_init(const VMemApi* os);

// The shared pool_vendor instance. TL_ASSERT if pool_vendor_init has not run yet.
MemPool* pool_vendor(void);
