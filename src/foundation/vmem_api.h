#pragma once
// ---------------------------------------------------------------------------------------------
// vmem_api.h - the virtual-memory fn-ptr seam, TRANSCRIBED from its owner, docs/PLATFORM.md.
//
// Spec: docs/PLATFORM.md §4 (the seam) and §9.2 (this struct, verbatim);
//   docs/ARCHITECTURE.md section 1 rule 1 (foundation is a leaf - vmem reaches it only through
//   fn-ptr tables injected at boot, never through a platform include).
// Purpose: the one table foundation's arenas call to reserve/commit/decommit/release address
//   space. The platform lane owns the semantics and the two OS implementations
//   (os_win_vmem.cpp / os_posix_vmem.cpp); this header exists because platform.h cannot be
//   included from foundation, exactly as foundation/atomic.h is owned by docs/JOBS.md. The
//   platform lane's platform.h must include this header, not redefine the struct (TODO.md,
//   W1 mem notes).
// Invariants: base/bytes given to commit/decommit are page multiples (the impl TL_CHECKs).
//   Zero-fill guarantee: a page first touched after commit - including a re-commit after
//   decommit - reads as zeros on every supported OS (docs/PLATFORM.md section 9.3 "vmem").
//   The arenas rely on that for padding determinism (docs/MEMORY.md section 1.1).
// Determinism: the table is fixed per build and resolved once at boot; foundation never names
//   an OS symbol, so the det libs stay audit-clean (docs/CPP-SUBSET.md section 4).
// Threading: reserve/commit/decommit/release are as thread-safe as the OS calls behind them;
//   arena code serialises its own use (one arena is single-writer, docs/MEMORY.md section 1.3).
// Includes: foundation/tl_types.h only.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

// docs/PLATFORM.md section 9.2, verbatim. reserve returns null on address-space exhaustion;
// commit returns ERR_PLATFORM_VMEM on failure (Windows commit charge; Linux overcommit cannot
// fail for RAM); decommit returns the pages, release unmaps the whole reservation.
struct VMemApi  { void* ctx; void* (*reserve)(void* ctx, u64 bytes); ErrCode (*commit)(void* ctx, void* base, u64 bytes);
                  ErrCode (*decommit)(void* ctx, void* base, u64 bytes); void (*release)(void* ctx, void* base, u64 bytes); u32 page_size; u32 _pad0; };

static_assert(sizeof(VMemApi) == 48, "5 pointers + 2 u32 on a 64-bit target (docs/PLATFORM.md section 9.2)");
static_assert(__is_trivially_copyable(VMemApi), "fn-ptr table is POD");
