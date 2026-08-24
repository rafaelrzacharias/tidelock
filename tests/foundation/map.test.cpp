// map.h - put/get/remove model vs a naive array; backward-shift correctness; two instances same
// op sequence -> identical iteration. Spec: docs/CONTAINERS.md §3, §8.3, §8.7. Rubric: TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/map.h"
#include "foundation/vmem_test_api.h"
#include "foundation/hash.h"

TL_TEST(map_put_get_overwrite, "foundation,containers,smoke,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.map"_id, 4ull * 1024 * 1024, 0, &api), ERR_OK);
    Map<u32, u32> m;
    map_init(&m, &arena, 4u);

    TL_EXPECT_TRUE(map_get(&m, 42u) == nullptr);   // absent
    map_put(&m, 1u, 100u);
    map_put(&m, 2u, 200u);
    TL_EXPECT_EQ(*map_get(&m, 1u), (u32)100);
    TL_EXPECT_EQ(*map_get(&m, 2u), (u32)200);
    TL_EXPECT_EQ(map_count(&m), (u32)2);

    map_put(&m, 1u, 999u);   // overwrite, no count change
    TL_EXPECT_EQ(*map_get(&m, 1u), (u32)999);
    TL_EXPECT_EQ(map_count(&m), (u32)2);
}

TL_TEST(map_remove_backward_shift, "foundation,containers,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.map_remove"_id, 4ull * 1024 * 1024, 0, &api), ERR_OK);
    // A small capacity forces collisions on the same probe run, exercising backward-shift.
    Map<u32, u32> m;
    map_init(&m, &arena, 8u);
    for (u32 i = 0; i < 6u; ++i) { map_put(&m, i, i * 10u); }
    TL_EXPECT_TRUE(map_remove(&m, 2u));
    TL_EXPECT_FALSE(map_remove(&m, 2u));   // already gone
    TL_EXPECT_TRUE(map_get(&m, 2u) == nullptr);
    // Every surviving key must still be reachable after the shift (this is the correctness bar
    // tombstone-free deletion has to clear - a broken shift strands a key behind a false empty).
    for (u32 i = 0; i < 6u; ++i) {
        if (i == 2u) { continue; }
        u32* v = map_get(&m, i);
        TL_EXPECT_NOT_NULL(v);
        if (v) { TL_EXPECT_EQ(*v, i * 10u); }
    }
    TL_EXPECT_EQ(map_count(&m), (u32)5);
}

TL_TEST(map_grow_rehash_preserves_entries, "foundation,containers,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.map_grow"_id, 4ull * 1024 * 1024, 0, &api), ERR_OK);
    Map<u32, u32> m;
    map_init(&m, &arena, 2u);   // tiny - forces several grows
    for (u32 i = 0; i < 200u; ++i) { map_put(&m, i, i * 3u); }
    TL_EXPECT_EQ(map_count(&m), (u32)200);
    for (u32 i = 0; i < 200u; ++i) {
        u32* v = map_get(&m, i);
        TL_EXPECT_NOT_NULL(v);
        if (v) { TL_EXPECT_EQ(*v, i * 3u); }
    }
}

// put/get/remove model vs a naive reference array (docs/TESTING.md §7 item 6 - property vs a
// naive model), over a fixed deterministic op sequence (no RNG dependency needed here - the
// sequence itself is the coverage).
TL_TEST(map_model_vs_naive_reference, "foundation,containers,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.map_model"_id, 4ull * 1024 * 1024, 0, &api), ERR_OK);
    Map<u32, u32> m;
    map_init(&m, &arena, 4u);

    bool ref_present[64] = {};
    u32 ref_val[64] = {};
    for (u32 step = 0; step < 300u; ++step) {
        u32 k = (step * 7u + 3u) % 64u;
        if (step % 3u == 0u) {
            map_put(&m, k, step);
            ref_present[k] = true; ref_val[k] = step;
        } else if (step % 3u == 1u) {
            bool removed = map_remove(&m, k);
            TL_EXPECT_EQ(removed, ref_present[k]);
            ref_present[k] = false;
        } else {
            u32* v = map_get(&m, k);
            TL_EXPECT_EQ(v != nullptr, ref_present[k]);
            if (v && ref_present[k]) { TL_EXPECT_EQ(*v, ref_val[k]); }
        }
    }
    u32 expect_count = 0;
    for (u32 i = 0; i < 64u; ++i) { if (ref_present[i]) { expect_count += 1u; } }
    TL_EXPECT_EQ(map_count(&m), expect_count);
}

TL_TEST(map_edge_zero_one_full_cycle, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.map_edge"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    Map<u32, u32> m;
    map_init(&m, &arena, 1u);   // rounds up to a real pow2 (2), edge: near-empty capacity
    TL_EXPECT_EQ(map_count(&m), (u32)0);
    map_put(&m, 5u, 50u);
    TL_EXPECT_EQ(map_count(&m), (u32)1);
    TL_EXPECT_TRUE(map_remove(&m, 5u));
    TL_EXPECT_EQ(map_count(&m), (u32)0);
    TL_EXPECT_TRUE(map_get(&m, 5u) == nullptr);
}

// Two instances fed the same op sequence produce identical bucket layout (docs/CONTAINERS.md §7 -
// "order-fragile per insertion sequence" is still a DETERMINISTIC function of that sequence).
TL_TEST(map_two_instance_determinism, "foundation,containers,determinism,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena_a = {}, arena_b = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena_a, "test.map_det_a"_id, 4ull * 1024 * 1024, 0, &api), ERR_OK);
    TL_ASSERT_EQ(vmem_arena_init(&arena_b, "test.map_det_b"_id, 4ull * 1024 * 1024, 0, &api), ERR_OK);
    Map<u32, u32> a, b;
    map_init(&a, &arena_a, 4u);
    map_init(&b, &arena_b, 4u);

    for (u32 i = 0; i < 50u; ++i) { map_put(&a, i, i * 2u); map_put(&b, i, i * 2u); }
    map_remove(&a, 10u); map_remove(&b, 10u);
    map_remove(&a, 20u); map_remove(&b, 20u);

    TL_EXPECT_EQ(a.cap, b.cap);
    TL_EXPECT_EQ(a.count, b.count);
    TL_EXPECT_MEM_EQ(a.state, b.state, (usize)a.cap);
    for (u32 i = 0; i < a.cap; ++i) {
        if (a.state[i] == MAP_SLOT_FULL) {
            TL_EXPECT_EQ(a.keys[i], b.keys[i]);
            TL_EXPECT_EQ(a.vals[i], b.vals[i]);
        }
    }
}
