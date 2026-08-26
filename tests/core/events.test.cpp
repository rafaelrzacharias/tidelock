// events.h - the two-half double buffer: emit in N visible in N+1 only, cleared in N+2,
// emission order preserved, per-type independence, rollback clear, overflow fatal.
// Spec: docs/ECS.md §5, §10.4, §10.8 (events.test.cpp line). Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "core/events.h"
#include "foundation/vmem_test_api.h"

#define TL_FIELDS_EvHit(X, XA, XH) \
    X(i32, damage) XH(Entity, target)
TL_COMPONENT(EvHit)

#define TL_FIELDS_EvPing(X, XA, XH) \
    X(u64, stamp)
TL_COMPONENT(EvPing)

namespace {

struct EvFixture { VMemApi api; EventTables ev; };

bool ev_fixture_init(EvFixture* f, u64 reserve) {
    f->api = test_vmem_api();
    return events_init(&f->ev, "ev.half0"_id, "ev.half1"_id, reserve, &f->api) == ERR_OK;
}

}  // namespace

TL_TEST(events_emit_in_n_visible_in_n1_cleared_in_n2, "core,ecs,events,smoke,fast") {
    EvFixture f;
    TL_ASSERT_TRUE(ev_fixture_init(&f, 1u * 1024u * 1024u));
    const u32 hit = events_register(&f.ev, &EvHit_info, 8u);
    TL_ASSERT_EQ(events_find(&f.ev, "EvHit"_id), hit);
    TL_EXPECT_EQ(events_find(&f.ev, "EvGhost"_id), MAX_EVENT_TYPES);

    // Tick N: two emissions; the read side (last tick) is empty all tick.
    EvHit a = { 5, handle_make<Entity>(1u, 1u) };
    EvHit b = { 9, handle_make<Entity>(2u, 1u) };
    events_emit(&f.ev, hit, &a);
    events_emit(&f.ev, hit, &b);
    TL_EXPECT_EQ(events_read(&f.ev, hit).count, 0u);

    // N -> N+1 barrier: both visible, in emission order, immutable view.
    events_swap(&f.ev);
    EventSlice s = events_read(&f.ev, hit);
    TL_ASSERT_EQ(s.count, 2u);
    TL_ASSERT_EQ(s.stride, (u32)sizeof(EvHit));
    const EvHit* rows = (const EvHit*)(const void*)s.data;
    TL_EXPECT_EQ(rows[0].damage, 5);
    TL_EXPECT_EQ(rows[1].damage, 9);
    TL_EXPECT_EQ(rows[1].target.bits, handle_make<Entity>(2u, 1u).bits);

    // Tick N+1 emits one more; the read view stays tick N's regardless.
    EvHit c = { 13, handle_make<Entity>(3u, 1u) };
    events_emit(&f.ev, hit, &c);
    TL_EXPECT_EQ(events_read(&f.ev, hit).count, 2u);

    // N+1 -> N+2: only c; N's events are gone (cleared write side reused).
    events_swap(&f.ev);
    s = events_read(&f.ev, hit);
    TL_ASSERT_EQ(s.count, 1u);
    TL_EXPECT_EQ(((const EvHit*)(const void*)s.data)[0].damage, 13);

    // N+2 -> N+3 with no emissions: empty.
    events_swap(&f.ev);
    TL_EXPECT_EQ(events_read(&f.ev, hit).count, 0u);
}

TL_TEST(events_types_are_independent_and_order_preserved, "core,ecs,events,fast") {
    EvFixture f;
    TL_ASSERT_TRUE(ev_fixture_init(&f, 1u * 1024u * 1024u));
    const u32 hit = events_register(&f.ev, &EvHit_info, 0u);    // 0 = EVENT_DEFAULT_CAP
    const u32 ping = events_register(&f.ev, &EvPing_info, 4u);
    TL_EXPECT_EQ(f.ev.t[hit].cap, (u32)EVENT_DEFAULT_CAP);

    // Interleaved emission across types keeps per-type emission order.
    for (u32 i = 0; i < 3u; ++i) {
        EvHit h = { (i32)i, handle_make<Entity>(i + 1u, 1u) };
        EvPing p = { 100u + i };
        events_emit(&f.ev, hit, &h);
        events_emit(&f.ev, ping, &p);
    }
    events_swap(&f.ev);
    EventSlice sh = events_read(&f.ev, hit);
    EventSlice sp = events_read(&f.ev, ping);
    TL_ASSERT_EQ(sh.count, 3u);
    TL_ASSERT_EQ(sp.count, 3u);
    for (u32 i = 0; i < 3u; ++i) {
        TL_EXPECT_EQ(((const EvHit*)(const void*)sh.data)[i].damage, (i32)i);
        TL_EXPECT_EQ(((const EvPing*)(const void*)sp.data)[i].stamp, 100u + i);
    }

    // The two halves are distinct fixed blocks per type (double-buffer, not a ring).
    TL_EXPECT_NE((const void*)f.ev.t[hit].block[0], (const void*)f.ev.t[hit].block[1]);
    TL_EXPECT_TRUE(f.ev.t[hit].read == f.ev.t[hit].block[0]);   // tick-N writes lived in half 0
    TL_EXPECT_TRUE(f.ev.t[hit].write == f.ev.t[hit].block[1]);
}

TL_TEST(events_clear_all_wipes_both_halves, "core,ecs,events,edge,fast") {
    // The rollback path: pending writes AND the readable last tick both vanish.
    EvFixture f;
    TL_ASSERT_TRUE(ev_fixture_init(&f, 1u * 1024u * 1024u));
    const u32 ping = events_register(&f.ev, &EvPing_info, 4u);
    EvPing p = { 7u };
    events_emit(&f.ev, ping, &p);
    events_swap(&f.ev);          // p readable
    events_emit(&f.ev, ping, &p);   // and one pending
    TL_EXPECT_EQ(events_read(&f.ev, ping).count, 1u);
    events_clear_all(&f.ev);
    TL_EXPECT_EQ(events_read(&f.ev, ping).count, 0u);
    events_swap(&f.ev);
    TL_EXPECT_EQ(events_read(&f.ev, ping).count, 0u);
}

TL_TEST_EXPECT_FATAL(events_overflow_is_fatal_not_a_drop, "core,ecs,events,fatal") {
    EvFixture f;
    if (!ev_fixture_init(&f, 1u * 1024u * 1024u)) { return; }
    const u32 ping = events_register(&f.ev, &EvPing_info, 2u);
    EvPing p = { 1u };
    events_emit(&f.ev, ping, &p);
    events_emit(&f.ev, ping, &p);
    ++t->checks;
    events_emit(&f.ev, ping, &p);   // cap 2: the third emit must TL_CHECK-fatal in every tier
}
