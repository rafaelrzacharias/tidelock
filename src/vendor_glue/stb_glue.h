#pragma once
// ---------------------------------------------------------------------------------------------
// stb_glue.h - the stb_image/stb_sprintf implementation TU's minimal proof-of-link surface.
//
// Spec: docs/MEMORY.md §8.6 (STBI_MALLOC/REALLOC/FREE -> pool_vendor); docs/BUILD.md §10.1
//   ("stb (one TU)"); docs/PLATFORM.md §9.5 (stb_image -> pool_vendor; stb_sprintf allocates
//   nothing).
// Purpose: stb_glue.cpp is the ONE translation unit in the tree that defines
//   STB_IMAGE_IMPLEMENTATION/STB_SPRINTF_IMPLEMENTATION - every other TU must reach stb_image.h/
//   stb_sprintf.h in declaration-only mode or risk a second *_IMPLEMENTATION definition. This
//   header is that TU's test-facing surface, proving both libs link and allocate correctly; it
//   is NOT the eventual consumer seam. Neither src/foundation (a DAG leaf, docs/ARCHITECTURE.md
//   §1 rule 1) nor any module above it may include "vendor_glue/..." at all - the future
//   fmt_buf/stb_sprintf hookup (docs/CONTAINERS.md §8.6b's TL_FATAL stub) needs a fn-ptr seam
//   TRANSCRIBED into foundation the way foundation/vmem_api.h transcribes docs/PLATFORM.md,
//   not a direct include of this header. That seam is that lane's design call, not vendored here.
// Invariants: stb_image's allocator macros are wired to pool_vendor unconditionally (no install
//   step - unlike SDL3/ImGui, stb_image has no runtime allocator-registration API, only
//   compile-time macros), so pool_vendor_init must have already run before the first call here.
// Determinism: none - both libs' outputs are non-authoritative asset/format tooling
//   (docs/MEMORY.md §1.5).
// Threading: as thread-safe as pool_vendor (docs/PLATFORM.md §9.5); stb_image/stb_sprintf carry
//   no internal state of their own beyond the caller-provided buffers.
// Includes: foundation/tl_types.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

// stbi_load_from_memory, re-exported: decodes an in-memory image to RGBA8, width*height*4 bytes,
// allocated through pool_vendor. Returns null on a malformed buffer; the caller frees the
// returned buffer with vendor_glue_stbi_free.
u8* vendor_glue_stbi_load_from_memory(const u8* buffer, i32 len, i32* out_w, i32* out_h);

// Frees a buffer returned by vendor_glue_stbi_load_from_memory.
void vendor_glue_stbi_free(void* image_data);

// stbsp_snprintf, re-exported: stb_sprintf's allocation-free formatter (docs/PLATFORM.md §9.5 -
// "stb_sprintf allocates nothing"). Same contract as snprintf: writes at most buf_size-1 bytes
// plus a NUL, returns the length the FULL formatted string would have needed.
i32 vendor_glue_stbsp_snprintf(char* buf, i32 buf_size, const char* fmt, ...);
