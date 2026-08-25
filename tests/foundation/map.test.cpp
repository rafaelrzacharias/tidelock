// map.h - put/get/remove model vs a naive array; backward-shift correctness; two instances same
// op sequence -> identical iteration. Spec: docs/CONTAINERS.md §3, §8.3, §8.7. Rubric: TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/map.h"
#include "foundation/vmem_test_api.h"
#include "foundation/hash.h"
#include <stdio.h>    // the map_fixed_overflow_names_its_fatal checker (TESTING.md §8 R-2)
#include <stdlib.h>   // system(), for the same checker
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>   // WIFEXITED/WEXITSTATUS for the checker's system() status
#endif

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
// NOTE the limit of this shape: same ops twice proves only that nothing address- or
// uninitialised-memory-dependent leaks into bucket choice. The load-bearing property lives in
// map_deletion_is_history_equivalent below.
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

// DIVERGENT histories that converge: backward-shift deletion (Knuth algorithm R) must leave the
// table byte-identical to one built by inserting only the survivors, in the same relative order.
// That is what "tombstone-free means no order dependence on deleted keys" actually asserts, and
// it is the property the same-ops-twice shape above cannot see. Keys are chosen to collide on one
// probe run at cap 16 (every key congruent mod nothing - the collisions come from the hash, so a
// dense block of small keys is used and the run is checked to be non-trivial by count).
TL_TEST(map_deletion_is_history_equivalent, "foundation,containers,determinism,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena_a = {}, arena_b = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena_a, "test.map_hist_a"_id, 4ull * 1024 * 1024, 0, &api), ERR_OK);
    TL_ASSERT_EQ(vmem_arena_init(&arena_b, "test.map_hist_b"_id, 4ull * 1024 * 1024, 0, &api), ERR_OK);
    Map<u32, u32> a, b;
    map_init(&a, &arena_a, 64u);   // fixed cap on both: no grow, so layout is comparable
    map_init(&b, &arena_b, 64u);

    // A: insert 0..23 then remove every third.  B: insert only the survivors, same relative order.
    for (u32 i = 0; i < 24u; ++i) { map_put(&a, i, i * 7u); }
    for (u32 i = 0; i < 24u; ++i) { if (i % 3u == 0u) { TL_EXPECT_TRUE(map_remove(&a, i)); } }
    for (u32 i = 0; i < 24u; ++i) { if (i % 3u != 0u) { map_put(&b, i, i * 7u); } }

    TL_EXPECT_EQ(a.count, b.count);
    TL_EXPECT_EQ(a.cap, b.cap);
    // Byte-identical across all three parallel arrays - keys and vals included, which only holds
    // because map_remove zeroes the slot it empties (W1 containers review 2).
    TL_EXPECT_MEM_EQ(a.state, b.state, (usize)a.cap);
    TL_EXPECT_MEM_EQ(a.keys, b.keys, (usize)a.cap * sizeof(u32));
    TL_EXPECT_MEM_EQ(a.vals, b.vals, (usize)a.cap * sizeof(u32));
    // ... and therefore identical iteration order, which is the consumer-visible form.
    u32 ita = 0, itb = 0; u32 ka = 0, kb = 0, va = 0, vb = 0; u32 walked = 0;
    while (map_iter(&a, &ita, &ka, &va)) {
        TL_ASSERT_TRUE(map_iter(&b, &itb, &kb, &vb));
        TL_EXPECT_EQ(ka, kb); TL_EXPECT_EQ(va, vb);
        walked += 1u;
    }
    TL_EXPECT_FALSE(map_iter(&b, &itb, &kb, &vb));
    TL_EXPECT_EQ(walked, a.count);
}

// map_init must not assume arena_push hands back zeros: below high_water they are zero only under
// ARENA_ZERO_ON_PUSH. A garbage `state` array reads every slot as MAP_SLOT_FULL and map_probe
// then spins forever hunting an empty slot - a hang, not a wrong answer (W1 containers review 2).
TL_TEST(map_init_zeroes_state_on_a_dirty_arena, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.map_dirty"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    void* p = arena_push(&arena, 64u * 1024u, 8u);
    memset(p, 0xDD, 64u * 1024u);       // dirty the range the map is about to be pushed into
    arena_reset_to(&arena, 0);
    Map<u32, u32> m;
    map_init(&m, &arena, 8u);
    u32 nonempty = 0;
    for (u32 i = 0; i < m.cap; ++i) { if (m.state[i] != MAP_SLOT_EMPTY) { nonempty += 1u; } }
    TL_EXPECT_EQ(nonempty, (u32)0);
    TL_EXPECT_TRUE(map_get(&m, 12345u) == nullptr);   // terminates only because state is clean
    map_put(&m, 12345u, 1u);
    TL_EXPECT_EQ(*map_get(&m, 12345u), (u32)1);
}

// The same dirty-arena rule for the rehash path: map_grow pushes three fresh blocks and must zero
// them, or a map that grows into reused arena bytes hits the same spin.
TL_TEST(map_grow_zeroes_state_on_a_dirty_arena, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.map_grow_dirty"_id, 1ull * 1024 * 1024, 0, &api), ERR_OK);
    void* p = arena_push(&arena, 128u * 1024u, 8u);
    memset(p, 0xDD, 128u * 1024u);
    arena_reset_to(&arena, 0);
    Map<u32, u32> m;
    map_init(&m, &arena, 2u);
    for (u32 i = 0; i < 100u; ++i) { map_put(&m, i, i); }   // several grows, all into dirty bytes
    u32 full = 0;
    for (u32 i = 0; i < m.cap; ++i) { if (m.state[i] == MAP_SLOT_FULL) { full += 1u; } }
    TL_EXPECT_EQ(full, (u32)100);
    TL_EXPECT_EQ(map_count(&m), (u32)100);
    TL_EXPECT_TRUE(map_get(&m, 99999u) == nullptr);
}

// R2 (RULED 2026-08-24, TODO.md; docs/MEMORY.md §1.2, docs/CONTAINERS.md §3): a fixed-mode Map is
// the enforcement of "any container on an ARENA_HASHED arena is sized at init". Two halves, both
// needed: this one proves a map sized at init serves its FULL specified load factor without ever
// touching its arena again (a fixed mode that fatals early would be enforcement by accident), and
// map_fixed_overflow_is_fatal proves the insert past it dies.
//
// The load bar is exact, not approximate: map_put grows when (count + 1) * 4 > cap * 3, so a
// cap-16 table accepts exactly 12 = 0.75 * 16 entries and the 13th is the one that would grow.
TL_TEST(map_fixed_serves_its_full_load_factor_without_growing, "foundation,containers,mem,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.map_fixed"_id, 4ull * 1024 * 1024, 0, &api), ERR_OK);
    Map<u32, u32> m;
    map_init_fixed(&m, &arena, 16u);
    TL_EXPECT_EQ(m.cap, (u32)16);
    TL_EXPECT_TRUE(m.arena == nullptr);   // Array<T>::grow_arena's shape: null = fixed

    // Every byte this Map will ever own is already below `used` - nothing it does from here can
    // orphan a block, which is the property that makes it legal on a hashed arena.
    u64 mark_after_init = arena_mark(&arena);

    for (u32 i = 0; i < 12u; ++i) { map_put(&m, i + 1u, (i + 1u) * 7u); }
    TL_EXPECT_EQ(map_count(&m), (u32)12);
    TL_EXPECT_EQ(m.cap, (u32)16);                          // never rehashed
    TL_EXPECT_EQ(arena_mark(&arena), mark_after_init);     // and never pushed

    for (u32 i = 0; i < 12u; ++i) {
        u32* v = map_get(&m, i + 1u);
        TL_ASSERT_NOT_NULL(v);
        TL_EXPECT_EQ(*v, (i + 1u) * 7u);
    }
    // A remove frees a slot, and refilling it does not grow: the fixed shape survives churn.
    TL_EXPECT_TRUE(map_remove(&m, 6u));
    TL_EXPECT_EQ(map_count(&m), (u32)11);
    map_put(&m, 1u, 999u);                     // overwrite below full load
    TL_EXPECT_EQ(*map_get(&m, 1u), (u32)999);
    TL_EXPECT_EQ(map_count(&m), (u32)11);
    map_put(&m, 100u, 55u);                    // refill, back to the full load
    TL_EXPECT_EQ(map_count(&m), (u32)12);
    TL_EXPECT_EQ(*map_get(&m, 100u), (u32)55);
    TL_EXPECT_EQ(arena_mark(&arena), mark_after_init);
    TL_EXPECT_EQ(m.cap, (u32)16);

    // An OVERWRITE at exactly the full load is a no-op insert and must neither grow nor fatal:
    // map_put probes before it tests the grow condition (the W2-prep lane's finding, fixed at
    // the wave merge - it used to take the grow path here, a TL_FATAL on a fixed Map for an
    // operation that adds nothing). Pinned at full load, not one below it.
    map_put(&m, 100u, 56u);
    TL_EXPECT_EQ(*map_get(&m, 100u), (u32)56);
    TL_EXPECT_EQ(map_count(&m), (u32)12);
    TL_EXPECT_EQ(m.cap, (u32)16);
    TL_EXPECT_EQ(arena_mark(&arena), mark_after_init);
}

// The other half. TL_FATAL fires in EVERY tier (unlike TL_ASSERT), so this row has no dev-only
// skip - it runs in a child process on all four, per docs/TESTING.md §9.1.
TL_TEST_EXPECT_FATAL(map_fixed_overflow_is_fatal, "foundation,containers,fatal") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    // TL_ASSERT_EQ, not a bare `if`: a failed setup returns, the child exits 0, and the runner
    // scores this row FAIL rather than letting it pass vacuously (array_fixed_overflow_is_fatal's
    // precedent).
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.map_fixed_of"_id, 4ull * 1024 * 1024, 0, &api), ERR_OK);
    Map<u32, u32> m;
    map_init_fixed(&m, &arena, 16u);
    for (u32 i = 0; i < 12u; ++i) { map_put(&m, i + 1u, i); }   // the full 0.75 load: legal
    map_put(&m, 13u, 13u);   // the insert that would grow: TL_FATAL, never returns
}

// The growing half of the map_put reorder (the 2026-08-25 sweep's D5; the fixed half is pinned at
// full load in map_fixed_serves_its_full_load_factor_without_growing): an overwrite of a present
// key at exactly full load on a GROWING map must not rehash either - before the reorder it did,
// spuriously, moving the arena mark and doubling cap for a put that added no entry.
TL_TEST(map_growing_overwrite_at_full_load_does_not_rehash, "foundation,containers,mem,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, "test.map_grow_ovw"_id, 4ull * 1024 * 1024, 0, &api), ERR_OK);
    Map<u32, u32> m;
    map_init(&m, &arena, 16u);
    for (u32 i = 0; i < 12u; ++i) { map_put(&m, i + 1u, i); }   // exactly 0.75 * 16
    TL_EXPECT_EQ(m.cap, (u32)16);
    const u64 mark = arena_mark(&arena);
    map_put(&m, 5u, 999u);   // overwrite at full load: present key, so no growth and no push
    TL_EXPECT_EQ(*map_get(&m, 5u), (u32)999);
    TL_EXPECT_EQ(map_count(&m), (u32)12);
    TL_EXPECT_EQ(m.cap, (u32)16);
    TL_EXPECT_EQ(arena_mark(&arena), mark);
    map_put(&m, 100u, 1u);   // and the insert that really is new DOES take the grow path
    TL_EXPECT_EQ(m.cap, (u32)32);
    TL_EXPECT_TRUE(arena_mark(&arena) != mark);
    TL_EXPECT_EQ(map_count(&m), (u32)13);
}

// D2 of the 2026-08-25 sweep: exit code 2 + the marker alone cannot say WHICH fatal fired - with
// map_grow's guard deleted, map_fixed_overflow_is_fatal still PASSED in dev, because map_grow
// then dereferences its null grow arena and arena_push's TL_ASSERT produces the same controlled
// exit (measured by mutation; on netcode/ship that assert is compiled out and the row fails on
// the access violation, so the 4-tier PR matrix contains the gap). Until the filed
// TL_TEST_EXPECT_FATAL tightening lands (TODO.md - child-stderr capture in the runner judging
// the marker's origin), this checker pins the message at the site: it relaunches this binary
// filtered to the row and asserts the child's fatal names the fixed-map overflow, not some
// accidental assert on the way there. tests/ carries the io/process exemption of TESTING.md §8 R-2.
TL_TEST(map_fixed_overflow_names_its_fatal, "foundation,containers") {
    const char* log_path = "tl_map_fixed_fatal.log.tmp";
    char cmd[1024];
#ifdef _WIN32
    // cmd.exe's `/c` strips only the first and last quote of a command line that starts with
    // one, so the whole thing needs an extra wrapping pair (the quirk tl_assert.test.cpp and
    // runner_timeout.test.cpp document; /bin/sh -c has no such rule).
    snprintf(cmd, sizeof(cmd), "\"\"%s\" --filter map_fixed_overflow_is_fatal > \"%s\" 2>&1\"",
             TL_TESTS_EXE, log_path);
#else
    snprintf(cmd, sizeof(cmd), "\"%s\" --filter map_fixed_overflow_is_fatal > \"%s\" 2>&1",
             TL_TESTS_EXE, log_path);
#endif
    const int rc = system(cmd);
#ifdef _WIN32
    const int code = rc;   // no wait-status encoding on Windows - the return value IS the exit code
#else
    const int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
    TL_EXPECT_EQ(code, 0);

    FILE* f = fopen(log_path, "rb");
    TL_ASSERT_TRUE(f != nullptr);
    char log[8192];
    const size_t n = fread(log, 1, sizeof(log) - 1, f);
    log[n] = 0;
    fclose(f);
    remove(log_path);
    // The fatal that fired is THE fatal: map_grow's own message, in the TL_FATAL marker line the
    // grandchild printed - not merely "some fatal happened".
    TL_EXPECT_TRUE(strstr(log, "map_grow: fixed map overflow") != nullptr);
    TL_EXPECT_TRUE(strstr(log, "1 passed") != nullptr);
    TL_EXPECT_TRUE(strstr(log, "0 failed") != nullptr);
}
