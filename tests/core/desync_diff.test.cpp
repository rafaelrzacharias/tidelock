// desync_diff.test.cpp - fingerprint short-circuit, DIFF_USED (no byte compare), DIFF_BYTES
// (first differing offset), max_n truncation, identical snapshots -> zero.
// Spec: docs/TOOLING.md §9.3.8. Rubric: docs/TESTING.md §7.
//
// TL_TEST's generated signature is `(TestCtx* t)` - every WorldFixture local here is `f`, every
// pair of Snapshot locals is `snap_a`/`snap_b`.
#include "core/world_test_util.h"
#include "core/desync_diff.h"

#include <string.h>

namespace {

// A standalone snapshot pair, backed by its own blob arena - no SnapshotRing needed (this file
// never restores, only diffs). Mirrors ring_init's own blob_push shape (foundation/snapshot.cpp)
// without the ring's 6-slot allocation this test does not need.
struct SnapPair {
    VMemArena blob_arena;
    Snapshot a, b;
};

bool snap_pair_init(SnapPair* sp, const VMemApi* api) {
    memset(sp, 0, sizeof(*sp));
    if (vmem_arena_init(&sp->blob_arena, "dd.blob"_id, 4u * 1024u * 1024u, 0u, api) != ERR_OK) { return false; }
    sp->a.blob = (u8*)arena_push(&sp->blob_arena, 2u * 1024u * 1024u, 64u);
    sp->a.blob_cap = 2u * 1024u * 1024u;
    sp->b.blob = (u8*)arena_push(&sp->blob_arena, 2u * 1024u * 1024u, 64u);
    sp->b.blob_cap = 2u * 1024u * 1024u;
    return sp->a.blob != nullptr && sp->b.blob != nullptr;
}

struct Recorded {
    DesyncEntry rows[16];
    u32 count;
};

void record(void* ctx, const DesyncEntry* e) {
    Recorded* r = (Recorded*)ctx;
    if (r->count < 16u) { r->rows[r->count] = *e; }
    r->count += 1u;
}

// The first registered arena whose live `used` is nonzero - a real byte to poke directly,
// without depending on which named arena world_init happens to register first (the arena set a
// zero-component World registers, docs/core/world.cpp's world_init, is itself not this file's
// contract to pin down).
i32 find_poke_index(const ArenaRegistry* reg) {
    for (u32 i = 0; i < reg->count; ++i) {
        if (reg->e[i].arena->used > 0u) { return (i32)i; }
    }
    return -1;
}

}  // namespace

TL_TEST(desync_diff_fingerprint_mismatch_short_circuits, "core,desync_diff,fast") {
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 1u));
    world_build_schedule(&f.w);
    registry_seal(&f.reg);

    VMemApi api = test_vmem_api();
    SnapPair sp;
    TL_ASSERT_TRUE(snap_pair_init(&sp, &api));
    TL_ASSERT_EQ(registry_snapshot(&f.reg, &sp.a, 0u), ERR_OK);
    TL_ASSERT_EQ(registry_snapshot(&f.reg, &sp.b, 1u), ERR_OK);
    sp.b.session_fingerprint[0] ^= 0xFFu;   // corrupt identity, not content

    Recorded rec{};
    const u32 n = desync_diff(&f.reg, &sp.a, &sp.b, 10u, record, &rec);
    TL_ASSERT_EQ(n, 1u);
    TL_ASSERT_EQ(rec.count, 1u);
    TL_EXPECT_EQ((u32)rec.rows[0].kind, (u32)DIFF_FINGERPRINT_MISMATCH);
}

TL_TEST(desync_diff_identical_snapshots_report_nothing, "core,desync_diff,fast") {
    WorldFixture& f = *wt_fixture(1u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 2u));
    world_build_schedule(&f.w);
    registry_seal(&f.reg);
    world_spawn(&f.w);
    world_flush(&f.w);

    VMemApi api = test_vmem_api();
    SnapPair sp;
    TL_ASSERT_TRUE(snap_pair_init(&sp, &api));
    TL_ASSERT_EQ(registry_snapshot(&f.reg, &sp.a, 5u), ERR_OK);
    TL_ASSERT_EQ(registry_snapshot(&f.reg, &sp.b, 6u), ERR_OK);   // same live state, different tick

    Recorded rec{};
    const u32 n = desync_diff(&f.reg, &sp.a, &sp.b, 10u, record, &rec);
    TL_EXPECT_EQ(n, 0u);
    TL_EXPECT_EQ(rec.count, 0u);
}

TL_TEST(desync_diff_used_mismatch_skips_byte_compare, "core,desync_diff,fast") {
    WorldFixture& f = *wt_fixture(2u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 3u));
    world_build_schedule(&f.w);
    registry_seal(&f.reg);

    VMemApi api = test_vmem_api();
    SnapPair sp;
    TL_ASSERT_TRUE(snap_pair_init(&sp, &api));
    TL_ASSERT_EQ(registry_snapshot(&f.reg, &sp.a, 0u), ERR_OK);   // no entities yet

    world_spawn(&f.w);
    world_flush(&f.w);   // grows world.entities.* arenas' used[]
    TL_ASSERT_EQ(registry_snapshot(&f.reg, &sp.b, 1u), ERR_OK);

    Recorded rec{};
    const u32 n = desync_diff(&f.reg, &sp.a, &sp.b, 10u, record, &rec);
    TL_ASSERT_TRUE(n >= 1u);
    // world.entities.live is a fixed-capacity bitset (used[] constant, content changes as slots
    // go live) - a real DIFF_BYTES alongside the DIFF_USED arenas is expected here, not a bug;
    // the contract this test actually checks is per-arena: an arena reported DIFF_USED never
    // ALSO gets a DIFF_BYTES row for the SAME arena_index (continue-after-report, this header's
    // own Purpose note), not that every other arena is untouched.
    bool found_used = false;
    for (u32 i = 0; i < rec.count; ++i) {
        if (rec.rows[i].kind != DIFF_USED) { continue; }
        found_used = true;
        TL_EXPECT_TRUE(rec.rows[i].used_a < rec.rows[i].used_b);   // grew, a < b
        for (u32 j = 0; j < rec.count; ++j) {
            if (j == i) { continue; }
            TL_EXPECT_TRUE(!(rec.rows[j].kind == DIFF_BYTES && rec.rows[j].arena_index == rec.rows[i].arena_index));
        }
    }
    TL_EXPECT_TRUE(found_used);
}

TL_TEST(desync_diff_byte_mismatch_reports_first_differing_offset, "core,desync_diff,fast") {
    WorldFixture& f = *wt_fixture(3u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 4u));
    world_build_schedule(&f.w);
    registry_seal(&f.reg);
    world_spawn(&f.w);
    world_flush(&f.w);   // real bytes to diff, without growing anything between snapshots

    VMemApi api = test_vmem_api();
    SnapPair sp;
    TL_ASSERT_TRUE(snap_pair_init(&sp, &api));
    TL_ASSERT_EQ(registry_snapshot(&f.reg, &sp.a, 0u), ERR_OK);

    const i32 idx = find_poke_index(&f.reg);
    TL_ASSERT_TRUE(idx >= 0);
    VMemArena* poked = f.reg.e[(u32)idx].arena;
    poked->base[0] = (u8)(poked->base[0] ^ 0xFFu);   // content changes, used[] does not

    TL_ASSERT_EQ(registry_snapshot(&f.reg, &sp.b, 1u), ERR_OK);
    poked->base[0] = (u8)(poked->base[0] ^ 0xFFu);   // restore live state - this fixture is reused by slot

    Recorded rec{};
    const u32 n = desync_diff(&f.reg, &sp.a, &sp.b, 10u, record, &rec);
    TL_ASSERT_EQ(n, 1u);
    TL_ASSERT_EQ((u32)rec.rows[0].kind, (u32)DIFF_BYTES);
    TL_EXPECT_EQ(rec.rows[0].arena_index, (u32)idx);
    TL_EXPECT_EQ(rec.rows[0].byte_offset, (u64)0u);
    TL_EXPECT_TRUE(rec.rows[0].bytes_a[0] != rec.rows[0].bytes_b[0]);
}

TL_TEST(desync_diff_max_n_stops_early, "core,desync_diff,fast") {
    WorldFixture& f = *wt_fixture(0u);   // slot reuse across tests: safe, see dotpath.test.cpp's note
    TL_ASSERT_TRUE(world_fixture_init(&f, 5u));
    world_build_schedule(&f.w);
    registry_seal(&f.reg);

    VMemApi api = test_vmem_api();
    SnapPair sp;
    TL_ASSERT_TRUE(snap_pair_init(&sp, &api));
    TL_ASSERT_EQ(registry_snapshot(&f.reg, &sp.a, 0u), ERR_OK);
    world_spawn(&f.w);
    world_spawn(&f.w);
    world_flush(&f.w);   // grows several arenas at once -> several DIFF_USED candidates
    TL_ASSERT_EQ(registry_snapshot(&f.reg, &sp.b, 1u), ERR_OK);

    Recorded rec{};
    const u32 n = desync_diff(&f.reg, &sp.a, &sp.b, 1u, record, &rec);
    TL_EXPECT_EQ(n, 1u);
    TL_EXPECT_EQ(rec.count, 1u);
}
