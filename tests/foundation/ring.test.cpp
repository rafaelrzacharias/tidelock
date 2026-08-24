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
#if !TL_DEV
    // The trigger is TL_ASSERT, compiled out here - the child would run to a clean
    // exit and the tier-agnostic expect-fatal verdict would rightly score it FAIL.
    // The established pattern (fx_fatal.test.cpp): a visible TL_SKIP in the body.
    TL_SKIP("the trigger is TL_ASSERT, dev-only (docs/CPP-SUBSET.md section 7b)");
#endif
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

// head/tail are FREE-RUNNING u32 counters, masked only at the point of indexing, so a long-lived
// ring (the log ring, the netcode redundancy window) crosses 2^32 and ring_count's wrapping
// subtract has to stay exact across that boundary. Driven from just below the wrap so the test is
// a few dozen ops, not 2^32.
TL_TEST(ring_counters_wrap_past_2_32, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.ring_wrap32"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    RingBuffer<u32> r;
    ring_init(&r, &arena, 8u, true);
    r.head = 0xFFFFFFF8u; r.tail = 0xFFFFFFF8u;   // empty, eight pushes from the u32 wrap
    // 12 pushes into a cap-8 overwrite ring: tail lands at 0x00000004 (wrapped), head at
    // 0xFFFFFFFC (not yet) - the one window where the two counters straddle the u32 boundary and
    // a non-wrapping count would answer ~4 billion instead of 8.
    for (u32 i = 0; i < 12u; ++i) { TL_ASSERT_TRUE(ring_push(&r, i)); }
    TL_EXPECT_EQ(r.tail, (u32)0x00000004u);
    TL_EXPECT_EQ(r.head, (u32)0xFFFFFFFCu);
    TL_EXPECT_TRUE(r.tail < r.head);              // tail has wrapped past 0, head has not
    TL_EXPECT_EQ(ring_count(&r), (u32)8);          // the wrapping subtract is still exact
    TL_EXPECT_TRUE(ring_full(&r));
    for (u32 i = 0; i < 8u; ++i) { TL_EXPECT_EQ(ring_peek(&r, i), 4u + i); }   // oldest 8 survive
    TL_EXPECT_EQ(ring_pop(&r), (u32)4);
    TL_EXPECT_EQ(ring_count(&r), (u32)7);
    // ...and pushing on through the boundary keeps working.
    for (u32 i = 100u; i < 110u; ++i) { TL_ASSERT_TRUE(ring_push(&r, i)); }
    TL_EXPECT_EQ(ring_count(&r), (u32)8);
    TL_EXPECT_EQ(ring_peek(&r, 7u), (u32)109);
}

// Order-sensitivity is by DESIGN here, and worth pinning so nobody "fixes" it later: a ring's
// state is its two counters, so two rings holding identical elements after different push
// histories are NOT byte-identical. That is why CONTAINERS.md §4 lists the ring's consumers as
// event queues, the netcode window and the log ring - none of them registered/hashed state.
TL_TEST(ring_state_is_history_dependent_by_design, "foundation,containers,determinism,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena_a = {}, arena_b = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena_a, "test.ring_hist_a"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    TL_ASSERT_EQ(vmem_arena_init(&arena_b, "test.ring_hist_b"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    RingBuffer<u32> a, b;
    ring_init(&a, &arena_a, 4u, true);
    ring_init(&b, &arena_b, 4u, true);
    // A reaches {8,9} by overwriting; B reaches {8,9} directly.
    for (u32 i = 0; i <= 9u; ++i) { ring_push(&a, i); }
    ring_pop(&a); ring_pop(&a);
    ring_push(&b, 8u); ring_push(&b, 9u);
    TL_EXPECT_EQ(ring_count(&a), ring_count(&b));
    for (u32 i = 0; i < ring_count(&a); ++i) { TL_EXPECT_EQ(ring_peek(&a, i), ring_peek(&b, i)); }
    TL_EXPECT_TRUE(a.head != b.head);   // ...but the counters remember, and that is the contract
}

TL_TEST_EXPECT_FATAL(ring_pop_when_empty_is_fatal, "foundation,containers,fatal") {
#if !TL_DEV
    // The trigger is TL_ASSERT, compiled out here - the child would run to a clean
    // exit and the tier-agnostic expect-fatal verdict would rightly score it FAIL.
    // The established pattern (fx_fatal.test.cpp): a visible TL_SKIP in the body.
    TL_SKIP("the trigger is TL_ASSERT, dev-only (docs/CPP-SUBSET.md section 7b)");
#endif
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.ring_pop_empty"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    RingBuffer<u32> r;
    ring_init(&r, &arena, 4u, false);
    ring_push(&r, 1u);
    ring_pop(&r);
    ring_pop(&r);   // empty: TL_ASSERT
}

TL_TEST_EXPECT_FATAL(ring_peek_past_count_is_fatal, "foundation,containers,fatal") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.ring_peek_oob"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    RingBuffer<u32> r;
    ring_init(&r, &arena, 4u, false);
    ring_push(&r, 1u);
    ring_peek(&r, 1u);   // count is 1: TL_CHECK, all tiers
}
