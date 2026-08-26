// net_internal.h - the per-slot frame rings and the LogRecord store (docs/NETCODE.md §20.1,
// §20.3(a), §20.2.3). Phase 1's half only.
//
// §20.1 scopes net_internal.h to "net/*.cpp". That rule exists so nothing ABOVE net sees net's
// private state; a module's own tests are not a consumer in that sense, and testing this header
// through a seam invented for the purpose would be testing the seam. Included directly, as
// tests/foundation does with its own modules' internals.
#include "runner/tl_test.h"
#include "net/net_internal.h"
#include "net/net_test_util.h"

TL_TEST(net_slot_ring_places_and_reads_by_absolute_tick, "net,internal,smoke,fast") {
    SlotRing r;
    slot_ring_clear(&r);
    for (u64 tick = 0; tick < SLOT_RING_TICKS; ++tick) { TL_ASSERT_FALSE(slot_ring_has(&r, tick)); }

    const WireFrame f = nt_digital_frame(7u, 0x21u, 1234, -5678);
    slot_ring_put(&r, 1000u, &f);
    TL_ASSERT_TRUE(slot_ring_has(&r, 1000u));
    const WireFrame* got = slot_ring_get(&r, 1000u);
    TL_ASSERT_NOT_NULL(got);
    TL_EXPECT_TRUE(nt_frames_equal_payload(&f, got));
    // Neighbouring ticks are not occupied by it.
    TL_EXPECT_FALSE(slot_ring_has(&r, 999u));
    TL_EXPECT_FALSE(slot_ring_has(&r, 1001u));
    TL_EXPECT_NULL(slot_ring_get(&r, 1001u));
}

TL_TEST(net_slot_ring_wrapped_entries_read_as_absent_not_as_the_newer_frame, "net,internal,edge,fast") {
    // The failure this design exists to prevent: index-only rings hand back the frame that has
    // since taken the slot. Storing the tick beside the frame makes a wrapped tick ABSENT.
    SlotRing r;
    slot_ring_clear(&r);
    const WireFrame old_f = nt_digital_frame(0u, 0x1u, 11, 22);
    const WireFrame new_f = nt_digital_frame(0u, 0x2u, 33, 44);
    slot_ring_put(&r, 5u, &old_f);
    TL_ASSERT_TRUE(slot_ring_has(&r, 5u));

    // One full lap later: the same index, a different tick.
    const u64 wrapped = 5u + SLOT_RING_TICKS;
    TL_ASSERT_EQ(slot_ring_index(wrapped), slot_ring_index(5u));
    slot_ring_put(&r, wrapped, &new_f);
    TL_EXPECT_TRUE(slot_ring_has(&r, wrapped));
    TL_EXPECT_FALSE(slot_ring_has(&r, 5u));          // gone, not stale
    TL_EXPECT_NULL(slot_ring_get(&r, 5u));
    TL_EXPECT_TRUE(nt_frames_equal_payload(&new_f, slot_ring_get(&r, wrapped)));
}

TL_TEST(net_slot_ring_holds_a_full_redundancy_window, "net,internal,fast") {
    // The ring must cover the redundancy window plus the confirmation horizon (docs/NETCODE.md
    // §20; static_asserted in wire.h). Fill it end to end and read every tick back.
    SlotRing r;
    slot_ring_clear(&r);
    const u64 base = 1u << 20;
    for (u32 i = 0; i < SLOT_RING_TICKS; ++i) {
        const WireFrame f = nt_digital_frame((u32)(base + i), 1u << (i % 32u), (i32)i, -(i32)i);
        slot_ring_put(&r, base + i, &f);
    }
    for (u32 i = 0; i < SLOT_RING_TICKS; ++i) {
        TL_ASSERT_TRUE(slot_ring_has(&r, base + i));
        const WireFrame want = nt_digital_frame((u32)(base + i), 1u << (i % 32u), (i32)i, -(i32)i);
        TL_ASSERT_TRUE(nt_frames_equal_payload(&want, slot_ring_get(&r, base + i)));
    }
    TL_EXPECT_GE((u32)SLOT_RING_TICKS, REDUNDANCY_TICKS + CONFIRMATION_HORIZON_TICKS + 6u);
}

TL_TEST(net_log_store_suppresses_duplicate_ids, "net,internal,fast") {
    // R6 (docs/NETCODE.md §20.2.3): the stable id is (origin_slot, seq) and duplicates are
    // NO-OPS, which is what lets a record be announced in `pending` and arrive again in its
    // tick's SeqSection.
    LogStore s;
    log_store_clear(&s);
    LogRecord a = {};
    a.format_version = NET_FORMAT_VERSION;
    a.kind = (u8)LR_DELAY;
    a.slot = 1u;
    a.origin_slot = 2u;
    a.seq = 9u;
    a.payload = 5u;
    a.effective_tick = 400u;

    TL_EXPECT_EQ(log_store_add(&s, &a), ERR_OK);
    TL_EXPECT_EQ(s.count, 1u);
    TL_EXPECT_TRUE(log_store_has(&s, 2u, 9u));
    // The same id again - even with a different payload - is a no-op, not a second entry.
    LogRecord dup = a;
    dup.payload = 99u;
    dup.effective_tick = 999u;
    TL_EXPECT_EQ(log_store_add(&s, &dup), ERR_NET_DUPLICATE_RECORD);
    TL_EXPECT_EQ(s.count, 1u);
    TL_EXPECT_EQ(s.records[0].payload, 5u);          // the first one stands

    // A different seq from the same origin, and the same seq from a different origin, are both
    // distinct records - the id is the PAIR.
    LogRecord b = a; b.seq = 10u;
    LogRecord c = a; c.origin_slot = 3u;
    TL_EXPECT_EQ(log_store_add(&s, &b), ERR_OK);
    TL_EXPECT_EQ(log_store_add(&s, &c), ERR_OK);
    TL_EXPECT_EQ(s.count, 3u);
}

TL_TEST(net_log_store_distinguishes_duplicate_from_full, "net,internal,edge,fast") {
    // The two failures are different things and must not share a return value: a duplicate is an
    // expected R6 no-op, a full store is DATA LOSS. A bool conflated them.
    {
        LogStore d;
        log_store_clear(&d);
        LogRecord r = {};
        r.format_version = NET_FORMAT_VERSION;
        r.kind = (u8)LR_JOIN;
        r.origin_slot = 4u;
        r.seq = 1u;
        r.effective_tick = 2u;
        TL_ASSERT_EQ(log_store_add(&d, &r), ERR_OK);
        TL_EXPECT_EQ(log_store_add(&d, &r), ERR_NET_DUPLICATE_RECORD);
        TL_EXPECT_NE(log_store_add(&d, &r), ERR_NET_STORE_FULL);
    }
    LogStore s;
    log_store_clear(&s);
    LogRecord r = {};
    r.format_version = NET_FORMAT_VERSION;
    r.kind = (u8)LR_JOIN;
    r.origin_slot = 0u;
    r.effective_tick = 1u;
    for (u32 i = 0; i < LOG_STORE_CAPACITY; ++i) {
        r.seq = i;
        TL_ASSERT_EQ(log_store_add(&s, &r), ERR_OK);
    }
    TL_EXPECT_EQ(s.count, LOG_STORE_CAPACITY);
    r.seq = LOG_STORE_CAPACITY;
    TL_EXPECT_EQ(log_store_add(&s, &r), ERR_NET_STORE_FULL);   // data loss, named as such
    TL_EXPECT_EQ(s.count, LOG_STORE_CAPACITY);
}

TL_TEST(net_log_store_reads_a_tick_in_origin_then_seq_order, "net,internal,fast") {
    // §20.2.2 requires a SeqSection's records ascending by (origin_slot, seq), and §20.2.9 the
    // same of a segment. Added deliberately out of order, including records at other ticks.
    LogStore s;
    log_store_clear(&s);
    struct Add { u8 origin; u32 seq; u64 tick; };
    const Add adds[] = {
        { 3u, 1u, 500u }, { 1u, 7u, 500u }, { 0u, 2u, 501u },
        { 1u, 2u, 500u }, { 3u, 0u, 500u }, { 2u, 9u, 499u },
    };
    for (u32 i = 0; i < tl_count(adds); ++i) {
        LogRecord r = {};
        r.format_version = NET_FORMAT_VERSION;
        r.kind = (u8)LR_SUSPECT;
        r.origin_slot = adds[i].origin;
        r.seq = adds[i].seq;
        r.effective_tick = adds[i].tick;
        TL_ASSERT_EQ(log_store_add(&s, &r), ERR_OK);
    }

    LogRecord out[8] = {};
    const u32 n = log_store_at_tick(&s, 500u, out, 8u);
    TL_ASSERT_EQ(n, 4u);
    // (1,2) (1,7) (3,0) (3,1) - ascending by origin, then by seq inside an origin.
    TL_EXPECT_EQ(out[0].origin_slot, (u8)1u); TL_EXPECT_EQ(out[0].seq, 2u);
    TL_EXPECT_EQ(out[1].origin_slot, (u8)1u); TL_EXPECT_EQ(out[1].seq, 7u);
    TL_EXPECT_EQ(out[2].origin_slot, (u8)3u); TL_EXPECT_EQ(out[2].seq, 0u);
    TL_EXPECT_EQ(out[3].origin_slot, (u8)3u); TL_EXPECT_EQ(out[3].seq, 1u);
    // Records at other ticks are not returned, and a tick with none returns none.
    TL_EXPECT_EQ(log_store_at_tick(&s, 501u, out, 8u), 1u);
    TL_EXPECT_EQ(log_store_at_tick(&s, 12345u, out, 8u), 0u);
    // A cap smaller than the count truncates without disturbing the order.
    TL_EXPECT_EQ(log_store_at_tick(&s, 500u, out, 2u), 2u);
    TL_EXPECT_EQ(out[0].origin_slot, (u8)1u);
    TL_EXPECT_EQ(out[1].seq, 7u);
    // Reading does not disturb the store's own order.
    TL_EXPECT_EQ(s.records[0].origin_slot, (u8)3u);
    TL_EXPECT_EQ(s.records[0].seq, 1u);
}

TL_TEST(net_log_store_ordered_read_feeds_the_archive_unchanged, "net,internal,fast") {
    // The ordering contract archive_encode_segment asserts is exactly what log_store_at_tick
    // produces, so the two cannot drift apart: sorted by (effective_tick, origin_slot, seq).
    LogStore s;
    log_store_clear(&s);
    for (u32 i = 0; i < 6u; ++i) {
        LogRecord r = {};
        r.format_version = NET_FORMAT_VERSION;
        r.kind = (u8)LR_DELAY;
        r.origin_slot = (u8)((5u - i) % 3u);
        r.seq = i;
        r.payload = i;
        r.effective_tick = 800u;
        TL_ASSERT_EQ(log_store_add(&s, &r), ERR_OK);
    }
    LogRecord out[8] = {};
    const u32 n = log_store_at_tick(&s, 800u, out, 8u);
    TL_ASSERT_EQ(n, 6u);
    for (u32 i = 1; i < n; ++i) {
        const bool ordered = (out[i - 1].origin_slot < out[i].origin_slot)
                          || (out[i - 1].origin_slot == out[i].origin_slot
                              && out[i - 1].seq <= out[i].seq);
        TL_ASSERT_TRUE(ordered);
    }
}
