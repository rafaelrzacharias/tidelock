// arena_registry.h + snapshot.h - snapshot/restore round trips, fingerprint gating, the ring,
// and the docs/MEMORY.md §8.8 done criteria: hash-region integrity, two worlds in one process
// hashing identically, and a mid-run restore reproducing the hash trace.
// Spec: docs/MEMORY.md §8.3, §8.7 test list; docs/DETERMINISM.md §4 (what the hash covers).
// Rubric: docs/TESTING.md §7. No hash VALUE is pinned here - only relative properties, so
// these tests are hash-implementation-agnostic (rapidhash's own vectors live in hash.test.cpp,
// the rng/hash lane).
// Deferred to the runner lane's TL_TEST_EXPECT_FATAL: add-after-seal, duplicate id,
// MAX_ARENAS+1, set_fingerprint before seal, hash_all before seal, netcode/ship overflow fatal.
#include "runner/tl_test.h"
#include "foundation/arena_registry.h"
#include "foundation/snapshot.h"
#include "vmem_test_api.h"

#include <string.h>

namespace {

// One world: three arenas with distinct registry roles, plus a snapshot-ring backing arena.
struct TestWorld {
    VMemApi api;
    VMemArena a;   // HASHED | SNAPSHOT
    VMemArena b;   // SNAPSHOT | GROWS_AT_BARRIER
    VMemArena c;   // HASHED only - restore must NOT touch it
    VMemArena backing;
    ArenaRegistry reg;
};

bool world_init(TestWorld* w) {
    w->api = test_vmem_api();
    w->reg = ArenaRegistry{};
    if (vmem_arena_init(&w->a, 0xAAu, 1u << 20, ARENA_ZERO_ON_PUSH, &w->api) != ERR_OK) { return false; }
    if (vmem_arena_init(&w->b, 0xBBu, 1u << 20, ARENA_ZERO_ON_PUSH, &w->api) != ERR_OK) { return false; }
    if (vmem_arena_init(&w->c, 0xCCu, 1u << 20, ARENA_ZERO_ON_PUSH, &w->api) != ERR_OK) { return false; }
    if (vmem_arena_init(&w->backing, 0xB0u, 64u << 20, 0u, &w->api) != ERR_OK) { return false; }
    registry_add(&w->reg, 0xAAu, &w->a, ARENA_HASHED | ARENA_SNAPSHOT);
    registry_add(&w->reg, 0xBBu, &w->b, ARENA_SNAPSHOT | ARENA_GROWS_AT_BARRIER);
    registry_add(&w->reg, 0xCCu, &w->c, ARENA_HASHED);
    registry_seal(&w->reg);
    return true;
}

void world_release(TestWorld* w) {
    w->api.release(w->api.ctx, w->a.base, w->a.reserved);
    w->api.release(w->api.ctx, w->b.base, w->b.reserved);
    w->api.release(w->api.ctx, w->c.base, w->c.reserved);
    w->api.release(w->api.ctx, w->backing.base, w->backing.reserved);
}

void fill(VMemArena* a, u64 n, u8 seed) {
    u8* p = (u8*)arena_push(a, n, 1u);
    for (u64 i = 0; i < n; ++i) { p[i] = (u8)(seed + (u8)i); }
}

}  // namespace

TL_TEST(registry_snapshot_restore_round_trip, "foundation,mem,smoke") {
    TestWorld w;
    TL_ASSERT_TRUE(world_init(&w));
    fill(&w.a, 100u, 1u);      // odd size: forces a 64-byte-aligned gap before b's segment
    fill(&w.b, 3000u, 7u);
    fill(&w.c, 500u, 11u);

    SnapshotRing ring;
    TL_ASSERT_EQ(ring_init(&ring, 1u << 20, &w.backing), ERR_OK);
    Snapshot* s = ring_push(&ring, 42u);
    TL_ASSERT_TRUE(s != nullptr);
    TL_ASSERT_EQ(registry_snapshot(&w.reg, s, 42u), ERR_OK);
    TL_EXPECT_EQ(s->tick, (u64)42);
    TL_EXPECT_EQ(s->count, 3u);
    TL_EXPECT_EQ(s->used[0], (u64)100);
    TL_EXPECT_EQ(s->used[1], (u64)3000);
    TL_EXPECT_EQ(s->used[2], (u64)500);   // recorded even though c is not snapshotted

    // Keep golden copies, then trash the world: grow a, rewrite b, rewrite c.
    u8 gold_a[100]; memcpy(gold_a, w.a.base, 100u);
    u8 gold_b[3000]; memcpy(gold_b, w.b.base, 3000u);
    fill(&w.a, 5000u, 99u);
    memset(w.a.base, 0xEC, 100u);   // trash the snapshotted range itself, not just the tail
    memset(w.b.base, 0xF0, 3000u);
    memset(w.c.base, 0x0F, 500u);

    TL_ASSERT_EQ(registry_restore(&w.reg, s), ERR_OK);
    TL_EXPECT_EQ(w.a.used, (u64)100);
    TL_EXPECT_EQ(w.b.used, (u64)3000);
    TL_EXPECT_EQ(memcmp(w.a.base, gold_a, 100u), 0);
    TL_EXPECT_EQ(memcmp(w.b.base, gold_b, 3000u), 0);
    // c is not snapshotted: restore must not touch its bytes or extent.
    TL_EXPECT_EQ(w.c.used, (u64)500);
    TL_EXPECT_EQ(w.c.base[0], (u8)0x0F);

    // Restore is repeatable (idempotent from the same snapshot).
    TL_ASSERT_EQ(registry_restore(&w.reg, s), ERR_OK);
    TL_EXPECT_EQ(memcmp(w.a.base, gold_a, 100u), 0);

    world_release(&w);
}

TL_TEST(registry_restore_gates_on_fingerprint_and_count, "foundation,mem,smoke") {
    TestWorld w;
    TL_ASSERT_TRUE(world_init(&w));
    fill(&w.a, 64u, 3u);

    u8 fp1[32]; u8 fp2[32];
    for (u32 i = 0; i < 32u; ++i) { fp1[i] = (u8)i; fp2[i] = (u8)(i + 1u); }
    registry_set_fingerprint(&w.reg, fp1);

    SnapshotRing ring;
    TL_ASSERT_EQ(ring_init(&ring, 1u << 16, &w.backing), ERR_OK);
    Snapshot* s = ring_push(&ring, 7u);
    TL_ASSERT_EQ(registry_snapshot(&w.reg, s, 7u), ERR_OK);

    // Same registry, different session fingerprint: refused, nothing touched.
    registry_set_fingerprint(&w.reg, fp2);
    memset(w.a.base, 0xEE, 64u);
    TL_EXPECT_EQ(registry_restore(&w.reg, s), ERR_SNAPSHOT_MISMATCH);
    TL_EXPECT_EQ(w.a.base[0], (u8)0xEE);   // fail-loud gate: no partial restore
    registry_set_fingerprint(&w.reg, fp1);
    TL_ASSERT_EQ(registry_restore(&w.reg, s), ERR_OK);

    // A registry with a different count (a different world layout): refused.
    TestWorld v;
    TL_ASSERT_TRUE(world_init(&v));
    registry_set_fingerprint(&v.reg, fp1);
    ArenaRegistry two = {};
    registry_add(&two, 0xAAu, &v.a, ARENA_HASHED | ARENA_SNAPSHOT);
    registry_add(&two, 0xBBu, &v.b, ARENA_SNAPSHOT);
    registry_seal(&two);
    registry_set_fingerprint(&two, fp1);
    TL_EXPECT_EQ(registry_restore(&two, s), ERR_SNAPSHOT_MISMATCH);

    world_release(&v);
    world_release(&w);
}

TL_TEST(registry_restore_refuses_wrong_ids_before_app_fingerprint, "foundation,mem,smoke,fast") {
    // W1 mem review 2: before the app stamps the BLAKE2b fingerprint, restore's id gate rests
    // entirely on registry_seal's own id fold (docs/MEMORY.md section 8.3). A same-count
    // registry with one different id, or the same ids in a different order, must be refused -
    // registration order is the lockstep contract.
    TestWorld w;
    TL_ASSERT_TRUE(world_init(&w));
    fill(&w.a, 64u, 3u);

    SnapshotRing ring;
    TL_ASSERT_EQ(ring_init(&ring, 1u << 16, &w.backing), ERR_OK);
    Snapshot* s = ring_push(&ring, 9u);
    TL_ASSERT_EQ(registry_snapshot(&w.reg, s, 9u), ERR_OK);

    // Same count, same shapes, one different id: refused with no app fingerprint ever set.
    TestWorld v;
    TL_ASSERT_TRUE(world_init(&v));
    ArenaRegistry wrong_id = {};
    registry_add(&wrong_id, 0xAAu, &v.a, ARENA_HASHED | ARENA_SNAPSHOT);
    registry_add(&wrong_id, 0xBBu, &v.b, ARENA_SNAPSHOT | ARENA_GROWS_AT_BARRIER);
    registry_add(&wrong_id, 0xDDu, &v.c, ARENA_HASHED);   // 0xDD, not 0xCC
    registry_seal(&wrong_id);
    TL_EXPECT_EQ(registry_restore(&wrong_id, s), ERR_SNAPSHOT_MISMATCH);

    // Same ids, different ORDER: also refused (the fold is order-sensitive by design).
    ArenaRegistry wrong_order = {};
    registry_add(&wrong_order, 0xBBu, &v.b, ARENA_SNAPSHOT | ARENA_GROWS_AT_BARRIER);
    registry_add(&wrong_order, 0xAAu, &v.a, ARENA_HASHED | ARENA_SNAPSHOT);
    registry_add(&wrong_order, 0xCCu, &v.c, ARENA_HASHED);
    registry_seal(&wrong_order);
    TL_EXPECT_EQ(registry_restore(&wrong_order, s), ERR_SNAPSHOT_MISMATCH);

    // The matching registry (v's own, same ids in the same order) is accepted.
    TL_EXPECT_EQ(registry_restore(&v.reg, s), ERR_OK);

    world_release(&v);
    world_release(&w);
}

TL_TEST(registry_restore_into_fresh_world_commits, "foundation,mem,smoke") {
    // Late-join/resync shape (docs/DETERMINISM.md §5): the target arenas have never committed
    // a page; restore must commit on demand and land byte-identical extents.
    TestWorld w;
    TL_ASSERT_TRUE(world_init(&w));
    fill(&w.a, (u64)COMMIT_GRANULE + 100u, 5u);   // crosses a commit granule
    fill(&w.b, 10u, 9u);

    SnapshotRing ring;
    TL_ASSERT_EQ(ring_init(&ring, 4u << 20, &w.backing), ERR_OK);
    Snapshot* s = ring_push(&ring, 1u);
    TL_ASSERT_EQ(registry_snapshot(&w.reg, s, 1u), ERR_OK);

    TestWorld f;
    TL_ASSERT_TRUE(world_init(&f));
    TL_EXPECT_EQ(f.a.committed, (u64)0);
    TL_ASSERT_EQ(registry_restore(&f.reg, s), ERR_OK);
    TL_EXPECT_EQ(f.a.used, (u64)COMMIT_GRANULE + 100u);
    TL_EXPECT_GE(f.a.committed, f.a.used);
    TL_EXPECT_EQ(memcmp(f.a.base, w.a.base, (usize)COMMIT_GRANULE + 100u), 0);
    TL_EXPECT_EQ(memcmp(f.b.base, w.b.base, 10u), 0);

    world_release(&f);
    world_release(&w);
}

TL_TEST(ring_push_find_wrap_and_eviction, "foundation,mem,smoke,fast") {
    TestWorld w;
    TL_ASSERT_TRUE(world_init(&w));
    fill(&w.a, 16u, 1u);

    SnapshotRing ring;
    TL_ASSERT_EQ(ring_init(&ring, 1u << 16, &w.backing), ERR_OK);
    TL_EXPECT_EQ(ring_init(nullptr, 1u, &w.backing), ERR_MEM_BAD_ARG);
    TL_EXPECT_EQ(ring_init(&ring, 0u, &w.backing), ERR_MEM_BAD_ARG);

    // Empty ring: nothing to find.
    TL_EXPECT_NULL(ring_find(&ring, 0u));
    TL_EXPECT_NULL(ring_find(&ring, 100u));

    // Push CONFIRMATION_HORIZON_TICKS + 2: the oldest two are evicted by wrap. (`tk`, not `t` -
    // the runner's TestCtx parameter is named t and the macros reference it.)
    for (u64 tk = 100u; tk < 100u + (u64)CONFIRMATION_HORIZON_TICKS + 2u; ++tk) {
        Snapshot* s = ring_push(&ring, tk);
        TL_ASSERT_TRUE(s != nullptr);
        TL_ASSERT_EQ(registry_snapshot(&w.reg, s, tk), ERR_OK);
    }
    TL_EXPECT_NULL(ring_find(&ring, 100u));
    TL_EXPECT_NULL(ring_find(&ring, 101u));
    for (u64 tk = 102u; tk < 100u + (u64)CONFIRMATION_HORIZON_TICKS + 2u; ++tk) {
        const Snapshot* s = ring_find(&ring, tk);
        TL_ASSERT_TRUE(s != nullptr);
        TL_EXPECT_EQ(s->tick, tk);
    }
    TL_EXPECT_EQ(ring.count, (u32)CONFIRMATION_HORIZON_TICKS);

    world_release(&w);
}

TL_TEST(ring_overflow_is_a_budget_violation, "foundation,mem,fast") {
#if TL_DEV
    // Dev tier: the overflow comes back as a named error and the slot is invalidated, so a
    // stale blob can never be found under the new tick (docs/MEMORY.md §7 R-2). The
    // netcode/ship half (TL_FATAL) is a deferred fatal-expected test.
    TestWorld w;
    TL_ASSERT_TRUE(world_init(&w));
    fill(&w.a, 10000u, 1u);

    SnapshotRing ring;
    TL_ASSERT_EQ(ring_init(&ring, 128u, &w.backing), ERR_OK);   // 128 B cap vs 10 KB of state
    Snapshot* s = ring_push(&ring, 55u);
    TL_EXPECT_EQ(registry_snapshot(&w.reg, s, 55u), ERR_MEM_RING_OVERFLOW);
    TL_EXPECT_NULL(ring_find(&ring, 55u));

    // The ring recovers: a fitting world snapshots fine into the next slot.
    arena_reset_to(&w.a, 32u);
    Snapshot* s2 = ring_push(&ring, 56u);
    TL_EXPECT_EQ(registry_snapshot(&w.reg, s2, 56u), ERR_OK);
    TL_EXPECT_TRUE(ring_find(&ring, 56u) == s2);

    world_release(&w);
#else
    TL_EXPECT_TRUE(true);   // netcode/ship: overflow is TL_FATAL by contract
#endif
}

// --- the docs/MEMORY.md §8.8 done criteria -----------------------------------------------------

TL_TEST(registry_hash_region_integrity, "foundation,mem,determinism,smoke") {
    // docs/DETERMINISM.md §4: mutating a transient byte must NOT move the hash; mutating any
    // authoritative byte MUST. Automated over every registered arena.
    TestWorld w;
    TL_ASSERT_TRUE(world_init(&w));
    fill(&w.a, 200u, 3u);
    fill(&w.b, 100u, 5u);
    fill(&w.c, 50u, 7u);

    u64 pa0[MAX_ARENAS]; u64 pa1[MAX_ARENAS];
    const u64 h0 = registry_hash_all(&w.reg, pa0);
    TL_EXPECT_EQ(registry_hash_all(&w.reg, pa1), h0);   // pure function of state
    TL_EXPECT_EQ(pa0[1], (u64)0);                        // b is not HASHED: folded as 0

    // Flip one byte INSIDE used of each HASHED arena: the world hash moves and the per-arena
    // array pinpoints exactly that arena (the desync bisection property, NETCODE §14).
    VMemArena* hashed[2] = { &w.a, &w.c };
    const u32 hashed_idx[2] = { 0u, 2u };
    for (u32 k = 0; k < 2u; ++k) {
        hashed[k]->base[7] = (u8)(hashed[k]->base[7] ^ 0xFFu);
        const u64 h = registry_hash_all(&w.reg, pa1);
        TL_EXPECT_NE(h, h0);
        TL_EXPECT_NE(pa1[hashed_idx[k]], pa0[hashed_idx[k]]);
        TL_EXPECT_EQ(pa1[hashed_idx[1u - k]], pa0[hashed_idx[1u - k]]);   // the OTHER one did not move
        hashed[k]->base[7] = (u8)(hashed[k]->base[7] ^ 0xFFu);
        TL_EXPECT_EQ(registry_hash_all(&w.reg, pa1), h0);                 // flip back restores
    }

    // A registered-but-not-HASHED arena is transient to the hash.
    w.b.base[0] = (u8)(w.b.base[0] ^ 0xFFu);
    TL_EXPECT_EQ(registry_hash_all(&w.reg, pa1), h0);
    w.b.base[0] = (u8)(w.b.base[0] ^ 0xFFu);

    // A byte ABOVE used (committed, dirty) is not state: flipping it must not move the hash.
    w.a.base[w.a.used + 10u] = 0x5Au;
    TL_EXPECT_EQ(registry_hash_all(&w.reg, pa1), h0);

    // The extent itself IS state: growing by one ZERO byte must move the hash.
    (void)arena_push(&w.a, 1u, 1u);
    TL_EXPECT_NE(registry_hash_all(&w.reg, pa1), h0);

    world_release(&w);
}

TL_TEST(registry_two_worlds_hash_identically, "foundation,mem,determinism,smoke") {
    // docs/MEMORY.md §8.8: two worlds' registries in one process hash identically. The two
    // worlds take DIFFERENT dirt histories (different pre-reset fills), then perform identical
    // logical operations - zero-on-push must make the hashed bytes converge, including the
    // alignment gap (docs/CPP-SUBSET.md §5: hashed memory is a pure function of state).
    TestWorld w1, w2;
    TL_ASSERT_TRUE(world_init(&w1));
    TL_ASSERT_TRUE(world_init(&w2));
    fill(&w1.a, 300u, 9u);   // divergent history...
    fill(&w2.a, 200u, 4u);
    arena_reset_to(&w1.a, 0u);
    arena_reset_to(&w2.a, 0u);

    TestWorld* worlds[2] = { &w1, &w2 };
    for (u32 i = 0; i < 2u; ++i) {
        TestWorld* w = worlds[i];
        u8* p = (u8*)arena_push(&w->a, 5u, 1u);
        memset(p, 0x5A, 5u);
        u8* q = (u8*)arena_push(&w->a, 100u, 64u);   // 64-aligned: [5,64) is a re-zeroed gap
        for (u32 j = 0; j < 100u; ++j) { q[j] = (u8)(j * 3u); }
        fill(&w->c, 40u, 11u);
    }

    u64 pa_1[MAX_ARENAS]; u64 pa_2[MAX_ARENAS];
    const u64 h1 = registry_hash_all(&w1.reg, pa_1);
    const u64 h2 = registry_hash_all(&w2.reg, pa_2);
    TL_EXPECT_EQ(h1, h2);
    for (u32 i = 0; i < 3u; ++i) { TL_EXPECT_EQ(pa_1[i], pa_2[i]); }

    // Diverge one authoritative byte: the worlds separate and the per-arena array names arena 2.
    w2.c.base[3] = (u8)(w2.c.base[3] ^ 1u);
    const u64 h2b = registry_hash_all(&w2.reg, pa_2);
    TL_EXPECT_NE(h2b, h1);
    TL_EXPECT_EQ(pa_1[0], pa_2[0]);
    TL_EXPECT_NE(pa_1[2], pa_2[2]);

    world_release(&w1);
    world_release(&w2);
}

namespace {
// A deterministic synthetic tick: mutate the HASHED|SNAPSHOT arena as a pure function of
// (state, tick) - growth AND in-place. (c is HASHED-but-not-SNAPSHOT by this fixture's design,
// so a rollback cannot reproduce a trace that mutates it: hashed authoritative state must also
// be snapshotted, which is the real registry wiring.)
void sim_step(TestWorld* w, u64 tick) {
    u8* p = (u8*)arena_push(&w->a, 16u, 16u);
    for (u32 i = 0; i < 16u; ++i) { p[i] = (u8)(tick * 31u + i); }
    w->a.base[tick % 8u] = (u8)(w->a.base[tick % 8u] ^ (u8)(tick * 7u + 1u));
}
}  // namespace

TL_TEST(registry_restore_reproduces_hash_trace, "foundation,mem,determinism,smoke") {
    // docs/MEMORY.md §8.8: a snapshot restore mid-run reproduces the original hash trace from
    // that tick onward - the rollback contract netcode rides on (docs/DETERMINISM.md §5).
    TestWorld w;
    TL_ASSERT_TRUE(world_init(&w));
    fill(&w.c, 32u, 1u);

    SnapshotRing ring;
    TL_ASSERT_EQ(ring_init(&ring, 1u << 20, &w.backing), ERR_OK);

    u64 trace[10];
    u64 pa[MAX_ARENAS];
    Snapshot* snap = nullptr;
    for (u64 tk = 0u; tk < 10u; ++tk) {
        sim_step(&w, tk);
        trace[tk] = registry_hash_all(&w.reg, pa);
        if (tk == 5u) {
            snap = ring_push(&ring, tk);
            TL_ASSERT_EQ(registry_snapshot(&w.reg, snap, tk), ERR_OK);
        }
    }
    // Roll back to tick 5 and replay 6..9: the trace must be bit-identical.
    TL_ASSERT_TRUE(snap != nullptr);
    TL_ASSERT_EQ(registry_restore(&w.reg, snap), ERR_OK);
    TL_EXPECT_EQ(registry_hash_all(&w.reg, pa), trace[5]);
    for (u64 tk = 6u; tk < 10u; ++tk) {
        sim_step(&w, tk);
        TL_EXPECT_EQ(registry_hash_all(&w.reg, pa), trace[tk]);
    }

    world_release(&w);
}
