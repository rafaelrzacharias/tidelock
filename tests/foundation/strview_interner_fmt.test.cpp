// strview.h / interner.h / fmt.h - intern idempotence, collision fatal-expected with a crafted
// pair, fmt_buf truncation. Spec: docs/CONTAINERS.md §5, §8.6, §8.7. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/strview.h"
#include "foundation/interner.h"
#include "foundation/fmt.h"
#include "foundation/vmem_test_api.h"

TL_TEST(strview_eq_hash_starts_with_split, "foundation,containers,smoke,fast") {
    StrView a = sv_lit("hello");
    StrView b = sv("hello");
    StrView c = sv_lit("world");
    TL_EXPECT_TRUE(sv_eq(a, b));
    TL_EXPECT_FALSE(sv_eq(a, c));
    TL_EXPECT_EQ(sv_hash(a), sv_hash(b));       // pure function of bytes
    TL_EXPECT_NE(sv_hash(a), sv_hash(c));
    TL_EXPECT_TRUE(sv_starts_with(sv_lit("hello world"), sv_lit("hello")));
    TL_EXPECT_FALSE(sv_starts_with(sv_lit("hi"), sv_lit("hello")));

    StrView lo, hi;
    sv_split_at(sv_lit("hello world"), 5u, &lo, &hi);
    TL_EXPECT_TRUE(sv_eq(lo, sv_lit("hello")));
    TL_EXPECT_TRUE(sv_eq(hi, sv_lit(" world")));
}

TL_TEST(strview_edge_empty, "foundation,containers,edge,fast") {
    StrView empty = sv_lit("");
    TL_EXPECT_EQ(empty.len, (u32)0);
    TL_EXPECT_TRUE(sv_eq(empty, sv("")));
    TL_EXPECT_TRUE(sv_starts_with(sv_lit("anything"), empty));
    StrView lo, hi;
    sv_split_at(sv_lit("abc"), 0u, &lo, &hi);
    TL_EXPECT_EQ(lo.len, (u32)0);
    TL_EXPECT_TRUE(sv_eq(hi, sv_lit("abc")));
    sv_split_at(sv_lit("abc"), 3u, &lo, &hi);
    TL_EXPECT_TRUE(sv_eq(lo, sv_lit("abc")));
    TL_EXPECT_EQ(hi.len, (u32)0);
}

static Interner make_test_interner(VMemApi* api, VMemArena* chars, VMemArena* meta) {
    // A plain helper, not a TL_TEST body - no TestCtx* t in scope, so TL_CHECK (not TL_ASSERT_EQ).
    TL_CHECK(vmem_arena_init(chars, "test.interner_chars"_id, 1ull * 1024 * 1024, 0, api) == ERR_OK);
    TL_CHECK(vmem_arena_init(meta, "test.interner_meta"_id, 4ull * 1024 * 1024, 0, api) == ERR_OK);
    Interner in;
    interner_init(&in, chars, meta, 256u);
    return in;
}

TL_TEST(interner_intern_is_idempotent, "foundation,containers,smoke,fast") {
    VMemApi api = test_vmem_api();
    VMemArena chars = {}, meta = {};
    Interner in = make_test_interner(&api, &chars, &meta);

    StrId id1 = intern(&in, sv_lit("player"));
    StrId id2 = intern(&in, sv_lit("player"));   // same content -> same id, count unchanged
    TL_EXPECT_EQ(id1, id2);
    TL_EXPECT_EQ(in.count, (u32)1);

    StrId id3 = intern(&in, sv_lit("enemy"));
    TL_EXPECT_NE(id1, id3);
    TL_EXPECT_EQ(in.count, (u32)2);

    TL_EXPECT_TRUE(sv_eq(intern_name(&in, id1), sv_lit("player")));
    TL_EXPECT_TRUE(sv_eq(intern_name(&in, id3), sv_lit("enemy")));
    TL_EXPECT_EQ(intern_hash(&in, id1), sv_hash(sv_lit("player")));
}

TL_TEST(interner_edge_empty_string, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    VMemArena chars = {}, meta = {};
    Interner in = make_test_interner(&api, &chars, &meta);
    StrId id = intern(&in, sv_lit(""));
    TL_EXPECT_TRUE(sv_eq(intern_name(&in, id), sv_lit("")));
    TL_EXPECT_EQ(intern(&in, sv_lit("")), id);   // idempotent for the empty string too
}

// A crafted pair, not a genuine 64-bit FNV-1a collision (computationally infeasible to brute-force
// in a unit test - docs/CONTAINERS.md §8.7 calls for exactly this shape). We forge the by_hash
// entry directly so intern()'s collision-check path executes on a hash that legitimately maps to
// two different byte sequences, which is the only observable effect a real collision would have.
TL_TEST_EXPECT_FATAL(interner_collision_with_crafted_pair_is_fatal, "foundation,containers,fatal") {
    VMemApi api = test_vmem_api();
    VMemArena chars = {}, meta = {};
    Interner in = make_test_interner(&api, &chars, &meta);
    StrId id_foo = intern(&in, sv_lit("foo"));
    TL_EXPECT_EQ(id_foo, (StrId)0);
    // Force "bar"'s hash to (falsely) resolve to id_foo, which stores "foo"'s bytes.
    map_put(&in.by_hash, sv_hash(sv_lit("bar")), id_foo);
    intern(&in, sv_lit("bar"));   // by_hash says id_foo, stored bytes are "foo" != "bar": TL_FATAL
}

TL_TEST(fmt_buf_truncation, "foundation,containers,fmt") {
    // fmt.h STUB - blocked on vendor/stb_sprintf (W1 platform lane, not yet landed). See fmt.h's
    // contract block. Replace this with a real truncation test the day the vendor tree lands.
    TL_SKIP("fmt_buf is a TL_FATAL stub pending vendor/stb_sprintf (W1 platform lane) - see fmt.h");
}
