// fold.test.cpp - the Live producer's fold algorithm (docs/INPUT.md §9.3, §7 test list: edge
// derivation, analog quantization, deadzone shapes, SOCD, chord specificity, context-switch
// synthetic release).
#include "runner/tl_test.h"
#include "core/producers/live.h"
#include "foundation/vmem_test_api.h"

namespace {

struct FoldFixture {
    VMemApi api;
    VMemArena arena;
    ActionMap map;
    LiveProducer lp;
};

bool fold_fixture_init(FoldFixture* f, u32 max_bindings) {
    f->api = test_vmem_api();
    if (vmem_arena_init(&f->arena, "fold.test"_id, 1024u * 1024u, 0u, &f->api) != ERR_OK) { return false; }
    action_map_init(&f->map, &f->arena, max_bindings);
    live_producer_init(&f->lp, &f->map, 0u, ImGuiCaptureApi{});
    return true;
}

RawEvent key_ev(u32 scancode, u8 down, u16 mods) {
    RawEvent e{};
    e.kind = EV_KEY;
    e.u.key.scancode = scancode;
    e.u.key.down = down;
    e.u.key.mods = mods;
    return e;
}

RawEvent pad_axis_ev(u8 pad, u8 axis, i16 value) {
    RawEvent e{};
    e.kind = EV_PAD_AXIS;
    e.u.pad_axis.pad = pad;
    e.u.pad_axis.axis = axis;
    e.u.pad_axis.value = value;
    return e;
}

}  // namespace

TL_TEST(fold_edge_derivation_over_three_ticks, "core,input,fold,edges,fast") {
    FoldFixture f;
    TL_ASSERT_TRUE(fold_fixture_init(&f, 4u));
    const ActionId jump = action_register(&f.map, "jump"_id, 0u, ACT_DIGITAL, CLS_EDGE);
    Binding b{}; b.action = jump; b.dev = DEV_KEY; b.code_pos = 44u;
    action_bind(&f.map, b);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;

    // Tick 0: fresh press -> down + pressed.
    RawEvent press = key_ev(44u, 1u, 0u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &press, 1u, 0u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[jump].flags, (u8)(AS_DOWN | AS_PRESSED));
    TL_EXPECT_EQ(out[0].actions[jump].value, (i8)1);

    // Tick 1: still held, no new event -> down only.
    TL_ASSERT_EQ(live_produce_frame(&f.lp, nullptr, 0u, 1u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[jump].flags, (u8)AS_DOWN);

    // Tick 2: release -> released only (not down).
    RawEvent release = key_ev(44u, 0u, 0u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &release, 1u, 2u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[jump].flags, (u8)AS_RELEASED);
    TL_EXPECT_EQ(out[0].actions[jump].value, (i8)0);

    // Tick 3: neither.
    TL_ASSERT_EQ(live_produce_frame(&f.lp, nullptr, 0u, 3u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[jump].flags, (u8)0);
    TL_EXPECT_EQ(live_mask, (u8)1u);
}

TL_TEST(fold_analog_quantization_bounds, "core,input,fold,analog,fast") {
    FoldFixture f;
    TL_ASSERT_TRUE(fold_fixture_init(&f, 4u));
    const ActionId move_x = action_register(&f.map, "move_x"_id, 0u, ACT_ANALOG, CLS_AXIS);
    Binding b{}; b.action = move_x; b.dev = DEV_KEYS_AXIS; b.code_neg = 1u; b.code_pos = 2u;
    b.socd = SOCD_NEUTRAL;
    action_bind(&f.map, b);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;

    RawEvent pos = key_ev(2u, 1u, 0u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &pos, 1u, 0u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[move_x].value, (i8)127);   // +1.0 * 127, RNE

    RawEvent release_pos = key_ev(2u, 0u, 0u);
    RawEvent neg = key_ev(1u, 1u, 0u);
    RawEvent evs[2] = { release_pos, neg };
    TL_ASSERT_EQ(live_produce_frame(&f.lp, evs, 2u, 1u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[move_x].value, (i8)-127);  // -1.0 * 127

    RawEvent release_neg = key_ev(1u, 0u, 0u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &release_neg, 1u, 2u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[move_x].value, (i8)0);     // neutral
}

TL_TEST(fold_deadzone_axial_shape, "core,input,fold,deadzone,fast") {
    FoldFixture f;
    TL_ASSERT_TRUE(fold_fixture_init(&f, 4u));
    const ActionId look_x = action_register(&f.map, "look_x"_id, 0u, ACT_ANALOG, CLS_AXIS);
    Binding b{}; b.action = look_x; b.dev = DEV_PAD_AXIS; b.code_neg = 0u; b.code_pos = 0u;
    b.dz = DZ_AXIAL; b.dz_radius = 0.5f; b.sensitivity = 1.0f;
    action_bind(&f.map, b);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;

    // Below the deadzone radius (~0.1 of full scale): clamps to exactly 0.
    RawEvent small = pad_axis_ev(0u, 0u, 3000);   // 3000/32768 ~= 0.0916
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &small, 1u, 0u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[look_x].value, (i8)0);

    // Full deflection: rescaled past the deadzone to (approximately) full scale.
    RawEvent full = pad_axis_ev(0u, 0u, 32767);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &full, 1u, 1u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_GT(out[0].actions[look_x].value, (i8)100);
}

TL_TEST(fold_socd_neutral_and_last_wins, "core,input,fold,socd,fast") {
    FoldFixture f;
    TL_ASSERT_TRUE(fold_fixture_init(&f, 4u));
    const ActionId ax_neutral = action_register(&f.map, "ax_neutral"_id, 0u, ACT_ANALOG, CLS_AXIS);
    const ActionId ax_last = action_register(&f.map, "ax_last"_id, 0u, ACT_ANALOG, CLS_AXIS);
    Binding bn{}; bn.action = ax_neutral; bn.dev = DEV_KEYS_AXIS; bn.code_neg = 10u; bn.code_pos = 11u; bn.socd = SOCD_NEUTRAL;
    action_bind(&f.map, bn);
    Binding bl{}; bl.action = ax_last; bl.dev = DEV_KEYS_AXIS; bl.code_neg = 10u; bl.code_pos = 11u; bl.socd = SOCD_LAST_WINS;
    action_bind(&f.map, bl);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;

    // Both directions held: SOCD_NEUTRAL cancels to 0; SOCD_LAST_WINS follows the more recent press.
    RawEvent neg_down = key_ev(10u, 1u, 0u);
    RawEvent pos_down = key_ev(11u, 1u, 0u);
    RawEvent both[2] = { neg_down, pos_down };
    TL_ASSERT_EQ(live_produce_frame(&f.lp, both, 2u, 0u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[ax_neutral].value, (i8)0);
    TL_EXPECT_EQ(out[0].actions[ax_last].value, (i8)127);   // pos pressed most recently (this tick, after neg)

    // Releasing pos leaves last_wins tracking neg (still held).
    RawEvent pos_up = key_ev(11u, 0u, 0u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &pos_up, 1u, 1u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[ax_last].value, (i8)-127);
}

TL_TEST(fold_chord_specificity, "core,input,fold,chord,fast") {
    FoldFixture f;
    TL_ASSERT_TRUE(fold_fixture_init(&f, 4u));
    const ActionId type_s = action_register(&f.map, "type_s"_id, 0u, ACT_DIGITAL, CLS_EDGE);
    const ActionId save = action_register(&f.map, "save"_id, 0u, ACT_DIGITAL, CLS_EDGE);
    Binding bs{}; bs.action = type_s; bs.dev = DEV_KEY; bs.code_pos = 22u; bs.modifiers = 0u;
    action_bind(&f.map, bs);
    Binding bctrl{}; bctrl.action = save; bctrl.dev = DEV_KEY; bctrl.code_pos = 22u; bctrl.modifiers = 0x02u;   // ctrl
    action_bind(&f.map, bctrl);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;

    // 's' alone: only the unmodified binding (type_s) is satisfied.
    RawEvent s_alone = key_ev(22u, 1u, 0u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &s_alone, 1u, 0u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_TRUE((out[0].actions[type_s].flags & AS_DOWN) != 0u);
    TL_EXPECT_TRUE((out[0].actions[save].flags & AS_DOWN) == 0u);

    RawEvent s_up = key_ev(22u, 0u, 0u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &s_up, 1u, 1u, out, &live_mask), PRODUCE_READY);

    // ctrl+s: the more-specific binding (save) wins; type_s does NOT fire even though 's' is down.
    RawEvent ctrl_s = key_ev(22u, 1u, 0x02u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &ctrl_s, 1u, 2u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_TRUE((out[0].actions[save].flags & AS_DOWN) != 0u);
    TL_EXPECT_TRUE((out[0].actions[type_s].flags & AS_DOWN) == 0u);
}

TL_TEST(fold_context_switch_synthetic_release, "core,input,fold,context,fast") {
    FoldFixture f;
    TL_ASSERT_TRUE(fold_fixture_init(&f, 4u));
    const ActionId jump = action_register(&f.map, "jump"_id, 0u, ACT_DIGITAL, CLS_EDGE);
    Binding b{}; b.action = jump; b.dev = DEV_KEY; b.code_pos = 44u; b.context = CONTEXT_DEFAULT;
    action_bind(&f.map, b);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;

    RawEvent press = key_ev(44u, 1u, 0u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &press, 1u, 0u, out, &live_mask), PRODUCE_READY);
    TL_ASSERT_TRUE((out[0].actions[jump].flags & AS_DOWN) != 0u);

    // Switch to a context with no binding for `jump`; the physical key is still held, but the
    // action naturally resolves to not-down under the new context - a synthetic release.
    action_map_set_context(&f.map, 1u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, nullptr, 0u, 1u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[jump].flags, (u8)AS_RELEASED);
}
