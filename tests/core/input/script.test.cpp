// script.test.cpp - the Script InputProducer: exact-tick delivery (docs/INPUT.md §9.4, §7).
#include "runner/tl_test.h"
#include "core/producers/script.h"
#include "foundation/vmem_test_api.h"

namespace {
struct ScriptFixture { VMemApi api; VMemArena arena; ScriptProducer sp; };
bool script_fixture_init(ScriptFixture* f, u32 max_events, u8 live_mask) {
    f->api = test_vmem_api();
    if (vmem_arena_init(&f->arena, "script.test"_id, 1024u * 1024u, 0u, &f->api) != ERR_OK) { return false; }
    script_producer_init(&f->sp, &f->arena, max_events, live_mask);
    return true;
}
}  // namespace

TL_TEST(script_press_fires_exactly_one_tick, "core,input,script,fast") {
    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;
    ScriptFixture g;
    TL_ASSERT_TRUE(script_fixture_init(&g, 8u, 0b1u));
    script_press(&g.sp, (ActionId)3u, 5u, 1, 0u);
    for (u64 tick = 0; tick <= 6u; ++tick) {
        TL_ASSERT_EQ(script_produce(&g.sp, tick, out, &live_mask), PRODUCE_READY);
        TL_EXPECT_EQ(out[0].tick, (u32)tick);
        if (tick == 5u) {
            TL_EXPECT_EQ(out[0].actions[3].flags, (u8)(AS_DOWN | AS_PRESSED));
            TL_EXPECT_EQ(out[0].actions[3].value, (i8)1);
        } else if (tick == 6u) {
            TL_EXPECT_EQ(out[0].actions[3].flags, (u8)AS_RELEASED);
        } else {
            TL_EXPECT_EQ(out[0].actions[3].flags, (u8)0);
        }
    }
    TL_EXPECT_EQ(live_mask, (u8)0b1u);
}

TL_TEST(script_hold_brackets_a_range, "core,input,script,fast") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_init(&f, 8u, 0b1u));
    script_hold(&f.sp, (ActionId)1u, 100, 2u, 5u, 0u);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;
    for (u64 tick = 0; tick <= 6u; ++tick) {
        TL_ASSERT_EQ(script_produce(&f.sp, tick, out, &live_mask), PRODUCE_READY);
        const bool expect_down = (tick >= 2u && tick < 5u);
        TL_EXPECT_EQ((out[0].actions[1].flags & AS_DOWN) != 0u, expect_down);
        if (tick == 2u) { TL_EXPECT_TRUE((out[0].actions[1].flags & AS_PRESSED) != 0u); }
        if (tick == 5u) { TL_EXPECT_TRUE((out[0].actions[1].flags & AS_RELEASED) != 0u); }
    }
}

TL_TEST(script_set_persists_until_changed, "core,input,script,fast") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_init(&f, 8u, 0b1u));
    script_set(&f.sp, (ActionId)0u, 64, 1u, 0u);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;
    for (u64 tick = 0; tick <= 3u; ++tick) {
        TL_ASSERT_EQ(script_produce(&f.sp, tick, out, &live_mask), PRODUCE_READY);
        if (tick >= 1u) { TL_EXPECT_EQ(out[0].actions[0].value, (i8)64); }
        else { TL_EXPECT_EQ(out[0].actions[0].value, (i8)0); }
    }
}

TL_TEST(script_multiple_slots_independent, "core,input,script,fast") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_init(&f, 8u, 0b11u));
    script_press(&f.sp, (ActionId)0u, 0u, 1, 0u);
    script_press(&f.sp, (ActionId)0u, 0u, 1, 1u);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;
    TL_ASSERT_EQ(script_produce(&f.sp, 0u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_TRUE((out[0].actions[0].flags & AS_DOWN) != 0u);
    TL_EXPECT_TRUE((out[1].actions[0].flags & AS_DOWN) != 0u);
    TL_EXPECT_EQ(out[2].actions[0].flags, (u8)0);
    TL_EXPECT_EQ(live_mask, (u8)0b11u);
}
