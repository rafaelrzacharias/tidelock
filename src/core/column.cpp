// column.cpp - ComponentTable implementation. Spec: docs/ECS.md §10.3; contract in column.h.
#include "core/column.h"
#include <string.h>

namespace {

// The Entity domain's slot capacity and its fixed page-slot count (1024 pages of 4096).
constexpr u64 ECS_DOMAIN_SLOTS = (u64)Entity::IDX_MASK + 1u;
constexpr u32 ECS_DOMAIN_PAGES = (u32)(ECS_DOMAIN_SLOTS / ECS_PAGE_SIZE);
static_assert(ECS_DOMAIN_SLOTS % ECS_PAGE_SIZE == 0, "pages tile the domain exactly");

// The sparse slot for entity index eidx, committing (and NONE-filling) its page on first touch.
// Commit happens only on the add path, which is barrier-window-bound by contract.
u32* sparse_slot(ComponentTable* t, u32 eidx) {
    u32 p = eidx >> ECS_PAGE_SHIFT;
    TL_CHECK(p < t->page_count);
    if (t->pages[p] == nullptr) {
        u32* page = (u32*)arena_push(&t->page_arena, (u64)ECS_PAGE_SIZE * sizeof(u32), alignof(u32));
        memset(page, 0xFF, (usize)ECS_PAGE_SIZE * sizeof(u32));   // every slot ECS_SPARSE_NONE
        t->pages[p] = page;
    }
    return &t->pages[p][eidx & (ECS_PAGE_SIZE - 1u)];
}

}  // namespace

ErrCode column_init(ComponentTable* t, const ComponentInfo* info,
                    NameHash id_dense, NameHash id_entity, NameHash id_pages, const VMemApi* os) {
    TL_CHECK(info != nullptr && info->size != 0 && info->align != 0);
    TL_CHECK(info->size % info->align == 0);   // stride == size needs sizeof to be an align multiple
    t->info = info;
    t->stride = info->size;
    t->count = 0;
    // Reserves are the domain's worst case - address space only, committed on demand
    // (docs/MEMORY.md §1.1; the §6 budget note replaces these with measured numbers at T-A-03).
    ErrCode e = vmem_arena_init(&t->dense_arena, id_dense, ECS_DOMAIN_SLOTS * t->stride,
                                ARENA_ZERO_ON_PUSH, os);
    if (e != ERR_OK) { return e; }
    e = vmem_arena_init(&t->entity_arena, id_entity, ECS_DOMAIN_SLOTS * sizeof(Entity),
                        ARENA_ZERO_ON_PUSH, os);
    if (e != ERR_OK) { return e; }
    e = vmem_arena_init(&t->page_arena, id_pages,
                        (u64)ECS_DOMAIN_PAGES * sizeof(u32*) + ECS_DOMAIN_SLOTS * sizeof(u32),
                        ARENA_ZERO_ON_PUSH, os);
    if (e != ERR_OK) { return e; }
    t->dense = t->dense_arena.base;
    t->entities = (Entity*)t->entity_arena.base;
    // The page-pointer array is fixed at the whole domain up front (8 KB); fresh pages are
    // OS-zero, so every slot starts null = uncommitted.
    t->pages = (u32**)arena_push(&t->page_arena, (u64)ECS_DOMAIN_PAGES * sizeof(u32*), alignof(u32*));
    t->page_count = ECS_DOMAIN_PAGES;
    return ERR_OK;
}

void* column_get(ComponentTable* t, Entity e) {
    u32 eidx = handle_index(e);
    u32 p = eidx >> ECS_PAGE_SHIFT;
    if (p >= t->page_count || t->pages[p] == nullptr) { return nullptr; }
    u32 d = t->pages[p][eidx & (ECS_PAGE_SIZE - 1u)];
    if (d == ECS_SPARSE_NONE || t->entities[d].bits != e.bits) { return nullptr; }
    return t->dense + (u64)d * t->stride;
}

void column_add(ComponentTable* t, Entity e, const void* value) {
    TL_CHECK(!handle_is_null(e) && value != nullptr);
    u32* s = sparse_slot(t, handle_index(e));
    TL_CHECK(*s == ECS_SPARSE_NONE);   // one row per (component, entity)
    // `used` never shrinks on remove (the hashing ruling in column.h), so a re-add below the
    // high-water writes the zeroed row in place; only a row past `used` grows the arena.
    if (((u64)t->count + 1u) * t->stride > t->dense_arena.used) {
        void* grown = arena_push(&t->dense_arena, t->stride, t->info->align);
        TL_ASSERT(grown == t->dense + (u64)t->count * t->stride);   // the column owns its whole range
        (void)grown;
    }
    if (((u64)t->count + 1u) * sizeof(Entity) > t->entity_arena.used) {
        void* grown = arena_push(&t->entity_arena, sizeof(Entity), alignof(Entity));
        TL_ASSERT(grown == (void*)(t->entities + t->count));
        (void)grown;
    }
    u8* row = t->dense + (u64)t->count * t->stride;
    memcpy(row, value, t->info->size);
    t->entities[t->count] = e;
    *s = t->count;
    t->count += 1u;
}

void column_remove(ComponentTable* t, Entity e) {
    u32* s = sparse_slot(t, handle_index(e));
    u32 d = *s;
    TL_CHECK(d != ECS_SPARSE_NONE);
    TL_ASSERT(t->entities[d].bits == e.bits);   // a stale remove reaching a reused slot is a bug
    u32 last = t->count - 1u;
    if (d != last) {
        memcpy(t->dense + (u64)d * t->stride, t->dense + (u64)last * t->stride, t->stride);
        t->entities[d] = t->entities[last];
        *sparse_slot(t, handle_index(t->entities[d])) = d;
    }
    // The vacated tail row and entity slot stay inside the hashed extent - zero them (the
    // Array<T> ruling; leaving bytes would make the hash a function of removal history).
    memset(t->dense + (u64)last * t->stride, 0, t->stride);
    t->entities[last] = Entity{ 0 };
    *s = ECS_SPARSE_NONE;
    t->count = last;
}
