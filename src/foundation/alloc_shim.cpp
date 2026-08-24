// alloc_shim.cpp - the global-allocator tripwire. Spec: docs/MEMORY.md section 8.1.
// HEADER-FIRST STUB (docs/ROADMAP.md section 0 rule 1): the counter reads 0 and install reports
// unsupported until the shim slice lands, so nothing links against a lie - guard_tick_end sees
// a zero delta, which is vacuous, not wrong; the real hook lands with the guard slice.
#include "foundation/alloc_shim.h"
#include "foundation/vmem_arena.h"

extern "C" ErrCode tl_alloc_shim_install(void) {
    return ERR_MEM_UNSUPPORTED;  // counting not yet available (stub)
}

extern "C" u64 tl_crt_alloc_count(void) {
    return 0;
}
