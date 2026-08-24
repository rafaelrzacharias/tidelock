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

// "process-stable for the run" (docs/CANON.md "Types") has an operational meaning nothing tested:
// the same intern ORDER produces the same ids, in two independent interners, and ids are dense
// 0..count-1 so nothing about arena addresses or hash bucket order leaks into them.
TL_TEST(interner_two_instances_same_order_same_ids, "foundation,containers,determinism,fast") {
    VMemApi api = test_vmem_api();
    VMemArena chars_a = {}, meta_a = {}, chars_b = {}, meta_b = {};
    Interner a = make_test_interner(&api, &chars_a, &meta_a);
    Interner b = make_test_interner(&api, &chars_b, &meta_b);

    const char* names[8] = { "position", "velocity", "", "health", "position", "sprite", "health", "z" };
    StrId ids_a[8], ids_b[8];
    for (u32 i = 0; i < 8u; ++i) { ids_a[i] = intern(&a, sv(names[i])); }
    for (u32 i = 0; i < 8u; ++i) { ids_b[i] = intern(&b, sv(names[i])); }

    TL_EXPECT_SPAN_EQ(ids_a, ids_b, 8);
    TL_EXPECT_EQ(a.count, b.count);
    TL_EXPECT_EQ(a.count, (u32)6);                     // "position" and "health" repeat
    // Dense and issued in first-intern order: 0..count-1, never a hash-derived value.
    const StrId expect[8] = { 0u, 1u, 2u, 3u, 0u, 4u, 3u, 5u };
    TL_EXPECT_SPAN_EQ(ids_a, expect, 8);
    for (u32 i = 0; i < 8u; ++i) {
        TL_EXPECT_TRUE(sv_eq(intern_name(&a, ids_a[i]), sv(names[i])));
        TL_EXPECT_TRUE(sv_eq(intern_name(&b, ids_b[i]), sv(names[i])));
    }
}

// The empty string is a real row, not a degenerate one: it is interned at offset == the current
// chars mark and pushes zero bytes, so the NEXT string starts at the same offset. Both must still
// read back correctly, and re-interning either must be idempotent.
TL_TEST(interner_empty_string_shares_an_offset_with_its_successor, "foundation,containers,edge,fast") {
    VMemApi api = test_vmem_api();
    VMemArena chars = {}, meta = {};
    Interner in = make_test_interner(&api, &chars, &meta);
    StrId e = intern(&in, sv_lit(""));
    StrId n = intern(&in, sv_lit("next"));
    TL_EXPECT_EQ(in.offsets.data[e], in.offsets.data[n]);   // zero-length push advances nothing
    TL_EXPECT_EQ(in.lens.data[e], (u16)0);
    TL_EXPECT_EQ(in.lens.data[n], (u16)4);
    TL_EXPECT_TRUE(sv_eq(intern_name(&in, e), sv_lit("")));
    TL_EXPECT_TRUE(sv_eq(intern_name(&in, n), sv_lit("next")));
    TL_EXPECT_EQ(intern(&in, sv_lit("")), e);
    TL_EXPECT_EQ(intern(&in, sv_lit("next")), n);
    TL_EXPECT_EQ(in.count, (u32)2);
}

// Ids stay valid and stable while the interner fills toward its capacity - the reverse table grows
// by array_push, and nothing may renumber an id already handed out.
TL_TEST(interner_ids_are_stable_as_the_table_fills, "foundation,containers,determinism,fast") {
    VMemApi api = test_vmem_api();
    VMemArena chars = {}, meta = {};
    Interner in = make_test_interner(&api, &chars, &meta);
    StrId first[16];
    char buf[16];
    for (u32 i = 0; i < 16u; ++i) {
        buf[0] = 'k'; buf[1] = (char)('a' + (i / 10u)); buf[2] = (char)('0' + (i % 10u)); buf[3] = '\0';
        first[i] = intern(&in, sv(buf));
        TL_ASSERT_EQ(first[i], (StrId)i);
    }
    for (u32 i = 0; i < 200u; ++i) {   // fill well past the first 16
        buf[0] = 'f'; buf[1] = (char)('a' + (i / 26u)); buf[2] = (char)('a' + (i % 26u)); buf[3] = '\0';
        intern(&in, sv(buf));
    }
    for (u32 i = 0; i < 16u; ++i) {
        buf[0] = 'k'; buf[1] = (char)('a' + (i / 10u)); buf[2] = (char)('0' + (i % 10u)); buf[3] = '\0';
        TL_EXPECT_EQ(intern(&in, sv(buf)), first[i]);                    // same id, still
        TL_EXPECT_TRUE(sv_eq(intern_name(&in, first[i]), sv(buf)));       // still the same bytes
    }
}

// lens[] is u16, so a string longer than 65535 bytes would silently truncate. That bound is
// caller-input validation and is TL_CHECK in every tier (W1 containers review 2) - it was a
// TL_ASSERT, which is compiled out in netcode/ship exactly where the corruption would be silent.
TL_TEST_EXPECT_FATAL(interner_over_long_string_is_fatal_in_every_tier, "foundation,containers,fatal") {
    VMemApi api = test_vmem_api();
    VMemArena chars = {}, meta = {};
    Interner in = make_test_interner(&api, &chars, &meta);
    char big[4];
    big[0] = 'x'; big[1] = '\0';
    TL_EXPECT_EQ(intern(&in, sv_lit("ok")), (StrId)0);   // the interner works before the bad call
    StrView too_long = StrView{ big, 0x10000u };   // len only - the bytes are never read before the check
    intern(&in, too_long);
}
