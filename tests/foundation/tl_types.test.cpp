// tl_types.h - the runtime half of its contract. The width/layout invariants are static_asserts
// in the header itself (they hold in every TU); these cover what only runs.
// Spec: docs/CPP-SUBSET.md §1, §3; docs/CANON.md "Types". Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"

TL_TEST(tl_types_widths_and_wrap, "foundation,smoke,fast") {
    TL_EXPECT_EQ(sizeof(u8), (usize)1);
    TL_EXPECT_EQ(sizeof(u64), (usize)8);
    TL_EXPECT_EQ(sizeof(i32), (usize)4);

    // min/max edges: unsigned wraps, signed is two's complement (docs/CPP-SUBSET.md §5).
    const u8 u8_max = (u8)~(u8)0;
    TL_EXPECT_EQ(u8_max, (u8)255);
    TL_EXPECT_EQ((u8)(u8_max + (u8)1), (u8)0);
    TL_EXPECT_EQ((i32)((u32)0x7fffffffu + 1u), (i32)-2147483647 - 1);
}

TL_TEST(tl_types_uint_fit_boundaries, "foundation,smoke,fast") {
    // 0/1/many + every boundary of the selector table.
    TL_EXPECT_EQ(sizeof(uint_fit<1>), (usize)1);
    TL_EXPECT_EQ(sizeof(uint_fit<8>), (usize)1);
    TL_EXPECT_EQ(sizeof(uint_fit<9>), (usize)2);
    TL_EXPECT_EQ(sizeof(uint_fit<16>), (usize)2);
    TL_EXPECT_EQ(sizeof(uint_fit<17>), (usize)4);
    TL_EXPECT_EQ(sizeof(uint_fit<32>), (usize)4);
    TL_EXPECT_EQ(sizeof(uint_fit<33>), (usize)8);
    TL_EXPECT_EQ(sizeof(uint_fit<64>), (usize)8);

    // The CANON handle rows must fit the widths they claim (docs/CANON.md "Types").
    TL_EXPECT_EQ(sizeof(uint_fit<22 + 10>), (usize)4);   // Entity
    TL_EXPECT_EQ(sizeof(uint_fit<12 + 4>), (usize)2);    // resource handles
}

TL_TEST(tl_types_result_shape, "foundation,smoke,fast") {
    Result<u32> ok = { 7u, ERR_OK };
    TL_EXPECT_EQ(ok.err, ERR_OK);
    TL_EXPECT_EQ(ok.value, 7u);

    // Failure path: `value` is undefined and never read; only `err` is meaningful. A module
    // spells its own codes in its own range over the shared width (docs/CPP-SUBSET.md §3).
    const ErrCode some_module_error = (ErrCode)0x0101;
    Result<u32> bad = { 0u, some_module_error };
    TL_EXPECT_NE(bad.err, ERR_OK);
    TL_EXPECT_EQ(sizeof(bad), (usize)8);
    TL_EXPECT_EQ(sizeof(ErrCode), (usize)2);

    // The whole point of ErrCode being an enum: the attribute rides on the type, so a discarded
    // ErrCode is a compile error under -Werror (docs/CPP-SUBSET.md §9 R-1). Compile-time only -
    // the negative case lives in tools/audit, not here.
    TL_EXPECT_TRUE(__is_enum(ErrCode));
}
