// handle.h - make/index/gen round trips, null semantics, the CANON domain shapes.
// Spec: docs/MEMORY.md §3, §8.5, §8.7 test list. Rubric: docs/TESTING.md §7.
// Deferred to the runner lane's TL_TEST_EXPECT_FATAL: handle_make with gen 0, gen > GEN_MAX,
// idx > IDX_MASK - each is a dev assert (generation 0 is never issued by contract).
#include "runner/tl_test.h"
#include "foundation/handle.h"

struct TestTagA;
struct TestTagB;
using H32 = Handle<TestTagA, 22, 10>;   // the Entity shape (docs/CANON.md)
using H16 = Handle<TestTagB, 12, 4>;    // the resource shape

// Two tags over one geometry are distinct types - a cross-domain assignment must not compile.
// (Compile-time fact; recorded here as the contract's negative space.)
static_assert(!__is_same(H32, Handle<TestTagB, 22, 10>), "tags separate domains");

TL_TEST(handle_make_index_gen_round_trip, "foundation,mem,smoke,fast") {
    // Edge matrix over the H32 geometry: min/max index x min/max generation.
    const u32 idx_edges[4] = { 0u, 1u, H32::IDX_MASK - 1u, H32::IDX_MASK };
    const u32 gen_edges[4] = { 1u, 2u, H32::GEN_MAX - 1u, H32::GEN_MAX };
    for (u32 i = 0; i < 4u; ++i) {
        for (u32 g = 0; g < 4u; ++g) {
            const H32 h = handle_make<H32>(idx_edges[i], gen_edges[g]);
            TL_EXPECT_EQ(handle_index(h), idx_edges[i]);
            TL_EXPECT_EQ(handle_gen(h), gen_edges[g]);
            TL_EXPECT_FALSE(handle_is_null(h));
        }
    }
    // constexpr round trip: the whole API is usable at compile time.
    constexpr H32 ch = handle_make<H32>(12345u, 7u);
    static_assert(handle_index(ch) == 12345u && handle_gen(ch) == 7u && !handle_is_null(ch), "");
}

TL_TEST(handle_null_semantics, "foundation,mem,smoke,fast") {
    // Zero-initialised memory is never a valid handle (docs/CPP-SUBSET.md section 3).
    const H32 z = {};
    TL_EXPECT_TRUE(handle_is_null(z));
    TL_EXPECT_EQ(handle_gen(z), 0u);          // gen 0 exists only on null
    const H32 lowest = handle_make<H32>(0u, 1u);
    TL_EXPECT_FALSE(handle_is_null(lowest));  // idx 0 with a real gen is NOT null
    TL_EXPECT_TRUE(lowest.bits != 0u);
}

TL_TEST(handle_domain_shapes, "foundation,mem,fast") {
    // The CANON widths, and the bit budget arithmetic behind them.
    TL_EXPECT_EQ(sizeof(H32), (usize)4);
    TL_EXPECT_EQ(sizeof(H16), (usize)2);
    TL_EXPECT_EQ(H32::IDX_MASK, (u32)((1u << 22) - 1u));   // 4M slots
    TL_EXPECT_EQ(H32::GEN_MAX, (u32)1023);                 // 1024 gens (0 unissued -> 1023 usable)
    TL_EXPECT_EQ(H16::IDX_MASK, (u32)4095);
    TL_EXPECT_EQ(H16::GEN_MAX, (u32)15);

    // The 16/16 split some Alloy pools use (docs/MEMORY.md section 3).
    struct TestTagC;
    using H1616 = Handle<TestTagC, 16, 16>;
    TL_EXPECT_EQ(sizeof(H1616), (usize)4);
    const H1616 h = handle_make<H1616>(65535u, 65535u);
    TL_EXPECT_EQ(handle_index(h), 65535u);
    TL_EXPECT_EQ(handle_gen(h), 65535u);

    // Adjacent values do not alias across the idx/gen boundary.
    const H32 a = handle_make<H32>(H32::IDX_MASK, 1u);
    const H32 b = handle_make<H32>(0u, 2u);
    TL_EXPECT_NE(a.bits, b.bits);
    TL_EXPECT_EQ(handle_gen(a), 1u);
    TL_EXPECT_EQ(handle_index(b), 0u);
}
