// mem_pool.cpp - the vendor-heap pool. Spec: docs/MEMORY.md section 8.6.
// HEADER-FIRST STUB (docs/ROADMAP.md section 0 rule 1).
#include "foundation/mem_pool.h"

ErrCode pool_init(MemPool*, NameHash, u64, u64, const VMemApi*) {
    TL_FATAL("unimplemented: pool_init (w1-mem, docs/MEMORY.md section 8.6)");
}

void* pool_alloc(MemPool*, u64) {
    TL_FATAL("unimplemented: pool_alloc (w1-mem, docs/MEMORY.md section 8.6)");
}

void pool_free(MemPool*, void*) {
    TL_FATAL("unimplemented: pool_free (w1-mem, docs/MEMORY.md section 8.6)");
}

void* pool_realloc(MemPool*, void*, u64) {
    TL_FATAL("unimplemented: pool_realloc (w1-mem, docs/MEMORY.md section 8.6)");
}

const MemPoolStats* pool_stats(const MemPool*) {
    TL_FATAL("unimplemented: pool_stats (w1-mem, docs/MEMORY.md section 8.6)");
}
