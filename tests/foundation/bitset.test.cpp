// bitset.h - set/clear/test/find_first/popcount, the tail-bits-never-read rule, two-instance
// determinism. Spec: docs/CONTAINERS.md §4, §8.5, §8.7. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/bitset.h"
#include "foundation/vmem_test_api.h"
#include "foundation/hash.h"

TL_TEST(bitset_set_clear_test, "foundation,containers,smoke,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.bitset"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    Bitset b;
    bitset_init(&b, &arena, 130u);   // spans more than two words (130 = 2*64 + 2)

    for (u32 i = 0; i < 130u; ++i) { TL_EXPECT_FALSE(bitset_test(&b, i)); }   // fresh pages are OS-zero
    bitset_set(&b, 0u);
    bitset_set(&b, 63u);
    bitset_set(&b, 64u);
    bitset_set(&b, 129u);
    TL_EXPECT_TRUE(bitset_test(&b, 0u));
    TL_EXPECT_TRUE(bitset_test(&b, 63u));
    TL_EXPECT_TRUE(bitset_test(&b, 64u));
    TL_EXPECT_TRUE(bitset_test(&b, 129u));
    TL_EXPECT_FALSE(bitset_test(&b, 1u));
    TL_EXPECT_FALSE(bitset_test(&b, 128u));
    TL_EXPECT_EQ(bitset_popcount(&b), (u32)4);

    bitset_clear(&b, 63u);
    TL_EXPECT_FALSE(bitset_test(&b, 63u));
    TL_EXPECT_EQ(bitset_popcount(&b), (u32)3);
}

TL_TEST(bitset_find_first, "foundation,containers,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.bitset_ff"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    Bitset b;
    bitset_init(&b, &arena, 200u);
    TL_EXPECT_EQ(bitset_find_first(&b, 0u), (u32)200);   // none set -> bit_count sentinel

    bitset_set(&b, 5u);
    bitset_set(&b, 70u);
    TL_EXPECT_EQ(bitset_find_first(&b, 0u), (u32)5);
    TL_EXPECT_EQ(bitset_find_first(&b, 6u), (u32)70);
    TL_EXPECT_EQ(bitset_find_first(&b, 71u), (u32)200);
}

// The tail bits of the last word above bit_count are never read by any operation - a bit_count
// not a multiple of 64 must not let popcount/find_first see garbage past the edge.
TL_TEST(bitset_tail_bits_never_read, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.bitset_tail"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    Bitset b;
    bitset_init(&b, &arena, 5u);   // one word, only 5 of 64 bits meaningful
    for (u32 i = 0; i < 64u; ++i) { b.words[0] |= (u64(1) << i); }   // poke every bit including the tail
    TL_EXPECT_EQ(bitset_popcount(&b), (u32)5);            // only [0,5) counted
    TL_EXPECT_EQ(bitset_find_first(&b, 0u), (u32)0);
    TL_EXPECT_EQ(bitset_find_first(&b, 5u), (u32)5);       // bit 5 is out of range -> sentinel, even though the word bit is set
}

TL_TEST(bitset_edge_zero_and_one_bit, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.bitset_edge"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    Bitset zero;
    bitset_init(&zero, &arena, 0u);
    TL_EXPECT_EQ(bitset_popcount(&zero), (u32)0);
    TL_EXPECT_EQ(bitset_find_first(&zero, 0u), (u32)0);

    Bitset one;
    bitset_init(&one, &arena, 1u);
    TL_EXPECT_EQ(bitset_find_first(&one, 0u), (u32)1);
    bitset_set(&one, 0u);
    TL_EXPECT_EQ(bitset_popcount(&one), (u32)1);
}

TL_TEST(bitset_two_instance_determinism, "foundation,containers,determinism,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena_a = {}, arena_b = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena_a, "test.bitset_det_a"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    TL_ASSERT_EQ(vmem_arena_init(&arena_b, "test.bitset_det_b"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    Bitset a, b;
    bitset_init(&a, &arena_a, 300u);
    bitset_init(&b, &arena_b, 300u);

    const u32 ops[] = { 3, 17, 64, 199, 255, 299 };
    for (u32 i = 0; i < 6u; ++i) { bitset_set(&a, ops[i]); bitset_set(&b, ops[i]); }
    bitset_clear(&a, 64u); bitset_clear(&b, 64u);

    TL_EXPECT_EQ(bitset_popcount(&a), bitset_popcount(&b));
    u32 wc = bitset_word_count(300u);
    TL_EXPECT_MEM_EQ(a.words, b.words, (usize)wc * 8u);
}
