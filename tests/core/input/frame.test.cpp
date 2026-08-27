// frame.test.cpp - InputFrame/ActionState static_asserts (executable so a layout regression fails
// a test run, not just a compile), input_zero_frame, PeerSlots layout.
// Spec: docs/INPUT.md §1, §7, §8 R-2.
#include "runner/tl_test.h"
#include "core/input.h"

TL_TEST(input_frame_layout, "core,input,frame,fast") {
    TL_EXPECT_EQ(sizeof(ActionState), 2u);
    TL_EXPECT_EQ(sizeof(InputFrame), 76u);
    TL_EXPECT_EQ((u32)MAX_ACTIONS, 32u);
    TL_EXPECT_EQ(MAX_PEERS, 8u);
    TL_EXPECT_EQ(offsetof(InputFrame, actions), 0u);
    TL_EXPECT_EQ(offsetof(InputFrame, pointer_x), 64u);
    TL_EXPECT_EQ(offsetof(InputFrame, pointer_y), 68u);
    TL_EXPECT_EQ(offsetof(InputFrame, tick), 72u);
}

TL_TEST(input_zero_frame_is_all_zero, "core,input,frame,fast") {
    const InputFrame f = input_zero_frame();
    TL_ASSERT_EQ(f.tick, 0u);
    TL_ASSERT_EQ(f.pointer_x, 0);
    TL_ASSERT_EQ(f.pointer_y, 0);
    for (u32 a = 0; a < MAX_ACTIONS; ++a) {
        TL_EXPECT_EQ(f.actions[a].value, (i8)0);
        TL_EXPECT_EQ(f.actions[a].flags, (u8)0);
    }
}

TL_TEST(peer_slots_layout, "core,input,frame,fast") {
    TL_EXPECT_EQ(sizeof(PeerSlots), 8u + 8u * MAX_PEERS);
    TL_EXPECT_EQ((PeerSlots_info.flags & COMP_SINGLETON), (u32)COMP_SINGLETON);
    PeerSlots ps{};
    ps.live_mask = 0b1;
    ps.local_slot = 0;
    ps.slot_player_id[0] = 42u;
    TL_EXPECT_EQ(ps.slot_player_id[0], 42u);
    TL_EXPECT_EQ(ps.slot_player_id[1], 0u);
}
