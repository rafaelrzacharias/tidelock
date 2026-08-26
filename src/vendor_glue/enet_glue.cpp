// enet_glue.h - ENet's three allocator hooks over pool_enet. Spec: docs/MEMORY.md §8.6.
#include "vendor_glue/enet_glue.h"
#include "vendor_glue/pool_enet.h"
#include "foundation/tl_assert.h"

#include <enet/enet.h>

namespace {

void* tl_enet_malloc(size_t size) {
    return pool_alloc(pool_enet(), (u64)size);
}

void tl_enet_free(void* memory) {
    pool_free(pool_enet(), memory);
}

void tl_enet_no_memory(void) {
    TL_FATAL("enet: pool_enet budget exhausted");
}

}  // namespace

bool vendor_glue_enet_install(void) {
    ENetCallbacks callbacks = {tl_enet_malloc, tl_enet_free, tl_enet_no_memory};
    return enet_initialize_with_callbacks(ENET_VERSION, &callbacks) == 0;
}
