// array.h - vmem growth across a page boundary keeps `data` stable; fixed overflow fatal-expected
// (deferred to TL_TEST_EXPECT_FATAL, docs/TESTING.md §9.1's isolate contract); swap_remove order
// model; two-instance determinism.
// Spec: docs/CONTAINERS.md §1, §8.1, §8.7 test list. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/array.h"
#include "foundation/vmem_test_api.h"
#include "foundation/hash.h"

TL_TEST(array_vmem_push_pop_swap_remove, "foundation,containers,smoke,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.array"_id, 4ull * 1024 * 1024, 0, &api), ERR_OK);
    Array<u32> a;
    array_init_vmem(&a, &arena);
    TL_EXPECT_EQ(a.count, (u32)0);
    TL_EXPECT_EQ(a.cap, (u32)0);
    void* base_before = a.data;
    for (u32 i = 0; i < 5000u; ++i) { array_push(&a, i); }
    TL_EXPECT_EQ(a.count, (u32)5000);
    TL_EXPECT_TRUE(a.cap >= a.count);
    TL_EXPECT_EQ((void*)a.data, base_before);   // stable base across every growth (docs/CONTAINERS.md section 1)
    for (u32 i = 0; i < 5000u; ++i) { TL_EXPECT_EQ(a.data[i], i); }

    // zero-alloc on the hot path (docs/TESTING.md §7 item 7): a push that stays within existing
    // cap must not touch the arena at all. TL_ASSERT_NO_ALLOC does not compile yet (runner lane,
    // TODO.md), so this is the same manual arena_mark technique tests/foundation/scratch.test.cpp
    // already uses.
    TL_EXPECT_TRUE(a.count < a.cap);
    u64 mark_before = arena_mark(&arena);
    array_push(&a, 12345u);
    TL_EXPECT_EQ(arena_mark(&arena), mark_before);

    // swap_remove: element 3 replaced by the last element, count drops by one; order changes -
    // a pure function of the call (docs/CONTAINERS.md section 1).
    u32 last_before = a.data[a.count - 1u];
    array_swap_remove(&a, 3u);
    TL_EXPECT_EQ(a.count, (u32)5000);   // 5001 (5000 + the zero-alloc push above) - 1
    TL_EXPECT_EQ(a.data[3], last_before);

    u32 popped = array_pop(&a);
    TL_EXPECT_EQ(popped, (u32)4999);   // the new last element after the swap_remove above
    TL_EXPECT_EQ(a.count, (u32)4999);
}

TL_TEST(array_fixed_capacity, "foundation,containers,smoke,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.array_fixed"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    Array<u16> a;
    array_init_fixed(&a, &arena, 8u);
    TL_EXPECT_EQ(a.cap, (u32)8);
    TL_EXPECT_TRUE(a.grow_arena == nullptr);
    for (u16 i = 0; i < 8u; ++i) { array_push(&a, i); }
    TL_EXPECT_EQ(a.count, (u32)8);
    const u16 expect[8] = {0,1,2,3,4,5,6,7};
    TL_EXPECT_SPAN_EQ(a.data, expect, 8);
}

// array_push overflow is TL_FATAL in every tier (not gated on TL_DEV, unlike TL_ASSERT) - always
// runs in a child process per docs/TESTING.md §9.1.
TL_TEST_EXPECT_FATAL(array_fixed_overflow_is_fatal, "foundation,containers,fatal") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.array_overflow"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    Array<u8> a;
    array_init_fixed(&a, &arena, 1u);
    array_push(&a, (u8)1);
    array_push(&a, (u8)2);   // overflow: TL_FATAL, never returns
}

TL_TEST(array_slice_and_span, "foundation,containers,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.array_slice"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    Array<u32> a;
    array_init_fixed(&a, &arena, 10u);
    for (u32 i = 0; i < 10u; ++i) { array_push(&a, i * 10u); }

    Span<u32> full = array_span(&a);
    TL_EXPECT_EQ(full.count, (u32)10);
    TL_EXPECT_EQ(full.data[0], (u32)0);

    Span<u32> mid = array_slice(&a, 2u, 5u);
    TL_EXPECT_EQ(mid.count, (u32)3);
    TL_EXPECT_EQ(mid.data[0], (u32)20);
    TL_EXPECT_EQ(mid.data[2], (u32)40);

    array_clear(&a);
    TL_EXPECT_EQ(a.count, (u32)0);
    TL_EXPECT_EQ(a.cap, (u32)10);   // clear does not release capacity (docs/CONTAINERS.md section 8.1)
    const u32 zeros[10] = {};
    TL_EXPECT_SPAN_EQ(a.data, zeros, 10);   // ...but it DOES zero what it cleared
}

// Two instances fed the same op sequence produce identical walk order and identical bytes
// (docs/CONTAINERS.md §7 - the containers determinism rubric).
TL_TEST(array_two_instance_determinism, "foundation,containers,determinism,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena_a = {}, arena_b = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena_a, "test.array_det_a"_id, 2ull * 1024 * 1024, 0, &api), ERR_OK);
    TL_ASSERT_EQ(vmem_arena_init(&arena_b, "test.array_det_b"_id, 2ull * 1024 * 1024, 0, &api), ERR_OK);
    Array<u32> a, b;
    array_init_vmem(&a, &arena_a);
    array_init_vmem(&b, &arena_b);

    const u32 ops[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    for (u32 i = 0; i < 10u; ++i) { array_push(&a, ops[i]); array_push(&b, ops[i]); }
    array_swap_remove(&a, 2u); array_swap_remove(&b, 2u);
    array_pop(&a); array_pop(&b);

    TL_EXPECT_EQ(a.count, b.count);
    TL_EXPECT_SPAN_EQ(a.data, b.data, a.count);
}

// The hashed extent of a vmem-backed Array is its arena's [base, used), which covers the WHOLE
// committed capacity - [count, cap) included. So "hashed bytes are a pure function of state, never
// of allocation history" (vmem_arena.h; docs/CPP-SUBSET.md §5) is a claim about pop/swap_remove/
// clear, not only about arena_push. Divergent histories that converge on the same logical contents
// must hash identically. Before the W1 containers review these three left their vacated elements
// intact and both of the tests below failed on the hash line.
TL_TEST(array_pop_and_swap_remove_hash_is_a_pure_function_of_state, "foundation,containers,determinism,fast") {
    VMemApi api = test_vmem_api();
    VMemArena a = {}, b = {};
    TL_ASSERT_EQ(vmem_arena_init(&a, "test.array_pure_a"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    TL_ASSERT_EQ(vmem_arena_init(&b, "test.array_pure_b"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    Array<u32> aa, ab;
    array_init_vmem(&aa, &a);
    array_init_vmem(&ab, &b);

    // A: push 1..5, pop twice, then swap_remove the middle.  B: reach the same contents directly.
    for (u32 i = 1; i <= 5u; ++i) { array_push(&aa, i); }
    array_pop(&aa); array_pop(&aa);          // 1,2,3
    array_swap_remove(&aa, 0u);              // 3,2
    array_push(&ab, 3u); array_push(&ab, 2u);

    TL_EXPECT_EQ(aa.count, ab.count);
    TL_EXPECT_SPAN_EQ(aa.data, ab.data, aa.count);
    TL_EXPECT_EQ(a.used, b.used);            // same page-granular commit on both
    TL_EXPECT_EQ(tl_hash64(a.base, (usize)a.used, TL_HASH_SEED),
                 tl_hash64(b.base, (usize)b.used, TL_HASH_SEED));
}

TL_TEST(array_clear_then_refill_hashes_like_a_fresh_array, "foundation,containers,determinism,fast") {
    VMemApi api = test_vmem_api();
    VMemArena a = {}, b = {};
    TL_ASSERT_EQ(vmem_arena_init(&a, "test.array_clear_a"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    TL_ASSERT_EQ(vmem_arena_init(&b, "test.array_clear_b"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    Array<u32> aa, ab;
    array_init_vmem(&aa, &a);
    array_init_vmem(&ab, &b);

    for (u32 i = 1; i <= 100u; ++i) { array_push(&aa, i); }
    array_clear(&aa);
    for (u32 i = 1; i <= 3u; ++i) { array_push(&aa, i); }
    for (u32 i = 1; i <= 3u; ++i) { array_push(&ab, i); }

    TL_EXPECT_EQ(a.used, b.used);
    TL_EXPECT_EQ(tl_hash64(a.base, (usize)a.used, TL_HASH_SEED),
                 tl_hash64(b.base, (usize)b.used, TL_HASH_SEED));
}
