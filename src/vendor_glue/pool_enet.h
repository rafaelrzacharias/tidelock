#pragma once
// ---------------------------------------------------------------------------------------------
// pool_enet.h - the dedicated mem_pool for ENet (net buffers are sized by the protocol anyway).
//
// Spec: docs/MEMORY.md §1.5/§8.6 (the vendor-heap ruling); docs/PLATFORM.md §9.5 (16 MB, "owned
//   by net/" - the lifetime call is net/'s to make when it wires ENet in; the pool_* calls
//   themselves live here per docs/MEMORY.md §8.6's grep rule).
// Purpose: gives enet_glue's callbacks one MemPool to hook enet_initialize_with_callbacks to,
//   separate from pool_vendor (SDL3/SDL_ttf/ImGui/stb never touch network buffers).
// Invariants: pool_enet_init must run exactly once, before enet_initialize_with_callbacks; a
//   second call, or pool_enet() before the first, is a caller bug (TL_ASSERT).
// Determinism: none - this pool is never registered, hashed or snapshotted (docs/MEMORY.md §1.5).
// Threading: no locking of its own beyond mem_pool's; ENet's own single-threaded contract applies.
// Includes: foundation/mem_pool.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/mem_pool.h"

// Reserves and commits pool_enet's 16 MB arena (docs/PLATFORM.md §9.5). Call once, before
// enet_initialize_with_callbacks. A second call is a caller bug (TL_ASSERT).
ErrCode pool_enet_init(const VMemApi* os);

// The pool_enet instance. TL_ASSERT if pool_enet_init has not run yet.
MemPool* pool_enet(void);
