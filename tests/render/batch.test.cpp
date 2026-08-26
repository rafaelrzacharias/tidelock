// batch.test.cpp - docs/RENDER2D.md §9.6 radix_order, batch_boundaries.
#include "runner/tl_test.h"
#include "render/render_internal.h"
#include "foundation/sort.h"
#include "foundation/vmem_test_api.h"
#include "foundation/rng.h"
#include "foundation/rng_systems.h"
#include "foundation/bitset.h"
#include "foundation/hash.h"
#include <time.h>

template <typename K>
static void ref_stable_insertion_sort(K* keys, u32* vals, u32 n) {
    for (u32 i = 1; i < n; ++i) {
        const K k = keys[i];
        const u32 v = vals[i];
        u32 j = i;
        while (j > 0 && keys[j - 1] > k) { keys[j] = keys[j - 1]; vals[j] = vals[j - 1]; --j; }
        keys[j] = k; vals[j] = v;
    }
}

// docs/RENDER2D.md §9.6: "sorted ascending; equal keys keep submission order (ties at every
// byte); 1M random keys vs a naive reference; all-identical 1M keys unchanged; < 30 ms". This is
// sort_u64_kv (docs/CONTAINERS.md §4) exercised over render's own key format (key_pack) - the
// containers lane's own suite (tests/foundation/sort.test.cpp) covers the primitive in general;
// this is the render-scale/render-format instance §9.6 names explicitly. 1M-scale, so "slow"
// tagged (docs/TESTING.md §6: PR lane runs --tag !slow) - same precedent as containers' own
// sort_u32_kv_random_1m_vs_reference_sample.
TL_TEST(radix_order, "render,slow") {
    VMemApi api = test_vmem_api();
    VMemArena data_arena{};
    TL_ASSERT_EQ(vmem_arena_init(&data_arena, "test.render_radix_data"_id, 64ull * 1024 * 1024, 0, &api), ERR_OK);
    Scratch s;
    TL_ASSERT_EQ(scratch_init(&s, "test.render_radix_scratch"_id, 32ull * 1024 * 1024, &api), ERR_OK);

    const u32 N = 1000000u;
    u64* keys = (u64*)arena_push(&data_arena, (u64)N * sizeof(u64), alignof(u64));
    u32* vals = (u32*)arena_push(&data_arena, (u64)N * sizeof(u32), alignof(u32));
    u64* orig_keys = (u64*)arena_push(&data_arena, (u64)N * sizeof(u64), alignof(u64));
    // A narrow (layer, depth, material) domain (4 * 1000 * 64 = 256,000 combos over 1M draws) -
    // pigeonhole guarantees heavy duplication, so the stability check below (dup_runs > 0) is
    // not vacuous (docs/LESSONS.md: "a seeded property test... COUNT what was tested").
    for (u32 i = 0; i < N; ++i) {
        const u8 layer = (u8)rng_below(rng_for(1u, 0u, RNG_SYS_LUAU_BASE, i, 0u), 4u);
        const u32 depth = (u32)rng_below(rng_for(1u, 0u, RNG_SYS_LUAU_BASE, i, 1u), 1000u);
        const u16 material = (u16)rng_below(rng_for(1u, 0u, RNG_SYS_LUAU_BASE, i, 2u), 64u);
        const u64 k = key_pack(layer, 0, depth, material, 0);
        keys[i] = k; orig_keys[i] = k; vals[i] = i;
    }

    // The sample oracle runs on a COPY of the first 200 original pairs, before the big sort
    // (docs/LESSONS.md: "the reference implementation oracle has to consume the output under
    // test" - both here read the SAME orig_keys sample, and the comparison is element-for-element).
    const u32 SAMPLE = 200u;
    u64 ref_k[SAMPLE]; u32 ref_v[SAMPLE]; u64 got_k[SAMPLE]; u32 got_v[SAMPLE];
    for (u32 i = 0; i < SAMPLE; ++i) { ref_k[i] = orig_keys[i]; ref_v[i] = i; got_k[i] = orig_keys[i]; got_v[i] = i; }
    ref_stable_insertion_sort<u64>(ref_k, ref_v, SAMPLE);
    sort_u64_kv(got_k, got_v, SAMPLE, &s);
    TL_EXPECT_SPAN_EQ(got_k, ref_k, SAMPLE);
    TL_EXPECT_SPAN_EQ(got_v, ref_v, SAMPLE);   // stability included

    const clock_t t0 = clock();
    sort_u64_kv(keys, vals, N, &s);
    const clock_t t1 = clock();
    const f64 ms = (f64)(t1 - t0) * 1000.0 / (f64)CLOCKS_PER_SEC;
    // Generous, non-strict smoke bound - WORKFLOW.md §4 owns real perf grading (the elected CI
    // leg); this only catches a gross algorithmic regression (e.g. an accidental O(n^2)).
    TL_EXPECT_LT(ms, 5000.0);

    u32 dup_runs = 0;
    for (u32 i = 1; i < N; ++i) {
        TL_ASSERT_LE(keys[i - 1u], keys[i]);
        if (keys[i] == keys[i - 1u]) { TL_ASSERT_TRUE(vals[i - 1u] < vals[i]); dup_runs += 1u; }
    }
    TL_EXPECT_TRUE(dup_runs > 0u);

    VMemArena seen_arena{};
    TL_ASSERT_EQ(vmem_arena_init(&seen_arena, "test.render_radix_seen"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    Bitset seen;
    bitset_init(&seen, &seen_arena, N);
    for (u32 i = 0; i < N; ++i) {
        TL_ASSERT_TRUE(vals[i] < N);
        TL_ASSERT_FALSE(bitset_test(&seen, vals[i]));
        bitset_set(&seen, vals[i]);
    }
    TL_EXPECT_EQ(bitset_popcount(&seen), N);
    for (u32 i = 0; i < N; ++i) { TL_ASSERT_EQ(keys[i], orig_keys[vals[i]]); }

    // all-identical 1M keys unchanged (every pass' histogram is single-bucket -> skipped).
    u64* keys2 = (u64*)arena_push(&data_arena, (u64)N * sizeof(u64), alignof(u64));
    u32* vals2 = (u32*)arena_push(&data_arena, (u64)N * sizeof(u32), alignof(u32));
    const u64 same_key = key_pack(5, 0, 0, 7, 0);
    for (u32 i = 0; i < N; ++i) { keys2[i] = same_key; vals2[i] = i; }
    sort_u64_kv(keys2, vals2, N, &s);
    for (u32 i = 0; i < N; ++i) {
        TL_ASSERT_EQ(keys2[i], same_key);
        TL_ASSERT_EQ(vals2[i], i);
    }
}

static RenderQueue make_batch_queue(VMemArena* arena, u32 cap) {
    RenderQueue q{};
    array_init_fixed(&q.keys, arena, cap);
    array_init_fixed(&q.data_index, arena, cap);
    array_init_fixed(&q.clip_id, arena, cap);
    array_init_fixed(&q.order, arena, cap);
    array_init_fixed(&q.batches, arena, cap);
    return q;
}

TL_TEST(batch_boundaries, "render") {
    VMemApi api = test_vmem_api();
    VMemArena arena{};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.render_batch_arena"_id, 4ull * 1024 * 1024, 0, &api), ERR_OK);
    Scratch s;
    TL_ASSERT_EQ(scratch_init(&s, "test.render_batch_scratch"_id, 1ull * 1024 * 1024, &api), ERR_OK);

    // empty queue -> 0 batches.
    {
        RenderQueue q = make_batch_queue(&arena, 16);
        render_sort_and_batch(&q, &s);
        TL_EXPECT_EQ(q.batches.count, 0u);
    }

    // depth never splits a batch: two commands, same (tex, layer, blend, clip), submitted with
    // depth in DESCENDING order so the sort must actually reorder them to prove it - if depth
    // were (wrongly) a batch key they would land in two singleton batches regardless of order.
    {
        RenderQueue q = make_batch_queue(&arena, 16);
        array_push(&q.keys, key_pack(0, 0, 200, 1, 0)); array_push(&q.clip_id, (u16)5); array_push(&q.data_index, 0u);
        array_push(&q.keys, key_pack(0, 0, 100, 1, 0)); array_push(&q.clip_id, (u16)5); array_push(&q.data_index, 1u);
        render_sort_and_batch(&q, &s);
        TL_ASSERT_EQ(q.batches.count, 1u);
        TL_EXPECT_EQ(q.batches.data[0].count, 2u);
        TL_EXPECT_EQ(q.batches.data[0].tex, (u16)1);
        TL_EXPECT_EQ(q.batches.data[0].clip, (u16)5);
    }

    // splits exactly on tex/clip/blend/layer changes: a base command plus four variants, each
    // differing from the base in exactly one of the four dimensions, all at the SAME depth so
    // depth cannot be the thing separating them. Submitted out of sorted order.
    {
        RenderQueue q = make_batch_queue(&arena, 16);
        const u32 d = 100u;
        // base: layer0 blend0 tex1 clip0
        array_push(&q.keys, key_pack(0, 0, d, 1, 0)); array_push(&q.clip_id, (u16)0); array_push(&q.data_index, 0u);
        // tex differs
        array_push(&q.keys, key_pack(0, 0, d, 2, 0)); array_push(&q.clip_id, (u16)0); array_push(&q.data_index, 1u);
        // clip differs
        array_push(&q.keys, key_pack(0, 0, d, 1, 0)); array_push(&q.clip_id, (u16)1); array_push(&q.data_index, 2u);
        // blend differs
        array_push(&q.keys, key_pack(0, 1, d, 1, 0)); array_push(&q.clip_id, (u16)0); array_push(&q.data_index, 3u);
        // layer differs
        array_push(&q.keys, key_pack(1, 0, d, 1, 0)); array_push(&q.clip_id, (u16)0); array_push(&q.data_index, 4u);
        const u32 n = q.keys.count;
        render_sort_and_batch(&q, &s);
        TL_EXPECT_EQ(q.batches.count, 5u);   // every one of the five is its own (tex,layer,blend,clip) combo
        u32 sum = 0;
        for (u32 i = 0; i < q.batches.count; ++i) { sum += q.batches.data[i].count; }
        TL_EXPECT_EQ(sum, n);
    }
}
