#pragma once
// ---------------------------------------------------------------------------------------------
// sdl3_glue.h - hooks SDL3's allocator to pool_vendor.
//
// Spec: docs/MEMORY.md §8.6 (`tl_sdl_malloc/calloc/realloc/free`, `SDL_SetMemoryFunctions` before
//   `SDL_Init`); docs/PLATFORM.md §9.5 (SDL3 -> pool_vendor).
// Purpose: the ONE call site of SDL_SetMemoryFunctions in the tree, so the impl_sdl3 platform
//   lane installs SDL3's allocator hookup by calling this instead of reaching into pool_vendor.h
//   itself.
// Invariants: must run before the first SDL allocation - in practice before SDL_Init - and after
//   pool_vendor_init (TL_ASSERT: pool_vendor() already enforces this).
// Determinism: none - SDL3's heap is never authoritative (docs/MEMORY.md §1.5).
// Threading: call once from the thread that will call SDL_Init (docs/PLATFORM.md §9.3 "window").
// Includes: foundation/tl_types.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

// Installs tl_sdl_malloc/calloc/realloc/free as SDL3's allocator via SDL_SetMemoryFunctions.
// Call once, before SDL_Init and after pool_vendor_init.
void vendor_glue_sdl3_install(void);
