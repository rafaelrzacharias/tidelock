#pragma once
// ---------------------------------------------------------------------------------------------
// os_vmem.h - internal seam between the shared os_*_vmem.cpp TU and each impl's vmem.cpp.
//
// Spec: docs/PLATFORM.md §4, §9.1 ("os_win_vmem.cpp / os_posix_vmem.cpp ... one compiled per OS").
// Purpose: one OS-backed VMemApi table, built once, pointed at by both impl_sdl3/vmem.cpp and
//   impl_headless/vmem.cpp ("thin: point the table at the shared os_* functions", PLATFORM.md §7).
// Invariants: exactly one of os_win_vmem.cpp / os_posix_vmem.cpp is compiled per target
//   (src/platform/CMakeLists.txt selects by WIN32); both define this same symbol.
// Determinism: not part of the public contract - platform.h never includes this. No state: the
//   four calls are stateless OS wrappers, `ctx` is unused.
// Threading: as thread-safe as the OS calls behind it (reserve/commit/decommit/release).
// Includes: foundation/vmem_api.h only.
// ---------------------------------------------------------------------------------------------
#include "foundation/vmem_api.h"

// Fills `*out` with the real OS-backed VMemApi (VirtualAlloc/mmap family, docs/PLATFORM.md §9.3
// "vmem"). `out->ctx` is left null - the four calls carry no state of their own.
void os_vmem_fill_table(VMemApi* out);
