// sorted.h - SortedMap<K,V>/SortedSet<K>: kept-sorted invariant, memmove insert/remove, lower
// bound edges, two-instance determinism. Spec: docs/CONTAINERS.md §3, §8.4, §8.7. Rubric: TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/sorted.h"
#include "foundation/vmem_test_api.h"
#include "foundation/hash.h"

static bool sorted_map_keys_ascending(const SortedMap<u32, u32>* m) {
    for (u32 i = 1; i < m->keys.count; ++i) { if (m->keys.data[i - 1] >= m->keys.data[i]) { return false; } }
    return true;
}

TL_TEST(sorted_map_put_keeps_order, "foundation,containers,smoke,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.sorted_map"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    SortedMap<u32, u32> m;
    sorted_map_init(&m, &arena, 8u);

    const u32 keys[] = { 5, 1, 9, 3, 7 };
    for (u32 i = 0; i < 5u; ++i) { sorted_map_put(&m, keys[i], keys[i] * 100u); }
    TL_EXPECT_EQ(m.keys.count, (u32)5);
    TL_EXPECT_TRUE(sorted_map_keys_ascending(&m));
    const u32 expect[5] = {1,3,5,7,9};
    TL_EXPECT_SPAN_EQ(m.keys.data, expect, 5);

    u32* v = sorted_map_get(&m, 7u);
    TL_EXPECT_NOT_NULL(v);
    if (v) { TL_EXPECT_EQ(*v, (u32)700); }
    TL_EXPECT_TRUE(sorted_map_get(&m, 4u) == nullptr);

    sorted_map_put(&m, 5u, 999u);   // overwrite, no growth
    TL_EXPECT_EQ(m.keys.count, (u32)5);
    TL_EXPECT_EQ(*sorted_map_get(&m, 5u), (u32)999);
}

TL_TEST(sorted_map_remove_closes_gap, "foundation,containers,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.sorted_map_rm"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    SortedMap<u32, u32> m;
    sorted_map_init(&m, &arena, 8u);
    for (u32 i = 0; i < 5u; ++i) { sorted_map_put(&m, i, i); }
    TL_EXPECT_TRUE(sorted_map_remove(&m, 2u));
    TL_EXPECT_FALSE(sorted_map_remove(&m, 2u));
    TL_EXPECT_EQ(m.keys.count, (u32)4);
    TL_EXPECT_TRUE(sorted_map_keys_ascending(&m));
    const u32 expect[4] = {0,1,3,4};
    TL_EXPECT_SPAN_EQ(m.keys.data, expect, 4);
}

TL_TEST_EXPECT_FATAL(sorted_map_overflow_is_fatal, "foundation,containers,fatal") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.sorted_map_overflow"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    SortedMap<u32, u32> m;
    sorted_map_init(&m, &arena, 1u);
    sorted_map_put(&m, 1u, 1u);
    sorted_map_put(&m, 2u, 2u);   // overflow: TL_FATAL
}

TL_TEST(sorted_set_insert_contains_remove, "foundation,containers,smoke,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.sorted_set"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    SortedSet<u32> s;
    sorted_set_init(&s, &arena, 8u);

    TL_EXPECT_TRUE(sorted_set_insert(&s, 5u));
    TL_EXPECT_FALSE(sorted_set_insert(&s, 5u));   // already present
    TL_EXPECT_TRUE(sorted_set_insert(&s, 1u));
    TL_EXPECT_TRUE(sorted_set_insert(&s, 9u));
    TL_EXPECT_EQ(s.keys.count, (u32)3);
    const u32 expect[3] = {1,5,9};
    TL_EXPECT_SPAN_EQ(s.keys.data, expect, 3);

    TL_EXPECT_TRUE(sorted_set_contains(&s, 5u));
    TL_EXPECT_FALSE(sorted_set_contains(&s, 2u));
    TL_EXPECT_TRUE(sorted_set_remove(&s, 5u));
    TL_EXPECT_FALSE(sorted_set_contains(&s, 5u));
    TL_EXPECT_EQ(s.keys.count, (u32)2);
}

TL_TEST(sorted_edge_empty_and_single, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.sorted_edge"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    SortedSet<u32> s;
    sorted_set_init(&s, &arena, 4u);
    TL_EXPECT_EQ(s.keys.count, (u32)0);
    TL_EXPECT_FALSE(sorted_set_contains(&s, 1u));
    TL_EXPECT_FALSE(sorted_set_remove(&s, 1u));
    sorted_set_insert(&s, 42u);
    TL_EXPECT_TRUE(sorted_set_remove(&s, 42u));
    TL_EXPECT_EQ(s.keys.count, (u32)0);
}

// Order is a pure function of the KEY SET, not insertion order - two instances inserting the same
// keys in different sequences converge to identical arrays (docs/CONTAINERS.md §3).
TL_TEST(sorted_two_instance_determinism_different_insert_order, "foundation,containers,determinism,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena_a = {}, arena_b = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena_a, "test.sorted_det_a"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    TL_ASSERT_EQ(vmem_arena_init(&arena_b, "test.sorted_det_b"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    SortedSet<u32> a, b;
    sorted_set_init(&a, &arena_a, 16u);
    sorted_set_init(&b, &arena_b, 16u);

    const u32 order_a[] = { 5, 3, 8, 1, 9, 2 };
    const u32 order_b[] = { 9, 1, 2, 8, 5, 3 };
    for (u32 i = 0; i < 6u; ++i) { sorted_set_insert(&a, order_a[i]); }
    for (u32 i = 0; i < 6u; ++i) { sorted_set_insert(&b, order_b[i]); }

    TL_EXPECT_EQ(a.keys.count, b.keys.count);
    TL_EXPECT_SPAN_EQ(a.keys.data, b.keys.data, a.keys.count);
}

// R5 (RULED 2026-08-24, TODO.md; docs/CONTAINERS.md §8.4): sorted_map_iter returns bool, in
// map_iter's exact shape - one iterator idiom per module. This is also the FIRST call site the
// template has ever had, so before this test no compiler had type-checked it (LESSONS: "a template
// with no call site has never been compiled"); the void-returning version shipped uninstantiated.
//
// The loop below is written the way a consumer writes it, and is the whole point of the change:
// the return value bounds the walk, so the caller never reads `count` at all.
TL_TEST(sorted_map_iter_returns_bool_like_map_iter, "foundation,containers,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.sorted_iter"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    SortedMap<u32, u32> m;
    sorted_map_init(&m, &arena, 16u);

    // Empty: the very first call returns false and touches neither out-param.
    u32 it = 0, k = 0xABCDu, v = 0xEF01u;
    TL_EXPECT_FALSE(sorted_map_iter(&m, &it, &k, &v));
    TL_EXPECT_EQ(k, (u32)0xABCD);
    TL_EXPECT_EQ(v, (u32)0xEF01);
    TL_EXPECT_EQ(it, (u32)0);   // a refused step does not advance the cursor either

    const u32 keys_in[] = { 40u, 10u, 30u, 20u };   // inserted out of order; the walk is sorted
    for (u32 i = 0; i < 4u; ++i) { sorted_map_put(&m, keys_in[i], keys_in[i] * 3u); }

    u32 expect_k[4] = { 10u, 20u, 30u, 40u };
    u32 walked = 0;
    it = 0;
    while (sorted_map_iter(&m, &it, &k, &v)) {
        TL_ASSERT_TRUE(walked < 4u);
        TL_EXPECT_EQ(k, expect_k[walked]);
        TL_EXPECT_EQ(v, expect_k[walked] * 3u);
        walked += 1u;
    }
    TL_EXPECT_EQ(walked, (u32)4);
    TL_EXPECT_EQ(it, m.keys.count);   // the cursor stopped exactly at count

    // Exhausted, and idempotent there: further calls keep returning false without running off the
    // array or advancing - the case rev 1 spent a TL_ASSERT on.
    k = 0x1111u; v = 0x2222u;
    TL_EXPECT_FALSE(sorted_map_iter(&m, &it, &k, &v));
    TL_EXPECT_FALSE(sorted_map_iter(&m, &it, &k, &v));
    TL_EXPECT_EQ(k, (u32)0x1111);
    TL_EXPECT_EQ(v, (u32)0x2222);
    TL_EXPECT_EQ(it, (u32)4);

    // A cursor started past the end is refused, not read: the bound is >=, not ==.
    it = 99u;
    TL_EXPECT_FALSE(sorted_map_iter(&m, &it, &k, &v));
    TL_EXPECT_EQ(it, (u32)99);
}
