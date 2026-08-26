#pragma once
// ---------------------------------------------------------------------------------------------
// vendor_new.h - a pool-backed global operator new/delete for vendored C++ libraries that expose
//   no allocator hook.
//
// Spec: docs/MEMORY.md §1.5 and §2 (both amended by RR-18, ruled 2026-08-26), §8.6 (the pool);
//   docs/PLATFORM.md §9.5 ("src/vendor_glue/ - the one folder allowed a static pool pointer").
// Purpose: docs/MEMORY.md §2's model is that every vendored library routes through mem_pool via
//   its hook API. Luau's VM does exactly that; Luau's COMPILER has no hook API at all and
//   allocates with global `operator new` - measured 32 calls per luau_compile against ZERO from
//   the VM. `operator new` is replaceable per-PROGRAM and nowhere narrower, so the only way to
//   budget that heap is to replace it program-wide, which is what this TU does. A program that
//   does NOT install it links foundation's TL_FATAL tripwires instead (alloc_shim_ops.cpp, its
//   own archive member precisely so the two never collide).
// Invariants: install before the first allocation the vendored library makes and uninstall after
//   its last; between those points EVERY global new in the process comes from `pool`, including
//   any accidental one from src/ - which is why the symbol audit's ban on new/delete in src/ libs
//   is the check that still matters, not this. Not reentrant; not thread-safe (the pool is not).
// Determinism: never authoritative, never registered, never hashed (docs/MEMORY.md §5). Bytes
//   allocated here belong to a compile, not to a tick.
// Threading: one owner. The Luau compiler runs on the thread that asked for the compile.
// Includes: foundation/mem_pool.h, foundation/tl_types.h.
//
// The ONE pointer this needs is namespace-scope mutable state, which docs/CPP-SUBSET.md §1 bans
// everywhere else. It is exempted by name in tools/audit/static_allow.txt (lib + directory +
// stem, the same shape as RR-7's tooling-plane exemption), and there is no other way: a
// replaceable `operator new` takes no context parameter and never will.
// ---------------------------------------------------------------------------------------------
#include "foundation/mem_pool.h"
#include "foundation/tl_types.h"

// Routes every subsequent global operator new/delete in this program to `pool`. Passing null
// uninstalls, after which an allocation TL_FATALs exactly as the foundation tripwire would.
// Installing over an existing pool is a TL_FATAL: two owners of one program-wide hook is a bug,
// and the second installer would silently orphan the first's live blocks.
void vendor_heap_install(MemPool* pool);

// The pool currently serving global operator new, or null when none is installed. Read by the
// tests and by the profiler; never by an allocation path.
MemPool* vendor_heap_current(void);
