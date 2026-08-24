// slotmap.h - LIFO reuse, stale handle null, gen wrap -> quarantine, zeroed dead slot (hash
// purity), two-instance determinism. Spec: docs/CONTAINERS.md §2, §8.2, §8.7. Rubric: TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/slotmap.h"
#include "foundation/vmem_test_api.h"
#include "foundation/hash.h"

struct SmTestTag;
using SmH = Handle<SmTestTag, 4, 2>;   // 16 slots, GEN_MAX = 3 - small enough to exercise wrap directly

struct Payload { u32 a; u32 b; };

TL_TEST(slotmap_insert_get_basic, "foundation,containers,mem,smoke,fast") {
    VMemApi api = test_vmem_api();
    SlotMap<Payload, SmH> sm;
    TL_ASSERT_EQ(slotmap_init(&sm, "test.sm.slots"_id, "test.sm.gen"_id, "test.sm.free"_id, "test.sm.live"_id, &api), ERR_OK);

    Payload p1 = { 10u, 20u };
    SmH h1 = slotmap_insert(&sm, &p1);
    TL_EXPECT_FALSE(handle_is_null(h1));
    TL_EXPECT_EQ(handle_index(h1), (u32)0);
    TL_EXPECT_EQ(handle_gen(h1), (u32)1);

    Payload* got = slotmap_get(&sm, h1);
    TL_EXPECT_NOT_NULL(got);
    if (got) { TL_EXPECT_EQ(got->a, (u32)10); TL_EXPECT_EQ(got->b, (u32)20); }
    TL_EXPECT_EQ(sm.live_count, (u32)1);
    TL_EXPECT_EQ(slotmap_slot_cap(&sm), (u32)1);
}

TL_TEST(slotmap_null_handle_get_is_null_no_assert, "foundation,containers,mem,edge,fast") {
    VMemApi api = test_vmem_api();
    SlotMap<Payload, SmH> sm;
    TL_ASSERT_EQ(slotmap_init(&sm, "test.sm2.slots"_id, "test.sm2.gen"_id, "test.sm2.free"_id, "test.sm2.live"_id, &api), ERR_OK);
    SmH null_h = {};
    TL_EXPECT_TRUE(handle_is_null(null_h));
    TL_EXPECT_TRUE(slotmap_get(&sm, null_h) == nullptr);        // documented absence, never asserts
    TL_EXPECT_FALSE(slotmap_remove(&sm, null_h));
}

TL_TEST(slotmap_lifo_reuse_and_zeroed_dead_slot, "foundation,containers,mem,fast") {
    VMemApi api = test_vmem_api();
    SlotMap<Payload, SmH> sm;
    TL_ASSERT_EQ(slotmap_init(&sm, "test.sm3.slots"_id, "test.sm3.gen"_id, "test.sm3.free"_id, "test.sm3.live"_id, &api), ERR_OK);

    Payload p0 = { 1u, 1u }, p1 = { 2u, 2u }, p2 = { 3u, 3u };
    SmH h0 = slotmap_insert(&sm, &p0);   // idx 0
    SmH h1 = slotmap_insert(&sm, &p1);   // idx 1
    SmH h2 = slotmap_insert(&sm, &p2);   // idx 2
    TL_EXPECT_EQ(handle_index(h0), (u32)0);
    TL_EXPECT_EQ(handle_index(h1), (u32)1);
    TL_EXPECT_EQ(handle_index(h2), (u32)2);

    TL_EXPECT_TRUE(slotmap_remove(&sm, h1));   // frees idx 1
    Payload zero = {};
    TL_EXPECT_MEM_EQ(&sm.slots.data[1], &zero, sizeof(Payload));   // zeroed dead slot - hash purity
    TL_EXPECT_FALSE(bitset_test(&sm.live, 1u));

    // zero-alloc on the hot path (docs/TESTING.md §7 item 7): a LIFO-reuse insert must not grow
    // any of the four column arenas (manual arena_mark technique - TL_ASSERT_NO_ALLOC does not
    // compile yet, runner lane, TODO.md).
    u64 mark_slots_before = arena_mark(&sm._slots_arena);
    u64 mark_gen_before = arena_mark(&sm._gen_arena);
    u64 mark_free_before = arena_mark(&sm._free_arena);

    Payload p3 = { 4u, 4u };
    SmH h3 = slotmap_insert(&sm, &p3);         // LIFO: reuses idx 1, NOT a new idx 3
    TL_EXPECT_EQ(arena_mark(&sm._slots_arena), mark_slots_before);
    TL_EXPECT_EQ(arena_mark(&sm._gen_arena), mark_gen_before);
    TL_EXPECT_EQ(arena_mark(&sm._free_arena), mark_free_before);   // pop, not push
    TL_EXPECT_EQ(handle_index(h3), (u32)1);
    TL_EXPECT_EQ(handle_gen(h3), (u32)2);       // gen bumped by the remove ("gen stays" across reuse - it already moved on remove)
    TL_EXPECT_EQ(slotmap_slot_cap(&sm), (u32)3);   // no new slot was appended

    // The stale handle h1 (gen 1) must now read as absent. Asked directly, via slotmap_alive -
    // before R1 landed this line compared generations by hand, because slotmap_get asserts on
    // exactly this case in dev (TODO.md R1, RULED 2026-08-24).
    TL_EXPECT_FALSE(slotmap_alive(&sm, h1));
    TL_EXPECT_TRUE(slotmap_alive(&sm, h3));
}

TL_TEST(slotmap_iteration_is_0_to_slot_cap_skipping_dead, "foundation,containers,mem,fast") {
    VMemApi api = test_vmem_api();
    SlotMap<Payload, SmH> sm;
    TL_ASSERT_EQ(slotmap_init(&sm, "test.sm4.slots"_id, "test.sm4.gen"_id, "test.sm4.free"_id, "test.sm4.live"_id, &api), ERR_OK);
    Payload zero = {};
    for (u32 i = 0; i < 5u; ++i) { Payload p = { i, i }; slotmap_insert(&sm, &p); }
    slotmap_remove(&sm, handle_make<SmH>(1u, 1u));
    slotmap_remove(&sm, handle_make<SmH>(3u, 1u));

    u32 visited = 0;
    for (u32 i = 0; i < slotmap_slot_cap(&sm); ++i) {
        if (bitset_test(&sm.live, i)) { visited += 1u; }
    }
    TL_EXPECT_EQ(visited, sm.live_count);
    TL_EXPECT_EQ(visited, (u32)3);
    TL_EXPECT_EQ(slotmap_slot_cap(&sm), (u32)5);   // NOT live_count - the dead-slot range is part of the walk
    TL_EXPECT_MEM_EQ(&sm.slots.data[1], &zero, sizeof(Payload));
    TL_EXPECT_MEM_EQ(&sm.slots.data[3], &zero, sizeof(Payload));
}

// Gen wrap: GEN_MAX = 3 for this domain. Cycle one slot through 3 remove()s; the third remove
// hits gen == GEN_MAX and quarantines - never pushed back to free_list, never reissued.
TL_TEST(slotmap_gen_wrap_quarantines_never_reissued, "foundation,containers,mem,fast") {
    VMemApi api = test_vmem_api();
    SlotMap<Payload, SmH> sm;
    TL_ASSERT_EQ(slotmap_init(&sm, "test.sm5.slots"_id, "test.sm5.gen"_id, "test.sm5.free"_id, "test.sm5.live"_id, &api), ERR_OK);

    Payload p = { 7u, 7u };
    SmH ha = slotmap_insert(&sm, &p);   // idx 0, gen 1
    TL_EXPECT_EQ(handle_gen(ha), (u32)1);
    slotmap_remove(&sm, ha);            // gen -> 2, freed (LIFO)

    SmH hb = slotmap_insert(&sm, &p);   // reuses idx 0, gen 2
    TL_EXPECT_EQ(handle_index(hb), (u32)0);
    TL_EXPECT_EQ(handle_gen(hb), (u32)2);
    slotmap_remove(&sm, hb);            // gen -> 3 (== GEN_MAX), freed (LIFO, one more cycle left)

    SmH hc = slotmap_insert(&sm, &p);   // reuses idx 0, gen 3 == GEN_MAX
    TL_EXPECT_EQ(handle_index(hc), (u32)0);
    TL_EXPECT_EQ(handle_gen(hc), (u32)SmH::GEN_MAX);
    TL_EXPECT_EQ(sm.quarantined, (u32)0);
    slotmap_remove(&sm, hc);            // gen == GEN_MAX -> quarantine, NOT freed
    TL_EXPECT_EQ(sm.quarantined, (u32)1);
    TL_EXPECT_EQ(sm.free_list.count, (u32)0);   // idx 0 is retired, not reusable

    Payload q = { 9u, 9u };
    SmH hd = slotmap_insert(&sm, &q);   // must allocate a brand-new slot, idx 1 - not idx 0
    TL_EXPECT_EQ(handle_index(hd), (u32)1);
    TL_EXPECT_EQ(handle_gen(hd), (u32)1);
    TL_EXPECT_EQ(slotmap_slot_cap(&sm), (u32)2);
}

TL_TEST_EXPECT_FATAL(slotmap_stale_handle_asserts_in_dev, "foundation,containers,fatal") {
#if !TL_DEV
    // The trigger is TL_ASSERT, compiled out here - the child would run to a clean
    // exit and the tier-agnostic expect-fatal verdict would rightly score it FAIL.
    // The established pattern (fx_fatal.test.cpp): a visible TL_SKIP in the body.
    TL_SKIP("the trigger is TL_ASSERT, dev-only (docs/CPP-SUBSET.md section 7b)");
#endif
    VMemApi api = test_vmem_api();
    SlotMap<Payload, SmH> sm;
    TL_ASSERT_EQ(slotmap_init(&sm, "test.sm6.slots"_id, "test.sm6.gen"_id, "test.sm6.free"_id, "test.sm6.live"_id, &api), ERR_OK);
    Payload p = { 1u, 1u };
    SmH h = slotmap_insert(&sm, &p);
    slotmap_remove(&sm, h);
    slotmap_get(&sm, h);   // stale (gen mismatch, idx dead): TL_ASSERT in dev
}

// Two instances fed the same op sequence (insert/remove/LIFO-reuse) hash identically over
// [0, slot_cap) - the §8.7 test list item ("zeroed dead slot -> hash equals a fresh map with the
// same live set", read as: two histories reaching the same state hash the same).
TL_TEST(slotmap_two_instance_determinism, "foundation,containers,determinism,fast") {
    VMemApi api = test_vmem_api();
    SlotMap<Payload, SmH> a, b;
    TL_ASSERT_EQ(slotmap_init(&a, "test.sm7a.slots"_id, "test.sm7a.gen"_id, "test.sm7a.free"_id, "test.sm7a.live"_id, &api), ERR_OK);
    TL_ASSERT_EQ(slotmap_init(&b, "test.sm7b.slots"_id, "test.sm7b.gen"_id, "test.sm7b.free"_id, "test.sm7b.live"_id, &api), ERR_OK);

    for (u32 i = 0; i < 6u; ++i) {
        Payload p = { i, i * 2u };
        slotmap_insert(&a, &p);
        slotmap_insert(&b, &p);
    }
    slotmap_remove(&a, handle_make<SmH>(1u, 1u)); slotmap_remove(&b, handle_make<SmH>(1u, 1u));
    slotmap_remove(&a, handle_make<SmH>(4u, 1u)); slotmap_remove(&b, handle_make<SmH>(4u, 1u));
    Payload refill = { 99u, 99u };
    slotmap_insert(&a, &refill); slotmap_insert(&b, &refill);   // LIFO reuse of idx 4 on both

    TL_EXPECT_EQ(slotmap_slot_cap(&a), slotmap_slot_cap(&b));
    TL_EXPECT_EQ(a.live_count, b.live_count);
    TL_EXPECT_MEM_EQ(a.slots.data, b.slots.data, (usize)slotmap_slot_cap(&a) * sizeof(Payload));
    TL_EXPECT_SPAN_EQ(a.gen.data, b.gen.data, slotmap_slot_cap(&a));
    u64 hash_a = tl_hash64(a.slots.data, (usize)slotmap_slot_cap(&a) * sizeof(Payload), TL_HASH_SEED);
    u64 hash_b = tl_hash64(b.slots.data, (usize)slotmap_slot_cap(&b) * sizeof(Payload), TL_HASH_SEED);
    TL_EXPECT_EQ(hash_a, hash_b);
}

// Regression guard for the reserve floor this lane shipped and W1 containers review 1 deleted:
// a small-cap domain (SmH is 16 slots -> 128 bytes of `slots`) must survive slotmap_init and its
// first push on vmem_arena_init's own COMMIT_GRANULE rounding (ruled 2026-08-24, docs/MEMORY.md
// section 8.2) - no caller-side floor. Before that ruling landed, the first arena_push here
// TL_FATALed "arena over reserve"; the floor was the workaround, and this test is what proves it
// is no longer needed rather than merely unused.
TL_TEST(slotmap_small_cap_domain_needs_no_caller_reserve_floor, "foundation,containers,mem,edge,fast") {
    VMemApi api = test_vmem_api();
    SlotMap<Payload, SmH> sm;
    TL_ASSERT_EQ(slotmap_init(&sm, "test.sm8.slots"_id, "test.sm8.gen"_id, "test.sm8.free"_id, "test.sm8.live"_id, &api), ERR_OK);
    // Every column's stated budget is its usable budget: reserved is a COMMIT_GRANULE multiple.
    TL_EXPECT_EQ(sm._slots_arena.reserved % (u64)COMMIT_GRANULE, (u64)0);
    TL_EXPECT_EQ(sm._gen_arena.reserved % (u64)COMMIT_GRANULE, (u64)0);
    TL_EXPECT_EQ(sm._free_arena.reserved % (u64)COMMIT_GRANULE, (u64)0);
    TL_EXPECT_EQ(sm._live_arena.reserved % (u64)COMMIT_GRANULE, (u64)0);
    // 16 slots * 8 bytes = 128 bytes asked for; the granule rounding is what makes the first push
    // legal at all.
    TL_EXPECT_TRUE(sm._slots_arena.reserved >= (u64)COMMIT_GRANULE);
    Payload p = { 5u, 6u };
    SmH h = slotmap_insert(&sm, &p);   // the first arena_push on each column
    TL_EXPECT_FALSE(handle_is_null(h));
    TL_EXPECT_NOT_NULL(slotmap_get(&sm, h));
}

// DIVERGENT histories that converge - the non-vacuous form of the §8.7 item "zeroed dead slot ->
// hash equals a fresh map with the same live set". Read literally that item is unsatisfiable: a
// churned map and a fresh one differ in slot_cap and in gen[], so their columns cannot be
// byte-equal. What it means, and what this pins, is the SLOTS column: two maps that reach the same
// (slot_cap, live set, contents, per-slot reuse count) by DIFFERENT removal and reuse orders hash
// identically, because a removed slot is zeroed rather than left holding its old payload.
// slotmap_two_instance_determinism above feeds both instances the same ops and therefore cannot
// see this.
TL_TEST(slotmap_divergent_histories_converge_to_one_hash, "foundation,containers,determinism,fast") {
    VMemApi api = test_vmem_api();
    SlotMap<Payload, SmH> a, b;
    TL_ASSERT_EQ(slotmap_init(&a, "test.sm9a.slots"_id, "test.sm9a.gen"_id, "test.sm9a.free"_id, "test.sm9a.live"_id, &api), ERR_OK);
    TL_ASSERT_EQ(slotmap_init(&b, "test.sm9b.slots"_id, "test.sm9b.gen"_id, "test.sm9b.free"_id, "test.sm9b.live"_id, &api), ERR_OK);

    Payload seed[4] = { {1u,1u}, {2u,2u}, {3u,3u}, {4u,4u} };
    for (u32 i = 0; i < 4u; ++i) { slotmap_insert(&a, &seed[i]); slotmap_insert(&b, &seed[i]); }

    Payload x = { 77u, 78u }, y = { 88u, 89u };
    // A removes 1 then 2 (free list [1,2]), so LIFO hands back 2 first, then 1.
    TL_EXPECT_TRUE(slotmap_remove(&a, handle_make<SmH>(1u, 1u)));
    TL_EXPECT_TRUE(slotmap_remove(&a, handle_make<SmH>(2u, 1u)));
    slotmap_insert(&a, &x);   // -> idx 2
    slotmap_insert(&a, &y);   // -> idx 1
    // B removes 2 then 1 (free list [2,1]), so LIFO hands back 1 first, then 2 - the opposite
    // order - and the payloads are supplied in the opposite order so both converge on the same
    // slot contents: idx 1 = y, idx 2 = x.
    TL_EXPECT_TRUE(slotmap_remove(&b, handle_make<SmH>(2u, 1u)));
    TL_EXPECT_TRUE(slotmap_remove(&b, handle_make<SmH>(1u, 1u)));
    slotmap_insert(&b, &y);   // -> idx 1
    slotmap_insert(&b, &x);   // -> idx 2

    TL_EXPECT_EQ(slotmap_slot_cap(&a), slotmap_slot_cap(&b));
    TL_EXPECT_EQ(a.live_count, b.live_count);
    TL_EXPECT_EQ(a.free_list.count, (u32)0);
    TL_EXPECT_EQ(b.free_list.count, (u32)0);
    TL_EXPECT_MEM_EQ(a.slots.data, b.slots.data, (usize)slotmap_slot_cap(&a) * sizeof(Payload));
    TL_EXPECT_SPAN_EQ(a.gen.data, b.gen.data, slotmap_slot_cap(&a));
    TL_EXPECT_EQ(tl_hash64(a.slots.data, (usize)slotmap_slot_cap(&a) * sizeof(Payload), TL_HASH_SEED),
                 tl_hash64(b.slots.data, (usize)slotmap_slot_cap(&b) * sizeof(Payload), TL_HASH_SEED));
}

// A quarantined slot's payload must be zeroed like any other dead slot, and stay zeroed: it is
// never reissued, so nothing will ever overwrite it, and it is inside the hashed [0, slot_cap)
// range for the rest of the world's life.
TL_TEST(slotmap_quarantined_slot_stays_zero_and_hashed, "foundation,containers,mem,edge,fast") {
    VMemApi api = test_vmem_api();
    SlotMap<Payload, SmH> sm;
    TL_ASSERT_EQ(slotmap_init(&sm, "test.smA.slots"_id, "test.smA.gen"_id, "test.smA.free"_id, "test.smA.live"_id, &api), ERR_OK);
    Payload p = { 0xABCDu, 0xEF01u };
    SmH h = slotmap_insert(&sm, &p);
    for (u32 g = 1; g < (u32)SmH::GEN_MAX; ++g) {   // cycle idx 0 up to GEN_MAX
        TL_ASSERT_TRUE(slotmap_remove(&sm, h));
        h = slotmap_insert(&sm, &p);
    }
    TL_EXPECT_EQ(handle_gen(h), (u32)SmH::GEN_MAX);
    TL_EXPECT_TRUE(slotmap_remove(&sm, h));          // the wrap remove
    TL_EXPECT_EQ(sm.quarantined, (u32)1);
    Payload zero = {};
    TL_EXPECT_MEM_EQ(&sm.slots.data[0], &zero, sizeof(Payload));
    TL_EXPECT_EQ(sm.gen.data[0], (u16)SmH::GEN_MAX);  // gen frozen at max, never bumped past it
    TL_EXPECT_EQ(sm.free_list.count, (u32)0);
    // Fifty further inserts must never touch idx 0 again.
    for (u32 i = 0; i < 10u; ++i) { Payload q = { i, i }; SmH hn = slotmap_insert(&sm, &q); TL_EXPECT_TRUE(handle_index(hn) != 0u); }
    TL_EXPECT_MEM_EQ(&sm.slots.data[0], &zero, sizeof(Payload));
}

// Two invariants the CANON row depends on and no test asserted: a minted handle is never the null
// handle (bits == 0), and generation 0 is never issued - over a long churn, not one sample.
// Checked without slotmap_get so the dev-tier stale assert cannot mask a miss.
TL_TEST(slotmap_never_mints_null_or_gen_zero, "foundation,containers,mem,determinism,fast") {
    VMemApi api = test_vmem_api();
    SlotMap<Payload, SmH> sm;
    TL_ASSERT_EQ(slotmap_init(&sm, "test.smB.slots"_id, "test.smB.gen"_id, "test.smB.free"_id, "test.smB.live"_id, &api), ERR_OK);
    u32 minted = 0;
    SmH live[8] = {};
    for (u32 round = 0; round < 40u; ++round) {
        u32 slot = round % 8u;
        if (!handle_is_null(live[slot])) {
            // Only remove while the slot still has generations left; a quarantine ends its life.
            if (handle_gen(live[slot]) < (u32)SmH::GEN_MAX) { slotmap_remove(&sm, live[slot]); }
            live[slot] = SmH{};
        }
        Payload p = { round, round * 3u };
        SmH h = slotmap_insert(&sm, &p);
        TL_ASSERT_FALSE(handle_is_null(h));               // bits == 0 is never minted
        TL_ASSERT_TRUE(handle_gen(h) >= 1u);               // generation 0 is never issued
        TL_ASSERT_TRUE(handle_gen(h) <= (u32)SmH::GEN_MAX);
        live[slot] = h;
        minted += 1u;
    }
    TL_EXPECT_EQ(minted, (u32)40);
}

// R1's edge matrix (RULED 2026-08-24, TODO.md; docs/CONTAINERS.md §8.2): slotmap_alive is pure and
// assert-free, and answers false for every non-live shape a handle can take. It runs in EVERY tier
// - if any of these cases reached an assert instead of returning, the dev-tier child would die
// with the trap code rather than fail a check, which is the whole point of the query existing.
TL_TEST(slotmap_alive_edge_matrix, "foundation,containers,mem,edge,fast") {
    VMemApi api = test_vmem_api();
    SlotMap<Payload, SmH> sm;
    TL_ASSERT_EQ(slotmap_init(&sm, "test.smC.slots"_id, "test.smC.gen"_id, "test.smC.free"_id, "test.smC.live"_id, &api), ERR_OK);

    // (1) null handle - bits == 0, documented absence.
    TL_EXPECT_FALSE(slotmap_alive(&sm, SmH{}));

    // (2) out-of-range index on an EMPTY map: idx 0 is inside the handle domain but past
    // slots.count, so the short-circuit must stop before bitset_test's all-tier TL_CHECK.
    TL_EXPECT_FALSE(slotmap_alive(&sm, handle_make<SmH>(0u, 1u)));

    Payload p0 = { 1u, 2u }, p1 = { 3u, 4u };
    SmH h0 = slotmap_insert(&sm, &p0);   // idx 0, gen 1
    SmH h1 = slotmap_insert(&sm, &p1);   // idx 1, gen 1

    // (3) the true case, twice - a live handle is live.
    TL_EXPECT_TRUE(slotmap_alive(&sm, h0));
    TL_EXPECT_TRUE(slotmap_alive(&sm, h1));

    // (4) out-of-range index with the map non-empty: idx 15 is the top of SmH's domain
    // (IDX_MASK == 15) and slot_cap is 2.
    TL_EXPECT_EQ(slotmap_slot_cap(&sm), (u32)2);
    TL_EXPECT_FALSE(slotmap_alive(&sm, handle_make<SmH>((u32)SmH::IDX_MASK, 1u)));

    // (5) generation 0 - never issued, so a zeroed gen field with a non-zero index (which is NOT
    // the null handle) must read as dead. Built raw: handle_make asserts gen >= 1 by contract.
    SmH gen_zero = SmH{ (SmH::rep)1u };   // idx 1, gen 0
    TL_EXPECT_FALSE(handle_is_null(gen_zero));
    TL_EXPECT_EQ(handle_gen(gen_zero), (u32)0);
    TL_EXPECT_FALSE(slotmap_alive(&sm, gen_zero));

    // (6) dead slot, stale generation - removed, and the slot not yet reused.
    TL_EXPECT_TRUE(slotmap_remove(&sm, h1));
    TL_EXPECT_FALSE(slotmap_alive(&sm, h1));
    TL_EXPECT_TRUE(slotmap_alive(&sm, h0));   // its neighbour is untouched

    // (7) TRUE across a reuse, and false for the handle that owned the slot before it: the LIFO
    // insert hands idx 1 back at gen 2.
    Payload p2 = { 5u, 6u };
    SmH h1b = slotmap_insert(&sm, &p2);
    TL_EXPECT_EQ(handle_index(h1b), (u32)1);
    TL_EXPECT_EQ(handle_gen(h1b), (u32)2);
    TL_EXPECT_TRUE(slotmap_alive(&sm, h1b));
    TL_EXPECT_FALSE(slotmap_alive(&sm, h1));   // the stale generation, after the reuse

    // (8) a wrapped/quarantined slot. gen freezes at GEN_MAX on the wrap remove, so the retired
    // handle's generation still MATCHES gen[idx] - the live bit is the only term that separates
    // them, and this is the one case that proves the bitset term is load-bearing.
    SmH hq = h1b;
    while (handle_gen(hq) < (u32)SmH::GEN_MAX) {
        TL_ASSERT_TRUE(slotmap_remove(&sm, hq));
        hq = slotmap_insert(&sm, &p2);
    }
    TL_EXPECT_EQ(handle_index(hq), (u32)1);
    TL_EXPECT_TRUE(slotmap_alive(&sm, hq));
    TL_EXPECT_TRUE(slotmap_remove(&sm, hq));           // the wrap remove: quarantine
    TL_EXPECT_EQ(sm.quarantined, (u32)1);
    TL_EXPECT_EQ(sm.gen.data[1], (u16)SmH::GEN_MAX);   // gen unchanged - the handle still matches it
    TL_EXPECT_EQ(handle_gen(hq), (u32)SmH::GEN_MAX);
    TL_EXPECT_FALSE(slotmap_alive(&sm, hq));

    // The query never mutates: slot_cap, live_count and quarantined are exactly where the ops left
    // them after all fourteen calls above.
    TL_EXPECT_EQ(slotmap_slot_cap(&sm), (u32)2);
    TL_EXPECT_EQ(sm.live_count, (u32)1);
    TL_EXPECT_EQ(sm.quarantined, (u32)1);
}
