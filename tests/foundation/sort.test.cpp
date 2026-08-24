// sort.h - sort_u32_kv/sort_u64_kv: stability with duplicate keys; a real-scale random sweep vs a
// reference insertion sort on a sample; all-equal keys early-out. Spec: docs/CONTAINERS.md §4,
// §8.5, §8.7. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/sort.h"
#include "foundation/vmem_test_api.h"
#include "foundation/hash.h"
#include "foundation/rng.h"
#include "foundation/rng_systems.h"
#include "foundation/bitset.h"

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

// A stable insertion sort over (key, val) pairs - the independent oracle. Written here, in the
// test, over the same pairs the radix sees, so a wrong bucket, a lost element or a broken
// early-out shows up as a mismatch rather than as "the oracle also sorted its own array".
template <typename K>
static void ref_stable_insertion_sort(K* keys, u32* vals, u32 n) {
    for (u32 i = 1; i < n; ++i) {
        K k = keys[i]; u32 v = vals[i]; u32 j = i;
        while (j > 0u && keys[j - 1u] > k) { keys[j] = keys[j - 1u]; vals[j] = vals[j - 1u]; j -= 1u; }
        keys[j] = k; vals[j] = v;
    }
}

// Edge matrix the radix's own structure cares about: n = 2 (the smallest sort that can reorder),
// already-sorted and reverse-sorted input, and a run where only the HIGH byte differs (so passes
// 0..2 all early-out and only pass 3 moves anything - the case where a skipped pass leaving the
// source/destination pointers inconsistent would corrupt the result).
TL_TEST(sort_u32_kv_edge_n2_sorted_reversed_and_high_byte_only, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    Scratch s;
    TL_ASSERT_EQ(scratch_init(&s, "test.sort_edge2"_id, 1ull * 1024 * 1024, &api), ERR_OK);

    u32 k2[2] = { 9u, 4u }; u32 v2[2] = { 0u, 1u };
    sort_u32_kv(k2, v2, 2u, &s);
    TL_EXPECT_EQ(k2[0], (u32)4); TL_EXPECT_EQ(k2[1], (u32)9);
    TL_EXPECT_EQ(v2[0], (u32)1); TL_EXPECT_EQ(v2[1], (u32)0);

    u32 ks[6] = { 1u, 2u, 3u, 4u, 5u, 6u }; u32 vs[6] = { 0u, 1u, 2u, 3u, 4u, 5u };
    sort_u32_kv(ks, vs, 6u, &s);
    const u32 exp_s[6] = { 1u, 2u, 3u, 4u, 5u, 6u };
    const u32 exp_sv[6] = { 0u, 1u, 2u, 3u, 4u, 5u };
    TL_EXPECT_SPAN_EQ(ks, exp_s, 6);
    TL_EXPECT_SPAN_EQ(vs, exp_sv, 6);   // already sorted: unchanged, and still stable

    u32 kr[6] = { 6u, 5u, 4u, 3u, 2u, 1u }; u32 vr[6] = { 0u, 1u, 2u, 3u, 4u, 5u };
    sort_u32_kv(kr, vr, 6u, &s);
    const u32 exp_r[6] = { 1u, 2u, 3u, 4u, 5u, 6u };
    const u32 exp_rv[6] = { 5u, 4u, 3u, 2u, 1u, 0u };
    TL_EXPECT_SPAN_EQ(kr, exp_r, 6);
    TL_EXPECT_SPAN_EQ(vr, exp_rv, 6);

    // Only byte 3 carries ordering information; passes 0,1,2 have single-bucket histograms and
    // are skipped, so exactly one buffer swap happens and the copy-back path must fire.
    u32 kh[4] = { 0x03000000u, 0x01000000u, 0x02000000u, 0x01000000u };
    u32 vh[4] = { 0u, 1u, 2u, 3u };
    sort_u32_kv(kh, vh, 4u, &s);
    const u32 exp_h[4] = { 0x01000000u, 0x01000000u, 0x02000000u, 0x03000000u };
    const u32 exp_hv[4] = { 1u, 3u, 2u, 0u };   // the two 0x01... keep input order: stable
    TL_EXPECT_SPAN_EQ(kh, exp_h, 4);
    TL_EXPECT_SPAN_EQ(vh, exp_hv, 4);
}

// sort_u64_kv's stability was never exercised: the u64 test only checked ordering of distinct
// keys. Eight passes over equal keys is where an unstable placement would show.
TL_TEST(sort_u64_kv_stability_with_equal_keys, "foundation,containers,fast") {
    VMemApi api = test_vmem_api();
    Scratch s;
    TL_ASSERT_EQ(scratch_init(&s, "test.sort_u64_stable"_id, 1ull * 1024 * 1024, &api), ERR_OK);
    u64 keys[8] = { 0x0000000100000000ull, 5ull, 0x0000000100000000ull, 5ull,
                    0xFFFFFFFFFFFFFFFFull, 5ull, 0x0000000100000000ull, 0ull };
    u32 vals[8] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
    sort_u64_kv(keys, vals, 8u, &s);
    const u64 exp_k[8] = { 0ull, 5ull, 5ull, 5ull,
                           0x0000000100000000ull, 0x0000000100000000ull, 0x0000000100000000ull,
                           0xFFFFFFFFFFFFFFFFull };
    const u32 exp_v[8] = { 7u, 1u, 3u, 5u, 0u, 2u, 6u, 4u };
    TL_EXPECT_SPAN_EQ(keys, exp_k, 8);
    TL_EXPECT_SPAN_EQ(vals, exp_v, 8);
}

// The scratch buffer is normally carved from an arena that already holds the caller's own data (a
// broadphase pushes its key/val arrays from scratch, then calls the sort with the same Scratch).
// The sort's tmp buffers must sit ABOVE the caller's, never alias them, and its scope must close.
TL_TEST(sort_u32_kv_scratch_holding_the_callers_arrays, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    Scratch s;
    TL_ASSERT_EQ(scratch_init(&s, "test.sort_alias"_id, 4ull * 1024 * 1024, &api), ERR_OK);
    const u32 N = 512u;
    u32* keys = (u32*)scratch_push(&s, (u64)N * sizeof(u32), alignof(u32));
    u32* vals = (u32*)scratch_push(&s, (u64)N * sizeof(u32), alignof(u32));
    for (u32 i = 0; i < N; ++i) { keys[i] = (N - i) & 0xFFu; vals[i] = i; }
    u64 mark = arena_mark(&s.a);
    sort_u32_kv(keys, vals, N, &s);
    TL_EXPECT_EQ(arena_mark(&s.a), mark);   // the sort's scope is balanced, caller's data intact
    u32 dup_runs = 0;
    for (u32 i = 1; i < N; ++i) {
        TL_ASSERT_LE(keys[i - 1u], keys[i]);
        if (keys[i] == keys[i - 1u]) { TL_EXPECT_TRUE(vals[i - 1u] < vals[i]); dup_runs += 1u; }
    }
    TL_EXPECT_TRUE(dup_runs > 0u);
}

// A real-scale random sweep. Four independent oracles, none of which is the sort itself:
//   (a) fully ascending;
//   (b) the value column is a PERMUTATION of 0..N-1 - nothing lost, nothing duplicated (the
//       property the previous version of this test could not see at all);
//   (c) stability across the whole run - equal keys keep ascending original indices;
//   (d) a 200-element sample of the ORIGINAL pairs, sorted by the stable insertion sort above,
//       compared key-for-key AND val-for-val against sort_u32_kv on the same pairs.
// The version this replaces generated a SECOND random array, insertion-sorted it, and asserted
// that array was sorted - it never read the radix's output at all. Vacuous.
TL_TEST(sort_u32_kv_random_1m_vs_reference_sample, "foundation,containers,slow") {
    VMemApi api = test_vmem_api();
    VMemArena data_arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&data_arena, "test.sort_1m_data"_id, 64ull * 1024 * 1024, 0, &api), ERR_OK);
    Scratch s;
    TL_ASSERT_EQ(scratch_init(&s, "test.sort_1m_scratch"_id, 32ull * 1024 * 1024, &api), ERR_OK);

    const u32 N = 1000000u;
    u32* keys = (u32*)arena_push(&data_arena, (u64)N * sizeof(u32), alignof(u32));
    u32* vals = (u32*)arena_push(&data_arena, (u64)N * sizeof(u32), alignof(u32));
    u32* orig_keys = (u32*)arena_push(&data_arena, (u64)N * sizeof(u32), alignof(u32));
    for (u32 i = 0; i < N; ++i) {
        keys[i] = (u32)rng_below(rng_for(1u, 0u, RNG_SYS_LUAU_BASE, i, 0u), (u32)0x00FFFFFFu);   // duplicates likely
        orig_keys[i] = keys[i];
        vals[i] = i;
    }

    // The sample oracle runs on a COPY of the first 200 original pairs, before the big sort.
    const u32 SAMPLE = 200u;
    u32 ref_k[SAMPLE], ref_v[SAMPLE], got_k[SAMPLE], got_v[SAMPLE];
    for (u32 i = 0; i < SAMPLE; ++i) {
        ref_k[i] = orig_keys[i]; ref_v[i] = i;
        got_k[i] = orig_keys[i]; got_v[i] = i;
    }
    ref_stable_insertion_sort<u32>(ref_k, ref_v, SAMPLE);
    sort_u32_kv(got_k, got_v, SAMPLE, &s);
    TL_EXPECT_SPAN_EQ(got_k, ref_k, SAMPLE);
    TL_EXPECT_SPAN_EQ(got_v, ref_v, SAMPLE);   // (d) including stability

    sort_u32_kv(keys, vals, N, &s);

    u32 dup_runs = 0;
    for (u32 i = 1; i < N; ++i) {
        TL_ASSERT_LE(keys[i - 1u], keys[i]);                               // (a)
        if (keys[i] == keys[i - 1u]) {
            TL_ASSERT_TRUE(vals[i - 1u] < vals[i]);                        // (c)
            dup_runs += 1u;
        }
    }
    TL_EXPECT_TRUE(dup_runs > 0u);   // the stability check above is not vacuous on this data

    VMemArena seen_arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&seen_arena, "test.sort_1m_seen"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    Bitset seen;
    bitset_init(&seen, &seen_arena, N);
    for (u32 i = 0; i < N; ++i) {
        TL_ASSERT_TRUE(vals[i] < N);
        TL_ASSERT_FALSE(bitset_test(&seen, vals[i]));                      // (b) no duplicate
        bitset_set(&seen, vals[i]);
    }
    TL_EXPECT_EQ(bitset_popcount(&seen), N);                               // (b) nothing lost

    // ...and every surviving pair still carries the key it started with.
    for (u32 i = 0; i < N; ++i) { TL_ASSERT_EQ(keys[i], orig_keys[vals[i]]); }
}
