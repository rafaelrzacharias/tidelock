// wire.h's primitives: the varint/zigzag pair the column codec is written in, the frame geometry
// mirror's pins, and the version policy. Spec: docs/NETCODE.md §20.2.2 (varint/zigzag
// definitions), §20.2 (the version rule), docs/INPUT.md §1 (the frame). Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "net/net_test_util.h"

TL_TEST(wire_uvarint_encoding_is_pinned_against_hand_written_bytes, "net,wire,smoke,fast") {
    // Hand-written from the LEB128 definition, not from the encoder (LESSONS: a golden computed
    // by the code it guards is a screenshot).
    struct Case { u32 v; u32 n; u8 want[5]; };
    const Case cases[] = {
        { 0u,          1u, { 0x00 } },
        { 1u,          1u, { 0x01 } },
        { 127u,        1u, { 0x7F } },
        { 128u,        2u, { 0x80, 0x01 } },
        { 300u,        2u, { 0xAC, 0x02 } },
        { 16383u,      2u, { 0xFF, 0x7F } },
        { 16384u,      3u, { 0x80, 0x80, 0x01 } },
        { 0xFFFFFFFFu, 5u, { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F } },
    };
    for (u32 c = 0; c < tl_count(cases); ++c) {
        u8 buf[8];
        ByteWriter w;
        bw_init(&w, buf, sizeof(buf));
        wire_put_uvarint(&w, cases[c].v);
        TL_ASSERT_EQ(w.len, (u64)cases[c].n);
        TL_EXPECT_EQ(wire_uvarint_bytes(cases[c].v), cases[c].n);   // the size oracle agrees
        for (u32 i = 0; i < cases[c].n; ++i) { TL_EXPECT_EQ(buf[i], cases[c].want[i]); }

        ByteReader r;
        br_init(&r, buf, w.len);
        u32 got = 0xDEADBEEFu;
        TL_EXPECT_EQ(wire_get_uvarint(&r, &got), ERR_OK);
        TL_EXPECT_EQ(got, cases[c].v);
        TL_EXPECT_EQ(r.pos, r.len);
    }
}

TL_TEST(wire_zigzag_is_an_involution_over_the_edges, "net,wire,edge,fast") {
    const i32 vals[] = { 0, -1, 1, -2, 2, 63, -64, 1 << 30, -(1 << 30),
                         2147483647, (i32)(-2147483647 - 1) };
    for (u32 i = 0; i < tl_count(vals); ++i) {
        TL_EXPECT_EQ(wire_unzigzag32(wire_zigzag32(vals[i])), vals[i]);
    }
    // The mapping itself, hand-written from the definition.
    TL_EXPECT_EQ(wire_zigzag32(0), 0u);
    TL_EXPECT_EQ(wire_zigzag32(-1), 1u);
    TL_EXPECT_EQ(wire_zigzag32(1), 2u);
    TL_EXPECT_EQ(wire_zigzag32(-2), 3u);
    // Every u32 unzigzags to something that zigzags back: the decoder can be handed any bits.
    for (u32 i = 0; i < 64u; ++i) {
        const u32 z = (i < 32u) ? (1u << i) : nt_mix64(0x5A17u, i) & 0xFFFFFFFFu;
        TL_EXPECT_EQ(wire_zigzag32(wire_unzigzag32(z)), z);
    }
}

TL_TEST(wire_svarint_round_trips_signed_edges, "net,wire,edge,fast") {
    const i32 vals[] = { 0, -1, 1, 127, -128, 16383, -16384,
                         1 << 30, -(1 << 30), 2147483647, (i32)(-2147483647 - 1) };
    u8 buf[64];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    for (u32 i = 0; i < tl_count(vals); ++i) { wire_put_svarint(&w, vals[i]); }

    ByteReader r;
    br_init(&r, buf, w.len);
    for (u32 i = 0; i < tl_count(vals); ++i) {
        i32 got = 0;
        TL_ASSERT_EQ(wire_get_svarint(&r, &got), ERR_OK);
        TL_EXPECT_EQ(got, vals[i]);
    }
    TL_EXPECT_TRUE(br_ok(&r));
    TL_EXPECT_EQ(r.pos, r.len);
}

TL_TEST(wire_uvarint_refuses_overlong_and_oversized, "net,wire,edge,fast") {
    // Five bytes whose last still carries the continuation bit: no sixth byte is legal.
    {
        const u8 six[6] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 };
        ByteReader r;
        br_init(&r, six, sizeof(six));
        u32 got = 0xDEADBEEFu;
        TL_EXPECT_EQ(wire_get_uvarint(&r, &got), ERR_NET_VARINT_OVERFLOW);
        TL_EXPECT_EQ(got, 0u);   // a refused read leaves nothing behind
    }
    // A 5th byte with bits above 2^32: representable as bytes, not as a u32.
    {
        const u8 big[5] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x10 };
        ByteReader r;
        br_init(&r, big, sizeof(big));
        u32 got = 0xDEADBEEFu;
        TL_EXPECT_EQ(wire_get_uvarint(&r, &got), ERR_NET_VARINT_OVERFLOW);
        TL_EXPECT_EQ(got, 0u);
    }
    // 0x0F in the 5th byte is the largest legal one - the boundary the check must not overshoot.
    {
        const u8 max[5] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F };
        ByteReader r;
        br_init(&r, max, sizeof(max));
        u32 got = 0u;
        TL_EXPECT_EQ(wire_get_uvarint(&r, &got), ERR_OK);
        TL_EXPECT_EQ(got, 0xFFFFFFFFu);
    }
}

TL_TEST(wire_uvarint_truncation_is_data_not_a_bug, "net,wire,edge,fast") {
    // A continuation byte with nothing after it: the byte pair's sticky truncation code, and
    // the varint reader must surface it rather than inventing a value.
    const u8 cut[1] = { 0x80 };
    ByteReader r;
    br_init(&r, cut, sizeof(cut));
    u32 got = 0xDEADBEEFu;
    TL_EXPECT_EQ(wire_get_uvarint(&r, &got), ERR_BYTES_TRUNCATED);
    TL_EXPECT_EQ(got, 0u);
    TL_EXPECT_FALSE(br_ok(&r));

    // An empty buffer is the same condition, not a zero.
    ByteReader r2;
    br_init(&r2, cut, 0);
    i32 s = 7;
    TL_EXPECT_EQ(wire_get_svarint(&r2, &s), ERR_BYTES_TRUNCATED);
    TL_EXPECT_EQ(s, 0);
}

TL_TEST(wire_version_policy_refuses_only_newer, "net,wire,fast") {
    TL_EXPECT_EQ(wire_check_version(0u), ERR_OK);                       // older build's stream
    TL_EXPECT_EQ(wire_check_version(NET_FORMAT_VERSION), ERR_OK);
    TL_EXPECT_EQ(wire_check_version(NET_FORMAT_VERSION + 1u), ERR_NET_VERSION);
    TL_EXPECT_EQ(wire_check_version(0xFFFFFFFFu), ERR_NET_VERSION);
}

TL_TEST(wire_frame_mirror_matches_the_input_doc_geometry, "net,wire,smoke,fast") {
    // The mirror's whole justification is that it cannot drift from docs/INPUT.md §1 in silence.
    // These restate the doc's numbers at the one place the restatement is the point; the
    // static_asserts in wire.h fail the BUILD, this fails the SUITE, and the W3 handoff replaces
    // both with a comparison against the real core/input.h.
    TL_EXPECT_EQ(sizeof(WireFrame), (u64)76u);
    TL_EXPECT_EQ(NET_FRAME_MAX_ACTIONS, 32u);
    TL_EXPECT_EQ(sizeof(NetActionState), (u64)2u);
    TL_EXPECT_EQ((u64)offsetof(WireFrame, actions), (u64)0u);
    TL_EXPECT_EQ((u64)offsetof(WireFrame, pointer_x), (u64)64u);
    TL_EXPECT_EQ((u64)offsetof(WireFrame, pointer_y), (u64)68u);
    TL_EXPECT_EQ((u64)offsetof(WireFrame, tick), (u64)72u);

    // ZERO_FRAME is every action {0,0} and the pointer at the origin (docs/NETCODE.md §20.2.2).
    const WireFrame z = wire_zero_frame();
    TL_EXPECT_EQ(z.pointer_x, 0);
    TL_EXPECT_EQ(z.pointer_y, 0);
    TL_EXPECT_EQ(z.tick, 0u);
    for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
        TL_EXPECT_EQ(z.actions[a].value, (i8)0);
        TL_EXPECT_EQ(z.actions[a].flags, (u8)0);
    }
}

TL_TEST(wire_wrap_add_is_defined_at_the_i32_edges, "net,wire,edge,fast") {
    // The decoder's arithmetic is wrap_add on i32 (docs/NETCODE.md §20.2.2): it must come back
    // bit-identical at the edges rather than trap under UBSan, which is what -fsanitize
    // signed-integer-overflow would do to a plain `a + b`.
    const i32 imax = 2147483647;
    const i32 imin = (i32)(-2147483647 - 1);
    TL_EXPECT_EQ(wire_wrap_add_i32(imax, 1), imin);
    TL_EXPECT_EQ(wire_wrap_add_i32(imin, -1), imax);
    TL_EXPECT_EQ(wire_wrap_sub_i32(imin, 1), imax);
    TL_EXPECT_EQ(wire_wrap_add_i32(0, 0), 0);
    // add/sub are exact inverses over a spread of magnitudes, including the wrapping ones.
    for (u32 i = 0; i < 256u; ++i) {
        const i32 a = (i32)(u32)nt_mix64(0xA11Cu, i);
        const i32 b = (i32)(u32)nt_mix64(0xB22Du, i);
        TL_EXPECT_EQ(wire_wrap_sub_i32(wire_wrap_add_i32(a, b), b), a);
    }
}
