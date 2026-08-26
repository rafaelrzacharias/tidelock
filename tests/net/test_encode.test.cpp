// T1 (docs/NETCODE.md §20.6): the column codec - round-trip for 1..9 frames, the 3 B/frame idle
// steady state, every action changing, the pointer at ±(1<<30), and the rec & 0xF0 refusal.
// Spec: docs/NETCODE.md §20.2.2 (byte layout), §20.3(a) (algorithm and refusals).
#include "runner/tl_test.h"
#include "net/net_test_util.h"

// A column never exceeds this in these fixtures: 9 frames x (5 changed + 32*2 rec/value + 10
// pointer) is comfortably under it, and a fixed buffer keeps the encoder's overflow TL_CHECK
// meaningful (a producer that blows its budget is a bug, docs/NETCODE.md §20.1).
enum { EC_BUF = 4096 };

TL_TEST(encode_column_round_trips_one_to_nine_frames, "net,encode,smoke,fast") {
    // MAX_TICKS_PER_PACKET is 9, so 1..9 is the whole legal range of a column's length.
    for (u32 n = 1; n <= MAX_TICKS_PER_PACKET; ++n) {
        WireFrame src[MAX_TICKS_PER_PACKET];
        for (u32 i = 0; i < n; ++i) {
            // Content that moves in every dimension the codec encodes: a rotating action set, an
            // analog value, edge flags, and a pointer that is neither still nor linear.
            src[i] = nt_digital_frame(1000u + i, (1u << (i % NET_FRAME_MAX_ACTIONS)) | 0x5u,
                                      (i32)(i * 37) - 11, -(i32)(i * i * 5) + 3);
            nt_set_action(&src[i], 7u, (i8)(-100 + (i32)i * 7), 1u);          // analog, down
            nt_set_action(&src[i], 9u, (i8)0, (u8)((i % 2u) ? 2u : 4u));      // pressed / released
        }

        u8 buf[EC_BUF];
        ByteWriter w;
        bw_init(&w, buf, sizeof(buf));
        encode_column(&w, src, n);

        WireFrame got[MAX_TICKS_PER_PACKET];
        ByteReader r;
        br_init(&r, buf, w.len);
        TL_ASSERT_EQ(decode_column(&r, got, n, 1000u), ERR_OK);
        TL_EXPECT_EQ(r.pos, r.len);   // the column is exactly consumed - no slack, no shortfall

        for (u32 i = 0; i < n; ++i) {
            TL_EXPECT_TRUE(nt_frames_equal_payload(&src[i], &got[i]));
            TL_EXPECT_EQ(got[i].tick, 1000u + i);   // derived, never transmitted
        }
    }
}

TL_TEST(encode_column_idle_steady_state_is_three_bytes_per_frame, "net,encode,fast") {
    // docs/NETCODE.md §20.2.2: "Steady state: 1 + 1 + 1 = 3 B; an idle peer's column of 9 frames
    // is 27 B." changed = 0 (1 B) + svarint(0) + svarint(0).
    WireFrame idle[MAX_TICKS_PER_PACKET];
    for (u32 i = 0; i < MAX_TICKS_PER_PACKET; ++i) { idle[i] = nt_zero_frame(500u + i); }

    u8 buf[EC_BUF];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encode_column(&w, idle, MAX_TICKS_PER_PACKET);
    TL_EXPECT_EQ(w.len, (u64)27u);
    TL_EXPECT_EQ(w.len, (u64)(3u * MAX_TICKS_PER_PACKET));
    // The bytes themselves, hand-written: nine repeats of 00 00 00.
    for (u32 i = 0; i < 27u; ++i) { TL_EXPECT_EQ(buf[i], (u8)0u); }

    // A peer holding one action down and not moving is idle too from frame 1 onward: frame 0
    // pays for the change, every later frame is the same 3 B.
    WireFrame held[MAX_TICKS_PER_PACKET];
    for (u32 i = 0; i < MAX_TICKS_PER_PACKET; ++i) { held[i] = nt_digital_frame(700u + i, 1u, 0, 0); }
    ByteWriter w2;
    bw_init(&w2, buf, sizeof(buf));
    encode_column(&w2, held, MAX_TICKS_PER_PACKET);
    // frame 0: changed(1) + rec(1) + dvx(1) + dvy(1) = 4; frames 1..8: 3 each.
    TL_EXPECT_EQ(w2.len, (u64)(4u + 3u * (MAX_TICKS_PER_PACKET - 1u)));
}

TL_TEST(encode_column_every_action_changing_round_trips, "net,encode,edge,fast") {
    // The worst case for `changed`: all 32 bits set on every frame, with values that force the
    // explicit value byte (analog values that do not equal the down bit).
    WireFrame src[MAX_TICKS_PER_PACKET];
    for (u32 i = 0; i < MAX_TICKS_PER_PACKET; ++i) {
        src[i] = nt_zero_frame(0u);
        for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
            // Distinct on every frame, so every action's bit is set every time.
            nt_set_action(&src[i], a, (i8)(-127 + (i32)((a + i * 5u) % 255u)), (u8)((a + i) % 8u));
        }
    }
    u8 buf[EC_BUF];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encode_column(&w, src, MAX_TICKS_PER_PACKET);

    WireFrame got[MAX_TICKS_PER_PACKET];
    ByteReader r;
    br_init(&r, buf, w.len);
    TL_ASSERT_EQ(decode_column(&r, got, MAX_TICKS_PER_PACKET, 0u), ERR_OK);
    TL_EXPECT_EQ(r.pos, r.len);
    for (u32 i = 0; i < MAX_TICKS_PER_PACKET; ++i) {
        TL_EXPECT_TRUE(nt_frames_equal_payload(&src[i], &got[i]));
    }
    // changed = 0xFFFFFFFF is a 5-byte uvarint; the first frame must start with it.
    TL_EXPECT_EQ(buf[0], (u8)0xFFu);
}

TL_TEST(encode_column_pointer_at_the_spec_extremes, "net,encode,edge,fast") {
    // docs/NETCODE.md §20.6 T1 names ±(1<<30); §20.2.2 adds "frame 0's absolute pointer costs
    // <= 10 B per column per packet", which is two 5-byte svarints.
    const i32 hi = 1 << 30;
    const i32 lo = -(1 << 30);
    WireFrame src[3];
    src[0] = nt_zero_frame(0u); src[0].pointer_x = hi;  src[0].pointer_y = lo;
    src[1] = nt_zero_frame(0u); src[1].pointer_x = lo;  src[1].pointer_y = hi;
    src[2] = nt_zero_frame(0u); src[2].pointer_x = 0;   src[2].pointer_y = 0;

    u8 buf[EC_BUF];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encode_column(&w, src, 3u);

    WireFrame got[3];
    ByteReader r;
    br_init(&r, buf, w.len);
    TL_ASSERT_EQ(decode_column(&r, got, 3u, 42u), ERR_OK);
    for (u32 i = 0; i < 3u; ++i) {
        TL_EXPECT_EQ(got[i].pointer_x, src[i].pointer_x);
        TL_EXPECT_EQ(got[i].pointer_y, src[i].pointer_y);
    }
    // Frame 0 is changed(1) + dvx + dvy, and its pointer pair costs at most 10 B.
    TL_EXPECT_LE(w.len, (u64)(1u + 10u) + 2u * (u64)(1u + 10u));

    // The i32 extremes themselves, where the second difference wraps: the codec is required to
    // be lossless over the integers, not merely over "reasonable" ones.
    WireFrame ext[4];
    ext[0] = nt_zero_frame(0u); ext[0].pointer_x = 2147483647;              ext[0].pointer_y = 0;
    ext[1] = nt_zero_frame(0u); ext[1].pointer_x = (i32)(-2147483647 - 1);  ext[1].pointer_y = 1;
    ext[2] = nt_zero_frame(0u); ext[2].pointer_x = 2147483647;              ext[2].pointer_y = -1;
    ext[3] = nt_zero_frame(0u); ext[3].pointer_x = 0;                       ext[3].pointer_y = 0;
    ByteWriter w2;
    bw_init(&w2, buf, sizeof(buf));
    encode_column(&w2, ext, 4u);
    WireFrame got2[4];
    ByteReader r2;
    br_init(&r2, buf, w2.len);
    TL_ASSERT_EQ(decode_column(&r2, got2, 4u, 0u), ERR_OK);
    for (u32 i = 0; i < 4u; ++i) {
        TL_EXPECT_EQ(got2[i].pointer_x, ext[i].pointer_x);
        TL_EXPECT_EQ(got2[i].pointer_y, ext[i].pointer_y);
    }
}

TL_TEST(encode_column_refuses_a_rec_byte_with_reserved_bits, "net,encode,edge,fast") {
    // docs/NETCODE.md §20.3(a): the decoder "rejects rec & 0xF0 != 0". One action changing puts
    // the rec byte at a known offset: changed is 1 byte (bit 0 set -> 0x01), rec is byte 1.
    WireFrame src[1];
    src[0] = nt_digital_frame(0u, 1u, 0, 0);

    u8 buf[EC_BUF];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encode_column(&w, src, 1u);
    TL_ASSERT_EQ(buf[0], (u8)0x01u);          // changed = bit 0
    const u8 rec_ok = buf[1];
    TL_EXPECT_EQ((u8)(rec_ok & WIRE_REC_RESERVED_MASK), (u8)0u);

    WireFrame got[1];
    // Each reserved bit on its own is a refusal - not just the whole nibble at once.
    for (u32 b = 4; b < 8; ++b) {
        buf[1] = (u8)(rec_ok | (1u << b));
        ByteReader r;
        br_init(&r, buf, w.len);
        TL_EXPECT_EQ(decode_column(&r, got, 1u, 0u), ERR_NET_MALFORMED);
    }
    // Restored, the same bytes decode cleanly: the reserved bit was the only objection.
    buf[1] = rec_ok;
    ByteReader r;
    br_init(&r, buf, w.len);
    TL_EXPECT_EQ(decode_column(&r, got, 1u, 0u), ERR_OK);
}

TL_TEST(encode_column_refuses_every_truncated_prefix, "net,encode,edge,fast") {
    // A column is only decodable in full; every short read is DATA, never a partial frame set.
    WireFrame src[4];
    for (u32 i = 0; i < 4u; ++i) {
        src[i] = nt_digital_frame(i, 0x10001u, (i32)i * 100000, -(i32)i * 7);
        nt_set_action(&src[i], 3u, (i8)(50 + (i32)i), 1u);
    }
    u8 buf[EC_BUF];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encode_column(&w, src, 4u);

    WireFrame got[4];
    for (u64 cut = 0; cut < w.len; ++cut) {
        ByteReader r;
        br_init(&r, buf, cut);
        const ErrCode e = decode_column(&r, got, 4u, 0u);
        TL_EXPECT_NE(e, ERR_OK);
        // Truncation is the byte pair's code; a varint cut mid-continuation reports it too.
        TL_EXPECT_EQ(e, ERR_BYTES_TRUNCATED);
    }
}

TL_TEST(encode_column_edge_flags_with_an_unchanged_value_still_cross, "net,encode,edge,fast") {
    // `changed` compares the whole ActionState, not just the value. An action held down whose
    // pressed bit clears on the next frame has an IDENTICAL value and must still be transmitted -
    // this is the row that fails if `changed` is ever narrowed to a value comparison.
    WireFrame src[2];
    src[0] = nt_zero_frame(0u);
    nt_set_action(&src[0], 5u, (i8)1, (u8)(1u | 2u));   // down + pressed-this-tick
    src[1] = nt_zero_frame(0u);
    nt_set_action(&src[1], 5u, (i8)1, (u8)1u);          // down, no longer pressed

    u8 buf[EC_BUF];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encode_column(&w, src, 2u);

    WireFrame got[2];
    ByteReader r;
    br_init(&r, buf, w.len);
    TL_ASSERT_EQ(decode_column(&r, got, 2u, 0u), ERR_OK);
    TL_EXPECT_EQ(got[0].actions[5].flags, (u8)3u);
    TL_EXPECT_EQ(got[1].actions[5].flags, (u8)1u);
    TL_EXPECT_EQ(got[1].actions[5].value, (i8)1);
    TL_EXPECT_TRUE(nt_frames_equal_payload(&src[1], &got[1]));
}

TL_TEST(encode_column_digital_actions_carry_no_value_byte, "net,encode,fast") {
    // docs/NETCODE.md §20.2.2: value_follows is "0 for every digital action by construction".
    // A digital down is value 1 / flags bit0, so the implied value matches and no byte follows.
    WireFrame down[1];
    down[0] = nt_digital_frame(0u, 1u, 0, 0);
    u8 buf[EC_BUF];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encode_column(&w, down, 1u);
    // changed(1) + rec(1) + dvx(1) + dvy(1) - no value byte.
    TL_EXPECT_EQ(w.len, (u64)4u);
    TL_EXPECT_EQ((u8)(buf[1] & WIRE_REC_VALUE_FOLLOWS), (u8)0u);

    // An analog value that differs from the down bit DOES pay for a byte.
    WireFrame ana[1];
    ana[0] = nt_zero_frame(0u);
    nt_set_action(&ana[0], 0u, (i8)-42, (u8)1u);
    ByteWriter w2;
    bw_init(&w2, buf, sizeof(buf));
    encode_column(&w2, ana, 1u);
    TL_EXPECT_EQ(w2.len, (u64)5u);
    TL_EXPECT_EQ((u8)(buf[1] & WIRE_REC_VALUE_FOLLOWS), (u8)WIRE_REC_VALUE_FOLLOWS);
    TL_EXPECT_EQ((i8)buf[2], (i8)-42);
}

TL_TEST(encode_column_zero_frames_is_an_empty_column, "net,encode,edge,fast") {
    // PK_KEEPALIVE has frame_count = 0 (docs/NETCODE.md §20.2.2): the column is empty, not
    // absent, and decoding zero frames from an empty reader is success, not truncation.
    u8 buf[EC_BUF];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encode_column(&w, nullptr, 0u);
    TL_EXPECT_EQ(w.len, (u64)0u);

    ByteReader r;
    br_init(&r, buf, 0);
    TL_EXPECT_EQ(decode_column(&r, nullptr, 0u, 0u), ERR_OK);
}

TL_TEST(encode_column_output_is_a_pure_function_of_its_frames, "net,encode,determinism,fast") {
    // Determinism at the level that matters for lockstep: the same frames encode to the same
    // BYTES, and a column's bytes do not depend on base_tick (the tick is never transmitted) or
    // on anything the encoder was asked to do previously.
    WireFrame src[MAX_TICKS_PER_PACKET];
    for (u32 i = 0; i < MAX_TICKS_PER_PACKET; ++i) {
        src[i] = nt_digital_frame(i, (u32)nt_mix64(0xF00Du, i), (i32)nt_mix64(0xAAu, i),
                                  (i32)nt_mix64(0xBBu, i));
        nt_set_action(&src[i], 11u, (i8)nt_mix64(0xCCu, i), (u8)(nt_mix64(0xDDu, i) & 7u));
    }
    u8 a[EC_BUF], b[EC_BUF];
    ByteWriter wa, wb;
    bw_init(&wa, a, sizeof(a));
    bw_init(&wb, b, sizeof(b));
    encode_column(&wa, src, MAX_TICKS_PER_PACKET);
    // A different column encoded in between must leave no trace on the next one.
    u8 scratch[EC_BUF];
    ByteWriter ws;
    bw_init(&ws, scratch, sizeof(scratch));
    encode_column(&ws, src, 3u);
    encode_column(&wb, src, MAX_TICKS_PER_PACKET);
    TL_ASSERT_EQ(wa.len, wb.len);
    TL_EXPECT_EQ(memcmp(a, b, (usize)wa.len), 0);

    // Decoding the same bytes at two different base ticks differs only in the derived tick.
    WireFrame g1[MAX_TICKS_PER_PACKET], g2[MAX_TICKS_PER_PACKET];
    ByteReader r1, r2;
    br_init(&r1, a, wa.len);
    br_init(&r2, a, wa.len);
    TL_ASSERT_EQ(decode_column(&r1, g1, MAX_TICKS_PER_PACKET, 0u), ERR_OK);
    TL_ASSERT_EQ(decode_column(&r2, g2, MAX_TICKS_PER_PACKET, 9000u), ERR_OK);
    for (u32 i = 0; i < MAX_TICKS_PER_PACKET; ++i) {
        TL_EXPECT_TRUE(nt_frames_equal_payload(&g1[i], &g2[i]));
        TL_EXPECT_EQ(g1[i].tick, i);
        TL_EXPECT_EQ(g2[i].tick, 9000u + i);
    }
}
