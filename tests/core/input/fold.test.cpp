// fold.test.cpp - the Live producer's fold algorithm (docs/INPUT.md §9.3, §7 test list: edge
// derivation, analog quantization, deadzone shapes, SOCD, chord specificity, context-switch
// synthetic release).
#include "runner/tl_test.h"
#include "core/producers/live.h"
#include "foundation/vmem_test_api.h"
#include "foundation/ring.h"

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

RawEvent pad_connect_ev(u8 pad, u8 connected) {
    RawEvent e{};
    e.kind = EV_PAD_CONNECT;
    e.u.pad_connect.pad = pad;
    e.u.pad_connect.connected = connected;
    return e;
}

struct CaptureState { u8 want_mouse; u8 want_keyboard; };
void capture_fn(void* ctx, u8* want_mouse, u8* want_keyboard) {
    CaptureState* s = (CaptureState*)ctx;
    *want_mouse = s->want_mouse;
    *want_keyboard = s->want_keyboard;
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

    // Full deflection: rescaled past the deadzone to 0.99993896484375, * 127 = 126.99224... -
    // RNE rounds up to 127; plain truncation would give 126 (this is the discriminating case a
    // truncation mutation at live.cpp's quantization call site is caught by - a TL_EXPECT_GT
    // floor is satisfied by both answers and catches neither).
    RawEvent full = pad_axis_ev(0u, 0u, 32767);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &full, 1u, 1u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[look_x].value, (i8)127);

    // Symmetry (docs/INPUT.md section 9.6 "analog quantization +/-127 and symmetry"): the mirrored
    // negative deflection rescales to -0.99993896484375, * 127 = -126.99224... - RNE rounds away
    // from zero to -127, matching -(q(32767)) exactly.
    RawEvent full_neg = pad_axis_ev(0u, 0u, -32767);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &full_neg, 1u, 2u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[look_x].value, (i8)-127);
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

TL_TEST(live_produce_ring_silently_drops_the_oldest_excess, "core,input,live,ring,fast") {
    // Review round 1 finding 9: live_produce (the actual InputProducer fn-ptr, ctx = a ring) was
    // never exercised - every fold test calls live_produce_frame directly with a hand-built event
    // array. This drives the real ring: overwrite_oldest silently evicts events pushed past
    // capacity between two produce() calls (live.h's own contract block on live_produce).
    FoldFixture f;
    TL_ASSERT_TRUE(fold_fixture_init(&f, 8u));
    const ActionId actions[6] = {
        action_register(&f.map, "a1"_id, 0u, ACT_DIGITAL, CLS_EDGE),
        action_register(&f.map, "a2"_id, 0u, ACT_DIGITAL, CLS_EDGE),
        action_register(&f.map, "a3"_id, 0u, ACT_DIGITAL, CLS_EDGE),
        action_register(&f.map, "a4"_id, 0u, ACT_DIGITAL, CLS_EDGE),
        action_register(&f.map, "a5"_id, 0u, ACT_DIGITAL, CLS_EDGE),
        action_register(&f.map, "a6"_id, 0u, ACT_DIGITAL, CLS_EDGE),
    };
    for (u32 i = 0; i < 6u; ++i) {
        Binding b{}; b.action = actions[i]; b.dev = DEV_KEY; b.code_pos = (u16)(1u + i);
        action_bind(&f.map, b);
    }

    VMemArena ring_arena;
    TL_ASSERT_EQ(vmem_arena_init(&ring_arena, "fold.test.ring"_id, 64u * 1024u, 0u, &f.api), ERR_OK);
    RingBuffer<RawEvent> ring;
    ring_init(&ring, &ring_arena, 4u, true);   // cap 4, overwrite-oldest

    // Push 6 key-down events (scancodes 1..6) into a cap-4 ring: 1 and 2 are evicted before
    // live_produce ever sees them, not truncated by its own drain loop.
    for (u32 sc = 1u; sc <= 6u; ++sc) {
        TL_ASSERT_TRUE(ring_push(&ring, key_ev(sc, 1u, 0u)));
    }

    LiveProducerCtx ctx{ &f.lp, &ring };
    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;
    TL_ASSERT_EQ(live_produce(&ctx, 0u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_TRUE(ring_empty(&ring));   // everything the ring still held got drained in one call

    TL_EXPECT_EQ(out[0].actions[actions[0]].flags & AS_DOWN, (u8)0);   // scancode 1: dropped, never seen
    TL_EXPECT_EQ(out[0].actions[actions[1]].flags & AS_DOWN, (u8)0);   // scancode 2: dropped, never seen
    for (u32 i = 2; i < 6u; ++i) {
        TL_EXPECT_TRUE((out[0].actions[actions[i]].flags & AS_DOWN) != 0u);   // scancodes 3..6 survived
    }
}

TL_TEST(fold_socd_first_wins_release_fallback, "core,input,fold,socd,fast") {
    // Review round 1 finding 12: the SOCD_FIRST_WINS release-driven fallback branch
    // (live.cpp's `else if (s.last_dir == 1 && !pos_down) ...`) had no test.
    FoldFixture f;
    TL_ASSERT_TRUE(fold_fixture_init(&f, 4u));
    const ActionId ax = action_register(&f.map, "ax_first"_id, 0u, ACT_ANALOG, CLS_AXIS);
    Binding b{}; b.action = ax; b.dev = DEV_KEYS_AXIS; b.code_neg = 10u; b.code_pos = 11u; b.socd = SOCD_FIRST_WINS;
    action_bind(&f.map, b);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;

    // pos pressed first: locks the winning side to pos.
    RawEvent pos_down = key_ev(11u, 1u, 0u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &pos_down, 1u, 0u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[ax].value, (i8)127);

    // neg pressed while pos still held: SOCD_FIRST_WINS ignores it - the first press already won.
    RawEvent neg_down = key_ev(10u, 1u, 0u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &neg_down, 1u, 1u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[ax].value, (i8)127);

    // pos (the winning side) releases while neg is still held: the release-driven fallback engages.
    RawEvent pos_up = key_ev(11u, 0u, 0u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &pos_up, 1u, 2u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[ax].value, (i8)-127);
}

TL_TEST(fold_deadzone_none_trigger_and_radial_alias, "core,input,fold,deadzone,fast") {
    // Review round 1 finding 12: DZ_NONE, DZ_TRIGGER, and DZ_RADIAL (a documented no-op alias for
    // DZ_AXIAL) had no test at all.
    FoldFixture f;
    TL_ASSERT_TRUE(fold_fixture_init(&f, 8u));
    const ActionId ax_none = action_register(&f.map, "ax_none"_id, 0u, ACT_ANALOG, CLS_AXIS);
    const ActionId ax_trig = action_register(&f.map, "ax_trig"_id, 0u, ACT_ANALOG, CLS_AXIS);
    const ActionId ax_axial = action_register(&f.map, "ax_axial"_id, 0u, ACT_ANALOG, CLS_AXIS);
    const ActionId ax_radial = action_register(&f.map, "ax_radial"_id, 0u, ACT_ANALOG, CLS_AXIS);

    Binding bn{}; bn.action = ax_none; bn.dev = DEV_PAD_AXIS; bn.code_neg = 0u; bn.code_pos = 0u;
    bn.dz = DZ_NONE; bn.sensitivity = 1.0f;
    action_bind(&f.map, bn);
    Binding bt{}; bt.action = ax_trig; bt.dev = DEV_PAD_AXIS; bt.code_neg = 0u; bt.code_pos = 1u;
    bt.dz = DZ_TRIGGER; bt.dz_radius = 0.5f; bt.sensitivity = 1.0f;
    action_bind(&f.map, bt);
    Binding ba{}; ba.action = ax_axial; ba.dev = DEV_PAD_AXIS; ba.code_neg = 0u; ba.code_pos = 2u;
    ba.dz = DZ_AXIAL; ba.dz_radius = 0.5f; ba.sensitivity = 1.0f;
    action_bind(&f.map, ba);
    Binding br{}; br.action = ax_radial; br.dev = DEV_PAD_AXIS; br.code_neg = 0u; br.code_pos = 2u;
    br.dz = DZ_RADIAL; br.dz_radius = 0.5f; br.sensitivity = 1.0f;
    action_bind(&f.map, br);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;

    // DZ_NONE: a tiny raw value (32767/32768 * 3000/32768 range) that DZ_AXIAL/DZ_RADIAL at the
    // same radius zero out (fold_deadzone_axial_shape's own "below the radius" case) passes
    // through unclipped here.
    RawEvent small = pad_axis_ev(0u, 0u, 3000);   // 3000/32768 ~= 0.0916
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &small, 1u, 0u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[ax_none].value, (i8)12);   // round(0.0916 * 127) = 12, unclipped

    // DZ_TRIGGER, radius 0.5: below the radius clamps to exactly 0 (axis 1, tick 1).
    RawEvent below = pad_axis_ev(0u, 1u, 4096);   // 4096/32768 = 0.125 < 0.5
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &below, 1u, 1u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[ax_trig].value, (i8)0);

    // DZ_TRIGGER, radius 0.5: above the radius rescales - (0.75 - 0.5)/(1 - 0.5) = 0.5 exactly,
    // * 127 = 63.5 exactly, RNE ties to even -> 64 (axis 1, tick 2).
    RawEvent above = pad_axis_ev(0u, 1u, 24576);   // 24576/32768 = 0.75
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &above, 1u, 2u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[ax_trig].value, (i8)64);

    // DZ_RADIAL is the same code path as DZ_AXIAL (live.cpp's apply_deadzone: one case label for
    // both) - same input on axis 2, both actions must produce byte-identical output.
    RawEvent shared = pad_axis_ev(0u, 2u, 20000);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &shared, 1u, 3u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[ax_axial].value, out[0].actions[ax_radial].value);
    TL_EXPECT_NE(out[0].actions[ax_axial].value, (i8)0);   // and it is not the trivial "both zero" case
}

TL_TEST(fold_imgui_capture_masks_the_matching_device_only, "core,input,fold,capture,fast") {
    // Review round 1 finding 12: the ImGui capture mask (docs/INPUT.md section 5) had no test.
    VMemApi api = test_vmem_api();
    VMemArena arena;
    TL_ASSERT_EQ(vmem_arena_init(&arena, "fold.test.capture"_id, 1024u * 1024u, 0u, &api), ERR_OK);
    ActionMap map;
    action_map_init(&map, &arena, 4u);
    CaptureState cap{ 0u, 0u };
    LiveProducer lp;
    live_producer_init(&lp, &map, 0u, ImGuiCaptureApi{ &cap, capture_fn });

    const ActionId key_act = action_register(&map, "key_act"_id, 0u, ACT_DIGITAL, CLS_EDGE);
    Binding bk{}; bk.action = key_act; bk.dev = DEV_KEY; bk.code_pos = 44u;
    action_bind(&map, bk);
    const ActionId mouse_act = action_register(&map, "mouse_act"_id, 0u, ACT_DIGITAL, CLS_EDGE);
    Binding bm{}; bm.action = mouse_act; bm.dev = DEV_MOUSE_BUTTON; bm.code_pos = 1u;
    action_bind(&map, bm);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;

    // ImGui wants the keyboard only: the key press is masked out before device state even
    // updates; the mouse press (a different device) still reaches the fold.
    cap.want_keyboard = 1u;
    cap.want_mouse = 0u;
    RawEvent key_press = key_ev(44u, 1u, 0u);
    RawEvent mouse_ev{}; mouse_ev.kind = EV_MOUSE_BUTTON; mouse_ev.u.mouse_button.button = 1u; mouse_ev.u.mouse_button.down = 1u;
    RawEvent evs[2] = { key_press, mouse_ev };
    TL_ASSERT_EQ(live_produce_frame(&lp, evs, 2u, 0u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[key_act].flags & AS_DOWN, (u8)0);
    TL_EXPECT_TRUE((out[0].actions[mouse_act].flags & AS_DOWN) != 0u);
}

TL_TEST(fold_pad_disconnect_zeroes_axis_and_button_state, "core,input,fold,pad,fast") {
    // Review round 1 finding 12: pad connect/disconnect state zeroing had no test.
    FoldFixture f;
    TL_ASSERT_TRUE(fold_fixture_init(&f, 4u));
    const ActionId ax = action_register(&f.map, "ax_pad"_id, 0u, ACT_ANALOG, CLS_AXIS);
    Binding b{}; b.action = ax; b.dev = DEV_PAD_AXIS; b.code_neg = 0u; b.code_pos = 0u; b.sensitivity = 1.0f;
    action_bind(&f.map, b);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;

    RawEvent full = pad_axis_ev(0u, 0u, 32767);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &full, 1u, 0u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[ax].value, (i8)127);

    // Disconnecting the pad zeroes its axis (and button) state - the stale full-deflection value
    // does not survive a disconnect even though no new axis event arrives.
    RawEvent disconnect = pad_connect_ev(0u, 0u);
    TL_ASSERT_EQ(live_produce_frame(&f.lp, &disconnect, 1u, 1u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[ax].value, (i8)0);
}
