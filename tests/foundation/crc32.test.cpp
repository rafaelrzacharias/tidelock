// crc32.h - the project's table-driven CRC-32/ISO-HDLC. Spec: docs/NETCODE.md §1 (placement),
// §13.3 (own crc32 per block), §20.2.8/§20.2.9 (the header and payload fields that carry it).
// Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/crc32.h"

TL_TEST(crc32_matches_the_variants_published_check_value, "foundation,crc32,smoke,fast") {
    // The check constant of CRC-32/ISO-HDLC: the nine ASCII bytes "123456789" -> 0xCBF43926.
    // Published with the algorithm, not computed by the code under test - a golden computed by
    // the code it guards is a screenshot (LESSONS).
    const char* s = "123456789";
    TL_EXPECT_EQ(crc32(s, 9u), CRC32_CHECK_VALUE);
    TL_EXPECT_EQ(CRC32_CHECK_VALUE, 0xCBF43926u);

    // A second independent vector: the empty input is the init value closed, i.e. 0.
    TL_EXPECT_EQ(crc32(nullptr, 0u), 0u);
    // And one more published pair: a single 'a' is 0xE8B7BE43.
    const char a = 'a';
    TL_EXPECT_EQ(crc32(&a, 1u), 0xE8B7BE43u);
}

TL_TEST(crc32_table_is_the_reflected_polynomial, "foundation,crc32,fast") {
    // Row 1 of a reflected CRC-32 table is the polynomial itself shifted once; row 0 is 0.
    TL_EXPECT_EQ(CRC32_TABLE.v[0], 0u);
    TL_EXPECT_EQ(CRC32_TABLE.v[1], 0x77073096u);
    TL_EXPECT_EQ(CRC32_TABLE.v[255], 0x2D02EF8Du);
    TL_EXPECT_EQ(CRC32_POLY_REFLECTED, 0xEDB88320u);
    // Every row is the byte folded 8 times - checked against the generator, independently of
    // how the table was built.
    for (u32 i = 0; i < 256u; ++i) { TL_ASSERT_EQ(CRC32_TABLE.v[i], crc32_table_entry(i)); }
}

TL_TEST(crc32_streaming_equals_one_shot, "foundation,crc32,fast") {
    // The split-update path is what a header-then-payload checksum uses; it must agree with the
    // contiguous one at every split point, including both ends.
    u8 buf[257];
    for (u32 i = 0; i < sizeof(buf); ++i) { buf[i] = (u8)(i * 31u + 7u); }
    const u32 whole = crc32(buf, sizeof(buf));
    for (u64 split = 0; split <= sizeof(buf); ++split) {
        u32 c = crc32_update(CRC32_INIT, buf, split);
        c = crc32_update(c, buf + split, sizeof(buf) - split);
        TL_ASSERT_EQ(crc32_final(c), whole);
    }
}

TL_TEST(crc32_detects_every_single_bit_flip, "foundation,crc32,edge,fast") {
    // The property the archive actually relies on (docs/NETCODE.md §20.6 T2): a corrupted block
    // must not check out. Every single-bit flip over a realistic block size.
    u8 buf[64];
    for (u32 i = 0; i < sizeof(buf); ++i) { buf[i] = (u8)(i ^ 0xA5u); }
    const u32 good = crc32(buf, sizeof(buf));
    for (u32 i = 0; i < sizeof(buf); ++i) {
        for (u32 b = 0; b < 8u; ++b) {
            buf[i] = (u8)(buf[i] ^ (1u << b));
            TL_ASSERT_NE(crc32(buf, sizeof(buf)), good);
            buf[i] = (u8)(buf[i] ^ (1u << b));   // restore
        }
    }
    TL_EXPECT_EQ(crc32(buf, sizeof(buf)), good);   // fully restored
}

TL_TEST(crc32_is_length_sensitive, "foundation,crc32,edge,fast") {
    // Appended zero bytes must change the CRC - a checksum that ignored trailing zeros would
    // pass a truncated-then-zero-padded block, which is exactly the torn-write shape.
    const u8 zeros[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    u32 prev = crc32(zeros, 0u);
    for (u64 n = 1; n <= sizeof(zeros); ++n) {
        const u32 c = crc32(zeros, n);
        TL_ASSERT_NE(c, prev);
        prev = c;
    }
}
