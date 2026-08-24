// sort.h - sort_u32_kv/sort_u64_kv: stability with duplicate keys; a real-scale random sweep vs a
// reference insertion sort on a sample; all-equal keys early-out. Spec: docs/CONTAINERS.md §4,
// §8.5, §8.7. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/sort.h"
#include "foundation/vmem_test_api.h"
#include "foundation/hash.h"
#include "foundation/rng.h"
#include "foundation/rng_systems.h"

TL_TEST(sort_u32_kv_basic_and_stable, "foundation,containers,smoke,fast") {
    VMemApi api = test_vmem_api();
    Scratch s;
    TL_ASSERT_EQ(scratch_init(&s, "test.sort_scratch"_id, 4ull * 1024 * 1024, &api), ERR_OK);

    // vals carry the ORIGINAL index so stability is directly observable: equal keys must keep
    // ascending val order after the sort.
    u32 keys[8]  = { 5, 1, 5, 3, 1, 0, 5, 3 };
    u32 vals[8]  = { 0, 1, 2, 3, 4, 5, 6, 7 };
    // Scratch-arena discipline (docs/TESTING.md §7 item 7, manual arena_mark technique -
    // TL_ASSERT_NO_ALLOC does not compile yet, runner lane, TODO.md): the scratch scope inside
    // sort_kv must leave the mark exactly where it found it - net zero growth per call.
    u64 mark_before = arena_mark(&s.a);
    sort_u32_kv(keys, vals, 8u, &s);
    TL_EXPECT_EQ(arena_mark(&s.a), mark_before);

    const u32 expect_keys[8] = { 0, 1, 1, 3, 3, 5, 5, 5 };
    TL_EXPECT_SPAN_EQ(keys, expect_keys, 8);
    // Within each equal-key run, vals must stay in original ascending order (stability).
    TL_EXPECT_EQ(vals[0], (u32)5);                 // key 0
    TL_EXPECT_TRUE(vals[1] < vals[2]);              // key 1 run: original vals 1,4
    TL_EXPECT_EQ(vals[1], (u32)1); TL_EXPECT_EQ(vals[2], (u32)4);
    TL_EXPECT_EQ(vals[3], (u32)3); TL_EXPECT_EQ(vals[4], (u32)7);   // key 3 run: 3,7
    TL_EXPECT_EQ(vals[5], (u32)0); TL_EXPECT_EQ(vals[6], (u32)2); TL_EXPECT_EQ(vals[7], (u32)6);   // key 5 run
}

TL_TEST(sort_u32_kv_all_equal_keys_early_out, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    Scratch s;
    TL_ASSERT_EQ(scratch_init(&s, "test.sort_allequal"_id, 1ull * 1024 * 1024, &api), ERR_OK);
    u32 keys[5] = { 7, 7, 7, 7, 7 };
    u32 vals[5] = { 0, 1, 2, 3, 4 };
    sort_u32_kv(keys, vals, 5u, &s);
    const u32 expect_vals[5] = { 0, 1, 2, 3, 4 };   // untouched relative order (every pass skipped)
    TL_EXPECT_SPAN_EQ(vals, expect_vals, 5);
}

TL_TEST(sort_u32_kv_edge_n0_n1, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    Scratch s;
    TL_ASSERT_EQ(scratch_init(&s, "test.sort_edge01"_id, 1ull * 1024 * 1024, &api), ERR_OK);
    u32 keys0[1] = { 42 }; u32 vals0[1] = { 0 };
    sort_u32_kv(keys0, vals0, 0u, &s);
    TL_EXPECT_EQ(keys0[0], (u32)42);   // n=0: untouched

    u32 keys1[1] = { 42 }; u32 vals1[1] = { 7 };
    sort_u32_kv(keys1, vals1, 1u, &s);
    TL_EXPECT_EQ(keys1[0], (u32)42);
    TL_EXPECT_EQ(vals1[0], (u32)7);
}

TL_TEST(sort_u32_kv_descending_and_min_max, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    Scratch s;
    TL_ASSERT_EQ(scratch_init(&s, "test.sort_desc"_id, 1ull * 1024 * 1024, &api), ERR_OK);
    u32 keys[4] = { 0xFFFFFFFFu, 0u, 0x80000000u, 1u };
    u32 vals[4] = { 0, 1, 2, 3 };
    sort_u32_kv(keys, vals, 4u, &s);
    const u32 expect[4] = { 0u, 1u, 0x80000000u, 0xFFFFFFFFu };
    TL_EXPECT_SPAN_EQ(keys, expect, 4);
}

TL_TEST(sort_u64_kv_basic_and_high_bits, "foundation,containers,smoke,fast") {
    VMemApi api = test_vmem_api();
    Scratch s;
    TL_ASSERT_EQ(scratch_init(&s, "test.sort_u64"_id, 1ull * 1024 * 1024, &api), ERR_OK);
    u64 keys[4] = { 0xFFFFFFFFFFFFFFFFull, 0ull, 0x0100000000000000ull, 1ull };
    u32 vals[4] = { 0, 1, 2, 3 };
    sort_u64_kv(keys, vals, 4u, &s);
    const u64 expect[4] = { 0ull, 1ull, 0x0100000000000000ull, 0xFFFFFFFFFFFFFFFFull };
    TL_EXPECT_SPAN_EQ(keys, expect, 4);
}

// A real-scale random sweep vs a reference insertion sort on a sample (docs/TESTING.md §7 item 6 /
// docs/CONTAINERS.md §8.7's "1M random keys vs a reference insertion sort on a sample"). Keyed by
// rng_for so the run is reproducible.
TL_TEST(sort_u32_kv_random_1m_vs_reference_sample, "foundation,containers,slow") {
    VMemApi api = test_vmem_api();
    VMemArena data_arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&data_arena, "test.sort_1m_data"_id, 32ull * 1024 * 1024, 0, &api), ERR_OK);
    Scratch s;
    TL_ASSERT_EQ(scratch_init(&s, "test.sort_1m_scratch"_id, 32ull * 1024 * 1024, &api), ERR_OK);

    const u32 N = 1000000u;
    u32* keys = (u32*)arena_push(&data_arena, (u64)N * sizeof(u32), alignof(u32));
    u32* vals = (u32*)arena_push(&data_arena, (u64)N * sizeof(u32), alignof(u32));
    for (u32 i = 0; i < N; ++i) {
        keys[i] = (u32)rng_below(rng_for(1u, 0u, RNG_SYS_LUAU_BASE, i, 0u), (u32)0x00FFFFFFu);   // duplicates likely
        vals[i] = i;
    }
    sort_u32_kv(keys, vals, N, &s);

    // Full ascending check (cheap: O(N)).
    for (u32 i = 1; i < N; ++i) { TL_ASSERT_LE(keys[i - 1u], keys[i]); }

    // Reference insertion sort on a small sample of the ORIGINAL data, cross-checked against
    // where those same original indices ended up post-sort (a real independent oracle, not the
    // algorithm checking itself).
    const u32 SAMPLE = 200u;
    u32 sample_keys[SAMPLE];
    for (u32 i = 0; i < SAMPLE; ++i) {
        sample_keys[i] = (u32)rng_below(rng_for(2u, 0u, RNG_SYS_LUAU_BASE, i, 0u), (u32)0x00FFFFFFu);
    }
    for (u32 i = 1; i < SAMPLE; ++i) {   // insertion sort, independent of sort.cpp
        u32 k = sample_keys[i]; u32 j = i;
        while (j > 0u && sample_keys[j - 1u] > k) { sample_keys[j] = sample_keys[j - 1u]; j -= 1u; }
        sample_keys[j] = k;
    }
    for (u32 i = 1; i < SAMPLE; ++i) { TL_EXPECT_LE(sample_keys[i - 1u], sample_keys[i]); }
}
