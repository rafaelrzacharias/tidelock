// mem_pool.cpp - the vendor-heap pool. Spec: docs/MEMORY.md §8.6.
// Layout invariants this file owns (the header recovery contract):
//   - every carve (class page or large block) starts at a 64 KB-ALIGNED ADDRESS, so
//     `p & ~0xFFFF` recovers the carve header from any block pointer;
//   - a carve's first 64 bytes are the header {u16 klass; u16 large; u32 live; u64 size};
//     blocks start at offset 64 and are therefore 16-byte aligned for every class;
//   - the 64 KB class's pages are TWO granules (128 KB, one block at +64): a 64 KB block
//     cannot share a 64 KB page with its header - MEMORY.md §8.6 notes the exception;
//   - large carves round to 64 KB multiples so the arena stays permanently 64 KB-aligned;
//     freeing one decommits exactly its range via the injected VMemApi (the bump pointer is
//     not moved - address space is never reused, pages are returned).
#include "foundation/mem_pool.h"

#include <string.h>  // memcpy/memset (docs/CPP-SUBSET.md §1 allowlist)

namespace mem {

enum : u64 { GRANULE = 64u * 1024u, GRANULE_MASK = GRANULE - 1u };
enum : u32 { HEADER_BYTES = 64u };

struct CarveHeader {
    u16 klass;    // class index for a page; unused for large
    u16 large;    // 1 = large carve
    u32 live;     // live blocks in this page (pages only)
    u64 size;     // large: the user-requested size; pages: the carve size
};
static_assert(sizeof(CarveHeader) == 16, "first 16 bytes per docs/MEMORY.md section 8.6");

static inline u64 class_size(u32 c) { return (u64)16u << c; }

static u32 class_for(u64 size) {
    u32 c = 0;
    while (class_size(c) < size) { c += 1u; }
    return c;   // caller guarantees size <= POOL_MAX_SMALL, so c < POOL_CLASS_COUNT
}

// Carves `bytes` (a GRANULE multiple) at a 64 KB-aligned ADDRESS, wasting the gap the arena's
// own bump position imposes. Returns null (never fatal) when the pool budget refuses.
static u8* carve_aligned(MemPool* p, u64 bytes) {
    const u64 addr = (u64)(p->arena.base) + p->arena.used;
    const u64 gap = ((addr + GRANULE_MASK) & ~GRANULE_MASK) - addr;
    if (p->carved_bytes + bytes > p->budget_bytes) {
        return nullptr;   // Luau raises its own memory error; ImGui/SDL assert (section 1.5)
    }
    if (p->arena.used + gap + bytes > p->arena.reserved) {
        return nullptr;   // reserve exhausted behaves as budget exhausted for a vendor heap
    }
    u8* q = (u8*)arena_push(&p->arena, gap + bytes, 1u) + gap;
    p->carved_bytes += bytes;
    return q;
}

static void stats_on_alloc(MemPool* p, u64 payload) {
    p->stats.live_bytes += payload;
    if (p->stats.live_bytes > p->stats.peak_bytes) { p->stats.peak_bytes = p->stats.live_bytes; }
}

}  // namespace mem

ErrCode pool_init(MemPool* p, NameHash id, u64 reserve_bytes, u64 budget_bytes, const VMemApi* os) {
    if (p == nullptr) { return ERR_MEM_BAD_ARG; }
    TL_ASSERT(budget_bytes != 0u);   // an uncapped vendor heap is the bug the pool exists to prevent
    p->budget_bytes = budget_bytes;
    p->carved_bytes = 0u;
    for (u32 i = 0; i < POOL_CLASS_COUNT; ++i) { p->free_head[i] = nullptr; }
    p->stats = MemPoolStats{};
    return vmem_arena_init(&p->arena, id, reserve_bytes, 0u, os);
}

void* pool_alloc(MemPool* p, u64 size) {
    TL_ASSERT(p != nullptr);
    if (size == 0u) { return nullptr; }

    if (size > p->budget_bytes) { return nullptr; }   // also forecloses u64 wrap in the rounding below

    if (size > (u64)POOL_MAX_SMALL) {
        // Large path: a dedicated 64 KB-granular carve, header + block at offset 64.
        const u64 carve = (size + (u64)mem::HEADER_BYTES + mem::GRANULE_MASK) & ~mem::GRANULE_MASK;
        u8* page = mem::carve_aligned(p, carve);
        if (page == nullptr) { return nullptr; }
        mem::CarveHeader* h = (mem::CarveHeader*)page;
        h->klass = 0u; h->large = 1u; h->live = 1u; h->size = size;
        p->stats.large_count += 1u;
        mem::stats_on_alloc(p, size);
        return page + mem::HEADER_BYTES;
    }

    const u32 c = mem::class_for(size);
    if (p->free_head[c] == nullptr) {
        // Carve a class page and thread every block onto the freelist (LIFO, address order).
        // The top class needs two granules: one 64 KB block cannot share a page with its header.
        const u64 csize = mem::class_size(c);
        const u64 page_bytes = (csize == (u64)POOL_MAX_SMALL) ? mem::GRANULE * 2u : mem::GRANULE;
        u8* page = mem::carve_aligned(p, page_bytes);
        if (page == nullptr) { return nullptr; }
        mem::CarveHeader* h = (mem::CarveHeader*)page;
        h->klass = (u16)c; h->large = 0u; h->live = 0u; h->size = page_bytes;
        const u64 n = (page_bytes - (u64)mem::HEADER_BYTES) / csize;
        for (u64 i = n; i > 0u; --i) {   // reverse, so the freelist pops in address order
            u8* block = page + (u64)mem::HEADER_BYTES + (i - 1u) * csize;
            *(void**)block = p->free_head[c];
            p->free_head[c] = block;
        }
    }

    u8* block = (u8*)p->free_head[c];
    p->free_head[c] = *(void**)block;
    mem::CarveHeader* h = (mem::CarveHeader*)(((u64)block) & ~mem::GRANULE_MASK);
    h->live += 1u;
    p->stats.live_count[c] += 1u;
    mem::stats_on_alloc(p, mem::class_size(c));
    return block;
}

void pool_free(MemPool* p, void* q) {
    TL_ASSERT(p != nullptr);
    if (q == nullptr) { return; }
    u8* page = (u8*)(((u64)q) & ~mem::GRANULE_MASK);
    mem::CarveHeader* h = (mem::CarveHeader*)page;

    if (h->large != 0u) {
        TL_ASSERT(h->live == 1u && (u8*)q == page + mem::HEADER_BYTES);
        const u64 size = h->size;
        const u64 carve = (size + (u64)mem::HEADER_BYTES + mem::GRANULE_MASK) & ~mem::GRANULE_MASK;
        // Return the pages; the bump pointer is not moved - address space is never reused.
        const ErrCode e = p->arena.os->decommit(p->arena.os->ctx, page, carve);
        TL_CHECK(e == ERR_OK);
        p->carved_bytes -= carve;
        p->stats.large_count -= 1u;
        p->stats.live_bytes -= size;
        return;
    }

    const u32 c = h->klass;
    TL_ASSERT(c < POOL_CLASS_COUNT);
    TL_ASSERT(h->live > 0u);
    h->live -= 1u;
    *(void**)q = p->free_head[c];
    p->free_head[c] = q;
    p->stats.live_count[c] -= 1u;
    p->stats.live_bytes -= mem::class_size(c);
}

void* pool_realloc(MemPool* p, void* q, u64 new_size) {
    TL_ASSERT(p != nullptr);
    if (q == nullptr) { return pool_alloc(p, new_size); }
    if (new_size == 0u) { pool_free(p, q); return nullptr; }

    u8* page = (u8*)(((u64)q) & ~mem::GRANULE_MASK);
    const mem::CarveHeader* h = (const mem::CarveHeader*)page;
    u64 old_payload;
    if (h->large != 0u) {
        const u64 old_carve = (h->size + (u64)mem::HEADER_BYTES + mem::GRANULE_MASK) & ~mem::GRANULE_MASK;
        const u64 new_carve = (new_size + (u64)mem::HEADER_BYTES + mem::GRANULE_MASK) & ~mem::GRANULE_MASK;
        if (new_size > (u64)POOL_MAX_SMALL && new_carve == old_carve) {
            // Same pages: keep the pointer, retitle the block.
            mem::CarveHeader* hw = (mem::CarveHeader*)page;
            p->stats.live_bytes = p->stats.live_bytes - hw->size + new_size;
            if (p->stats.live_bytes > p->stats.peak_bytes) { p->stats.peak_bytes = p->stats.live_bytes; }
            hw->size = new_size;
            return q;
        }
        old_payload = h->size;
    } else {
        if (new_size <= (u64)POOL_MAX_SMALL && mem::class_for(new_size) == (u32)h->klass) {
            return q;   // same class: same pointer (docs/MEMORY.md section 8.6)
        }
        old_payload = mem::class_size(h->klass);
    }

    void* r = pool_alloc(p, new_size);
    if (r == nullptr) { return nullptr; }   // q untouched - the caller still owns it
    const u64 n = (old_payload < new_size) ? old_payload : new_size;
    memcpy(r, q, n);
    pool_free(p, q);
    return r;
}

const MemPoolStats* pool_stats(const MemPool* p) {
    TL_ASSERT(p != nullptr);
    return &p->stats;
}
