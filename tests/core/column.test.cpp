// column.h - the paged sparse-set column against a naive model: 100k random ops, stale-handle
// absence, page commit boundaries, and the vacated-row zeroing ruling.
// Spec: docs/ECS.md §10.3, §10.8 (column.test.cpp line). Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "core/column.h"
#include "foundation/rng.h"
#include "foundation/vmem_test_api.h"

#define TL_FIELDS_ColPoint(X, XA, XH) \
    X(i32, x) X(i32, y) X(u32, tag)
TL_COMPONENT(ColPoint)

namespace {

// The naive reference: one slot per entity index (the sparse set's own keying), value + handle.
struct ModelSlot { Entity e; ColPoint v; u8 present; u8 _pad[3]; };

// Shared fixture: a fresh column over the test-owned VMemApi (the fn-ptr seam working as
// designed - tests do not wait on the platform lane).
struct ColFixture { VMemApi api; ComponentTable t; };

bool col_fixture_init(ColFixture* f) {
    f->api = test_vmem_api();
    return column_init(&f->t, &ColPoint_info, "col.dense"_id, "col.entities"_id, "col.pages"_id,
                       &f->api) == ERR_OK;
}

}  // namespace

TL_TEST(column_add_get_remove_smoke, "core,ecs,column,smoke,fast") {
    ColFixture f;
    TL_ASSERT_TRUE(col_fixture_init(&f));
    TL_EXPECT_EQ(f.t.stride, (u32)sizeof(ColPoint));

    Entity a = handle_make<Entity>(0u, 1u);
    Entity b = handle_make<Entity>(4095u, 2u);   // same page as a, last slot
    ColPoint pa = { 1, 2, 3u };
    ColPoint pb = { -7, 8, 9u };
    column_add(&f.t, a, &pa);
    column_add(&f.t, b, &pb);
    TL_ASSERT_EQ(f.t.count, 2u);

    ColPoint* ga = (ColPoint*)column_get(&f.t, a);
    ColPoint* gb = (ColPoint*)column_get(&f.t, b);
    TL_ASSERT_NOT_NULL(ga);
    TL_ASSERT_NOT_NULL(gb);
    TL_EXPECT_EQ(ga->x, 1);
    TL_EXPECT_EQ(gb->x, -7);
    // Packed iteration order is insertion order until a remove reorders it.
    TL_EXPECT_EQ(f.t.entities[0].bits, a.bits);
    TL_EXPECT_EQ(f.t.entities[1].bits, b.bits);

    column_remove(&f.t, a);   // swap-remove: b moves into slot 0
    TL_ASSERT_EQ(f.t.count, 1u);
    TL_EXPECT_NULL(column_get(&f.t, a));
    ColPoint* gb2 = (ColPoint*)column_get(&f.t, b);
    TL_ASSERT_NOT_NULL(gb2);
    TL_EXPECT_EQ(gb2->tag, 9u);
    TL_EXPECT_EQ(f.t.entities[0].bits, b.bits);
}

TL_TEST(column_stale_generation_reads_absent, "core,ecs,column,edge,fast") {
    // The sparse index keys on the SLOT index; the entities[] bits compare is what makes a
    // reused slot's old handle read absent (docs/ECS.md §10.3's generation check).
    ColFixture f;
    TL_ASSERT_TRUE(col_fixture_init(&f));
    Entity old_e = handle_make<Entity>(17u, 1u);
    ColPoint v = { 5, 6, 7u };
    column_add(&f.t, old_e, &v);
    column_remove(&f.t, old_e);
    Entity new_e = handle_make<Entity>(17u, 2u);   // the slot reused at the next generation
    ColPoint v2 = { 8, 9, 10u };
    column_add(&f.t, new_e, &v2);
    TL_EXPECT_NULL(column_get(&f.t, old_e));            // stale: right slot, wrong generation
    TL_ASSERT_NOT_NULL(column_get(&f.t, new_e));
    // The null handle and a never-touched page are both absent, no assert (pure query).
    TL_EXPECT_NULL(column_get(&f.t, Entity{ 0 }));
    TL_EXPECT_NULL(column_get(&f.t, handle_make<Entity>(3000000u, 1u)));
}

TL_TEST(column_pages_commit_on_demand_across_the_domain, "core,ecs,column,edge,fast") {
    ColFixture f;
    TL_ASSERT_TRUE(col_fixture_init(&f));
    const u64 base_used = f.t.page_arena.used;   // the fixed page-pointer array
    TL_EXPECT_EQ(f.t.page_count, (u32)((((u64)Entity::IDX_MASK + 1u)) / ECS_PAGE_SIZE));

    // Page boundary pair (4095 | 4096) and the domain's far end - three distinct pages.
    Entity a = handle_make<Entity>(4095u, 1u);
    Entity b = handle_make<Entity>(4096u, 1u);
    Entity c = handle_make<Entity>(Entity::IDX_MASK, 1u);
    ColPoint v = { 1, 1, 1u };
    column_add(&f.t, a, &v);
    TL_EXPECT_EQ(f.t.page_arena.used - base_used, (u64)ECS_PAGE_SIZE * sizeof(u32));
    column_add(&f.t, b, &v);
    column_add(&f.t, c, &v);
    TL_EXPECT_EQ(f.t.page_arena.used - base_used, 3u * (u64)ECS_PAGE_SIZE * sizeof(u32));
    TL_ASSERT_NOT_NULL(column_get(&f.t, a));
    TL_ASSERT_NOT_NULL(column_get(&f.t, b));
    TL_ASSERT_NOT_NULL(column_get(&f.t, c));
    TL_EXPECT_NULL(column_get(&f.t, handle_make<Entity>(4094u, 1u)));   // committed page, empty slot
}

TL_TEST(column_vacated_rows_are_zero_inside_the_hashed_extent, "core,ecs,column,determinism,fast") {
    // The Array<T> ruling applied to columns: the dense/entity arenas' used never shrinks, so
    // [count*stride, used) is hashed forever - it must be all-zero after any remove pattern.
    // (Extent LENGTH is a function of the op history by design - lockstep peers share the
    // history; what must never leak is the CONTENT of removed rows. Same contract as Array.)
    ColFixture f;
    TL_ASSERT_TRUE(col_fixture_init(&f));
    Entity es[8];
    for (u32 i = 0; i < 8u; ++i) {
        es[i] = handle_make<Entity>(i, 1u);
        ColPoint v = { (i32)(i + 1u) * 3, -(i32)i, 0xA0u + i };
        column_add(&f.t, es[i], &v);
    }
    // Remove every other one, then one more - tail rows must scrub to zero.
    column_remove(&f.t, es[1]);
    column_remove(&f.t, es[3]);
    column_remove(&f.t, es[5]);
    column_remove(&f.t, es[7]);
    column_remove(&f.t, es[0]);
    TL_ASSERT_EQ(f.t.count, 3u);
    const u8* dense_tail = f.t.dense + (u64)f.t.count * f.t.stride;
    const u8* dense_end = f.t.dense_arena.base + f.t.dense_arena.used;
    bool all_zero = true;
    for (const u8* p = dense_tail; p < dense_end; ++p) { all_zero = all_zero && *p == 0u; }
    TL_EXPECT_TRUE(all_zero);
    for (u32 i = f.t.count; i < 8u; ++i) { TL_EXPECT_EQ(f.t.entities[i].bits, 0u); }
}

TL_TEST(column_100k_random_ops_match_a_naive_model, "core,ecs,column,property,fast") {
    // Random add/remove/probe over a 512-slot index range, model-checked at every step for the
    // touched entity and in bulk (count + full sweep) every 4096 ops.
    enum : u32 { RANGE = 512, OPS = 100000 };
    ColFixture f;
    TL_ASSERT_TRUE(col_fixture_init(&f));
    static ModelSlot model[RANGE];   // static: a test body runs once per process (isolate)
    memset(model, 0, sizeof(model));

    u32 live = 0;
    u32 checked = 0;
    for (u32 op = 0; op < OPS; ++op) {
        const u64 r = rng_for(tl_seed_for(t->seed, 1u), op, 1u, 0u, 0u);
        const u32 idx = (u32)(r % RANGE);
        ModelSlot* m = &model[idx];
        if (!m->present) {
            const u32 gen = 1u + (u32)((r >> 32) % 1000u);
            m->e = handle_make<Entity>(idx, gen);
            m->v.x = (i32)(u32)(r >> 8);
            m->v.y = (i32)(u32)(r >> 16);
            m->v.tag = (u32)(r >> 24);
            m->present = 1;
            column_add(&f.t, m->e, &m->v);
            live += 1u;
        } else if ((r >> 40) & 1u) {
            column_remove(&f.t, m->e);
            m->present = 0;
            live -= 1u;
            if (column_get(&f.t, m->e) != nullptr) { checked = 0xFFFFFFFFu; break; }
        } else {
            ColPoint* got = (ColPoint*)column_get(&f.t, m->e);
            if (got == nullptr || memcmp(got, &m->v, sizeof(ColPoint)) != 0) { checked = 0xFFFFFFFFu; break; }
            ++checked;
        }
        if ((op & 4095u) == 0u) {
            if (f.t.count != live) { checked = 0xFFFFFFFFu; break; }
            u32 seen = 0;
            for (u32 d = 0; d < f.t.count; ++d) {
                const Entity e = f.t.entities[d];
                const ModelSlot* ms = &model[handle_index(e)];
                if (!ms->present || ms->e.bits != e.bits) { seen = 0xFFFFFFFFu; break; }
                const ColPoint* row = (const ColPoint*)(f.t.dense + (u64)d * f.t.stride);
                if (memcmp(row, &ms->v, sizeof(ColPoint)) != 0) { seen = 0xFFFFFFFFu; break; }
                ++seen;
            }
            if (seen != f.t.count) { checked = 0xFFFFFFFFu; break; }
        }
    }
    TL_ASSERT_NE(checked, 0xFFFFFFFFu);   // any model mismatch above lands here with the op state lost - rerun with --seed to reproduce
    TL_EXPECT_GT(checked, OPS / 8u);      // the probe leg actually ran (vacuity guard)
    TL_EXPECT_EQ(f.t.count, live);
}
