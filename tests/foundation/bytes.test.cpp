// bytes.h - the little-endian ByteWriter/ByteReader pair: pinned byte order, round trips,
// underflow-as-data (sticky error), overflow-as-bug (fatal). Spec: docs/NETCODE.md §1 placement,
// docs/CPP-SUBSET.md §9 R-2 (wire structs write through this pair). Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/bytes.h"

TL_TEST(bytes_le_encoding_is_pinned_against_hand_written_bytes, "foundation,bytes,smoke,fast") {
    // The known answer is written by hand from the definition of little-endian, not computed by
    // the code under test (LESSONS: a golden computed by the code it guards is a screenshot).
    u8 buf[15];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    bw_put_u8(&w, 0x01u);
    bw_put_u16(&w, 0x2345u);
    bw_put_u32(&w, 0x6789ABCDu);
    bw_put_u64(&w, 0x0F1E2D3C4B5A6978ull);
    TL_ASSERT_EQ(w.len, 15u);
    const u8 want[15] = { 0x01,
                          0x45, 0x23,
                          0xCD, 0xAB, 0x89, 0x67,
                          0x78, 0x69, 0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F };
    for (u32 i = 0; i < 15u; ++i) { TL_EXPECT_EQ(buf[i], want[i]); }
}

TL_TEST(bytes_round_trip_all_widths_and_edges, "foundation,bytes,fast") {
    // Edge matrix per width: 0, 1, the max, and a mixed pattern; plus a raw-bytes payload.
    u8 buf[128];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    bw_put_u8(&w, 0u);   bw_put_u8(&w, 1u);   bw_put_u8(&w, 0xFFu);
    bw_put_u16(&w, 0u);  bw_put_u16(&w, 1u);  bw_put_u16(&w, 0xFFFFu);
    bw_put_u32(&w, 0u);  bw_put_u32(&w, 1u);  bw_put_u32(&w, 0xFFFFFFFFu);
    bw_put_u64(&w, 0u);  bw_put_u64(&w, 1u);  bw_put_u64(&w, 0xFFFFFFFFFFFFFFFFull);
    const u8 raw[5] = { 9, 8, 7, 6, 5 };
    bw_put_bytes(&w, raw, sizeof(raw));
    // Signed values cross as two's-complement casts (the callers' contract).
    bw_put_u32(&w, (u32)(i32)-2);
    TL_ASSERT_EQ(w.len, (u64)(3u + 6u + 12u + 24u + 5u + 4u));

    ByteReader r;
    br_init(&r, buf, w.len);
    TL_EXPECT_EQ(br_get_u8(&r), 0u);   TL_EXPECT_EQ(br_get_u8(&r), 1u);   TL_EXPECT_EQ(br_get_u8(&r), 0xFFu);
    TL_EXPECT_EQ(br_get_u16(&r), 0u);  TL_EXPECT_EQ(br_get_u16(&r), 1u);  TL_EXPECT_EQ(br_get_u16(&r), 0xFFFFu);
    TL_EXPECT_EQ(br_get_u32(&r), 0u);  TL_EXPECT_EQ(br_get_u32(&r), 1u);  TL_EXPECT_EQ(br_get_u32(&r), 0xFFFFFFFFu);
    TL_EXPECT_EQ(br_get_u64(&r), 0u);  TL_EXPECT_EQ(br_get_u64(&r), 1u);  TL_EXPECT_EQ(br_get_u64(&r), 0xFFFFFFFFFFFFFFFFull);
    u8 raw_back[5] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
    br_get_bytes(&r, raw_back, sizeof(raw_back));
    for (u32 i = 0; i < 5u; ++i) { TL_EXPECT_EQ(raw_back[i], raw[i]); }
    TL_EXPECT_EQ((i32)br_get_u32(&r), -2);
    TL_EXPECT_TRUE(br_ok(&r));
    TL_EXPECT_EQ(r.pos, r.len);
}

TL_TEST(bytes_reader_underflow_is_sticky_and_returns_zero, "foundation,bytes,edge,fast") {
    // One case per width: a buffer one byte short of the read. The failed read returns 0, sets
    // the code, and every LATER read - even one that would fit - stays 0 with the code held.
    const u8 three[3] = { 0x11, 0x22, 0x33 };
    ByteReader r;

    br_init(&r, three, 0);
    TL_EXPECT_EQ(br_get_u8(&r), 0u);
    TL_EXPECT_EQ(r.err, ERR_BYTES_TRUNCATED);

    br_init(&r, three, 1);
    TL_EXPECT_EQ(br_get_u16(&r), 0u);
    TL_EXPECT_EQ(r.err, ERR_BYTES_TRUNCATED);

    br_init(&r, three, 3);
    TL_EXPECT_EQ(br_get_u32(&r), 0u);
    TL_EXPECT_EQ(r.err, ERR_BYTES_TRUNCATED);

    br_init(&r, three, 3);
    TL_EXPECT_EQ(br_get_u64(&r), 0u);
    TL_EXPECT_EQ(r.err, ERR_BYTES_TRUNCATED);

    // Sticky: the in-range u8 at pos 0 is refused after a failure, and pos does not move.
    br_init(&r, three, 3);
    (void)br_get_u64(&r);
    TL_EXPECT_EQ(br_get_u8(&r), 0u);
    TL_EXPECT_EQ(r.err, ERR_BYTES_TRUNCATED);
    TL_EXPECT_EQ(r.pos, 0u);
    TL_EXPECT_FALSE(br_ok(&r));
}

TL_TEST(bytes_get_bytes_zero_fills_and_rejects_hostile_length, "foundation,bytes,edge,fast") {
    const u8 three[3] = { 0x11, 0x22, 0x33 };
    ByteReader r;
    br_init(&r, three, 3);
    u8 out[8];
    for (u32 i = 0; i < 8u; ++i) { out[i] = 0xAAu; }
    br_get_bytes(&r, out, 8);   // longer than the input: refused, out zero-filled, nothing stale
    TL_EXPECT_EQ(r.err, ERR_BYTES_TRUNCATED);
    for (u32 i = 0; i < 8u; ++i) { TL_EXPECT_EQ(out[i], 0u); }

    // A length large enough to wrap pos + n must be refused by the subtraction spelling, not
    // slip past a wrapped sum (the decoded-length-field attack).
    br_init(&r, three, 3);
    r.pos = 2;
    u8 one = 0xAAu;
    br_get_bytes(&r, &one, 0xFFFFFFFFFFFFFFFFull);
    TL_EXPECT_EQ(r.err, ERR_BYTES_TRUNCATED);
    TL_EXPECT_EQ(one, 0u);

    // n == 0 is a no-op in every state, including after a failure.
    br_init(&r, three, 3);
    br_get_bytes(&r, nullptr, 0);
    TL_EXPECT_TRUE(br_ok(&r));
    TL_EXPECT_EQ(r.pos, 0u);
    ++t->checks;   // the two calls above assert by not faulting; count them as exercised
}

TL_TEST(bytes_writer_exact_fit_and_empty, "foundation,bytes,edge,fast") {
    // Exact fit: cap == bytes written, no fatal; the empty writer/reader pair is legal.
    u8 buf[8];
    ByteWriter w;
    bw_init(&w, buf, 8);
    bw_put_u64(&w, 0x123456789ABCDEF0ull);
    TL_EXPECT_EQ(w.len, 8u);

    ByteWriter w0;
    bw_init(&w0, nullptr, 0);
    bw_put_bytes(&w0, nullptr, 0);
    TL_EXPECT_EQ(w0.len, 0u);

    ByteReader r0;
    br_init(&r0, nullptr, 0);
    TL_EXPECT_TRUE(br_ok(&r0));
}

TL_TEST_EXPECT_FATAL(bytes_writer_overflow_is_fatal, "foundation,bytes,fatal") {
    // A producer that blows its own buffer is a bug in every tier (TL_CHECK, not TL_ASSERT).
    u8 buf[2];
    ByteWriter w;
    bw_init(&w, buf, 2);
    bw_put_u16(&w, 0xBEEFu);
    ++t->checks;
    bw_put_u8(&w, 1u);   // one past cap - must fatal on every tier
}
