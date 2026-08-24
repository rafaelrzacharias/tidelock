// ring.h - wrap, overwrite flag, push-when-full-without-overwrite, two-instance determinism.
// Spec: docs/CONTAINERS.md §4, §8.5, §8.7. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/ring.h"
#include "foundation/vmem_test_api.h"
#include "foundation/hash.h"

TL_TEST(ring_push_pop_peek_wrap, "foundation,containers,smoke,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.ring"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    RingBuffer<u32> r;
    ring_init(&r, &arena, 4u, false);

    TL_EXPECT_TRUE(ring_empty(&r));
    TL_EXPECT_TRUE(ring_push(&r, 1u));
    TL_EXPECT_TRUE(ring_push(&r, 2u));
    TL_EXPECT_TRUE(ring_push(&r, 3u));
    TL_EXPECT_EQ(ring_count(&r), (u32)3);
    TL_EXPECT_EQ(ring_peek(&r, 0u), (u32)1);
    TL_EXPECT_EQ(ring_peek(&r, 2u), (u32)3);

    TL_EXPECT_EQ(ring_pop(&r), (u32)1);
    TL_EXPECT_TRUE(ring_push(&r, 4u));
    TL_EXPECT_TRUE(ring_push(&r, 5u));   // wraps past cap in the underlying storage
    TL_EXPECT_TRUE(ring_full(&r));
    TL_EXPECT_EQ(ring_count(&r), (u32)4);
    TL_EXPECT_EQ(ring_pop(&r), (u32)2);
    TL_EXPECT_EQ(ring_pop(&r), (u32)3);
    TL_EXPECT_EQ(ring_pop(&r), (u32)4);
    TL_EXPECT_EQ(ring_pop(&r), (u32)5);
    TL_EXPECT_TRUE(ring_empty(&r));
}

TL_TEST(ring_full_without_overwrite_rejects_push, "foundation,containers,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.ring_noverwrite"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    RingBuffer<u32> r;
    ring_init(&r, &arena, 2u, false);
    TL_EXPECT_TRUE(ring_push(&r, 10u));
    TL_EXPECT_TRUE(ring_push(&r, 20u));
    TL_EXPECT_FALSE(ring_push(&r, 30u));   // full, no overwrite -> rejected
    TL_EXPECT_EQ(ring_peek(&r, 0u), (u32)10);
    TL_EXPECT_EQ(ring_peek(&r, 1u), (u32)20);
}

TL_TEST(ring_overwrite_oldest_flag, "foundation,containers,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.ring_overwrite"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    RingBuffer<u32> r;
    ring_init(&r, &arena, 2u, true);
    TL_EXPECT_TRUE(ring_push(&r, 10u));
    TL_EXPECT_TRUE(ring_push(&r, 20u));
    TL_EXPECT_TRUE(ring_push(&r, 30u));   // overwrites 10
    TL_EXPECT_EQ(ring_count(&r), (u32)2);
    TL_EXPECT_EQ(ring_peek(&r, 0u), (u32)20);
    TL_EXPECT_EQ(ring_peek(&r, 1u), (u32)30);
}

TL_TEST(ring_edge_cap_one, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.ring_cap1"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    RingBuffer<u32> r;
    ring_init(&r, &arena, 1u, true);
    TL_EXPECT_TRUE(ring_push(&r, 1u));
    TL_EXPECT_TRUE(ring_push(&r, 2u));   // overwrites the only slot
    TL_EXPECT_EQ(ring_count(&r), (u32)1);
    TL_EXPECT_EQ(ring_peek(&r, 0u), (u32)2);
}

TL_TEST_EXPECT_FATAL(ring_non_pow2_cap_is_fatal, "foundation,containers,fatal") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.ring_badcap"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    RingBuffer<u32> r;
    ring_init(&r, &arena, 3u, false);   // not a power of two: TL_ASSERT (dev-tier fatal)
}

TL_TEST(ring_two_instance_determinism, "foundation,containers,determinism,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena_a = {}, arena_b = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena_a, "test.ring_det_a"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    TL_ASSERT_EQ(vmem_arena_init(&arena_b, "test.ring_det_b"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    RingBuffer<u32> a, b;
    ring_init(&a, &arena_a, 8u, true);
    ring_init(&b, &arena_b, 8u, true);

    for (u32 i = 0; i < 20u; ++i) { ring_push(&a, i); ring_push(&b, i); }   // wraps with overwrite several times
    ring_pop(&a); ring_pop(&b);
    ring_push(&a, 999u); ring_push(&b, 999u);

    TL_EXPECT_EQ(ring_count(&a), ring_count(&b));
    for (u32 i = 0; i < ring_count(&a); ++i) { TL_EXPECT_EQ(ring_peek(&a, i), ring_peek(&b, i)); }
}
