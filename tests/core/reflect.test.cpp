// reflect.h - the one-field-list-three-doors macros: table generation, token-keyed kinds
// (TODO.md E-1: pos_t vs invmass_t are ONE C++ type and must still get DIFFERENT kinds), the
// wire door's LE round trip, pad refusal, truncation, and the reflection hash.
// Spec: docs/ECS.md §6, §10.2, §10.8; docs/CPP-SUBSET.md §9 R-2. Rubric: docs/TESTING.md §7.
// The encoder-round-trip half of §10.8's reflect line lands with encoder.h (its own commit);
// the padding-assert compile-fail negative lands in tools/audit/selftest.py (the negatives lane).
#include "runner/tl_test.h"
#include "core/reflect.h"
#include "foundation/interner.h"   // StrId - the interned-name id (docs/CONTAINERS.md §8.6a)

// A gameplay-shaped component: scalars, a handle, a fixed array; no padding needed (16 B).
#define TL_FIELDS_TestHealth(X, XA, XH) \
    X(i32, hp) X(i32, hp_max) XH(Entity, last_attacker) XA(u8, flags, 4)
TL_COMPONENT(TestHealth)

// The same layout with one field renamed - only the name (and so name_hash) differs; the
// reflection hash must see it (a rename IS a schema change for the save decoder).
#define TL_FIELDS_TestHealth2(X, XA, XH) \
    X(i32, hp) X(i32, hp_points_max) XH(Entity, last_attacker) XA(u8, flags, 4)
TL_COMPONENT(TestHealth2)

// Explicit interior padding: the sum-of-sizes assert compiles only because _pad0 is a field.
#define TL_FIELDS_TestPadded(X, XA, XH) \
    X(u8, a) XA(u8, _pad0, 3) X(u32, b)
TL_COMPONENT(TestPadded)

// Every kind that has a C++ type today, in natural-alignment order - 80 B, zero implicit pads.
// The four fx<i32,30> rows and the two fx<i32,18> rows are ONE type per format (RR-5); their
// kinds must still come out distinct because the TOKEN carries the row.
#define TL_FIELDS_TestAllKinds(X, XA, XH)                                            \
    X(i64, k_i64) X(u64, k_u64) X(i32, k_i32) X(u32, k_u32)                          \
    X(pos_t, k_pos) X(vel_t, k_vel) X(invmass_t, k_invmass) X(stiff_t, k_stiff)      \
    X(q_t, k_q) X(angle_t, k_angle) X(omega_t, k_omega) X(dt_t, k_dt)                \
    X(scalar_t, k_scalar) X(lambda_t, k_lambda) XH(Entity, k_entity)                 \
    X(i16, k_i16) X(u16, k_u16) X(StrId, k_strid) X(i8, k_i8) X(u8, k_u8)            \
    X(bool, k_bool) XA(u8, k_arr, 3)
TL_COMPONENT(TestAllKinds)

// A pool row: same table machinery, no typed-API hook (that absence is compile-time negative
// space; what is testable here is that the table and info exist and agree with the struct).
#define TL_FIELDS_TestPoolRow(X, XA, XH) \
    X(pos_t, x) X(pos_t, y) X(u32, flags)
TL_POOL_ROW(TestPoolRow)

// A wire struct: format_version injected as field 0, offsets pinned, LE pair generated.
#define TL_FIELDS_TestWire(X, XA, XH) \
    X(u16, kind16) XA(u8, _pad0, 2) X(u64, tick) XH(Entity, who) XA(i32, vals, 3)
#define TL_OFFSETS_TestWire(X) X(kind16, 4) X(_pad0, 6) X(tick, 8) X(who, 16) X(vals, 20)
TL_WIRE_STRUCT(TestWire)

static_assert(sizeof(TestHealth) == 16 && sizeof(TestPadded) == 8, "test layout premise");
static_assert(sizeof(TestAllKinds) == 80, "every kind, naturally aligned, no implicit pad");
static_assert(sizeof(TestWire) == 32, "wire layout premise");
// Token-keyed kinds: one C++ type per format, one KIND per spelled row (TODO.md E-1).
static_assert(TestAllKinds_fields[4].kind == K_pos && TestAllKinds_fields[6].kind == K_invmass,
              "fx<i32,18> spelled pos_t and invmass_t must carry different kinds");
static_assert(TestAllKinds_fields[7].kind == K_stiff && TestAllKinds_fields[8].kind == K_q
           && TestAllKinds_fields[9].kind == K_angle && TestAllKinds_fields[11].kind == K_dt,
              "the four fx<i32,30> spellings each keep their own kind");
static_assert(TestAllKinds_fields[13].kind == K_scalar, "lambda_t is scalar_t's row (RR-5)");
static_assert(TL_WIRE_FV_ROW.name_hash == fnv1a64("format_version", 14), "wire row 0 premise");

TL_TEST(reflect_field_table_matches_the_struct, "core,ecs,reflect,smoke,fast") {
    // Offsets/sizes/counts against the compiler's own layout, plus name-hash spelling.
    TL_ASSERT_EQ(TestHealth_info.field_count, 4u);
    TL_EXPECT_EQ(TestHealth_info.size, (u32)sizeof(TestHealth));
    TL_EXPECT_EQ(TestHealth_info.align, (u32)alignof(TestHealth));
    TL_EXPECT_EQ(TestHealth_info.name_hash, "TestHealth"_id);
    const FieldInfo* f = TestHealth_fields;
    TL_EXPECT_EQ(f[0].name_hash, "hp"_id);
    TL_EXPECT_EQ(f[0].offset, (u32)offsetof(TestHealth, hp));
    TL_EXPECT_EQ(f[2].name_hash, "last_attacker"_id);
    TL_EXPECT_EQ(f[2].kind, K_Entity);
    TL_EXPECT_EQ(f[2].offset, (u32)offsetof(TestHealth, last_attacker));
    TL_EXPECT_EQ(f[3].count, 4u);
    TL_EXPECT_EQ(f[3].size, 4u);
    TL_EXPECT_EQ(f[3].offset, (u32)offsetof(TestHealth, flags));
    // The typed-API hook resolves the same info the macro emitted.
    TL_EXPECT_TRUE(tl_info_of((const TestHealth*)nullptr) == &TestHealth_info);
    // Padding-as-fields: the explicit _pad0 is an ordinary row.
    TL_EXPECT_EQ(TestPadded_info.field_count, 3u);
    TL_EXPECT_EQ(TestPadded_fields[1].count, 3u);
    TL_EXPECT_TRUE(tl_field_is_pad(&TestPadded_fields[1]));
    TL_EXPECT_FALSE(tl_field_is_pad(&TestPadded_fields[0]));
    // Pool-row door: same table shape.
    TL_EXPECT_EQ(TestPoolRow_info.field_count, 3u);
    TL_EXPECT_EQ(TestPoolRow_fields[1].kind, K_pos);
}

TL_TEST(reflect_every_kind_size_agrees_with_the_kind_table, "core,ecs,reflect,fast") {
    // size == kind_scalar_size(kind) * count for every field of the all-kinds struct, so the
    // kind table can never drift from the C++ types it describes (StrId's u16 included).
    for (u32 i = 0; i < TestAllKinds_info.field_count; ++i) {
        const FieldInfo* f = &TestAllKinds_fields[i];
        TL_EXPECT_EQ(f->size, kind_scalar_size(f->kind) * f->count);
    }
    TL_EXPECT_EQ(kind_scalar_size(K_StrId), (u32)sizeof(StrId));
    TL_EXPECT_EQ(kind_scalar_size(K_Entity), (u32)sizeof(Entity));
    // Offsets are the compiler's: field i starts where the previous ended (no implicit gaps).
    u32 expect_off = 0;
    for (u32 i = 0; i < TestAllKinds_info.field_count; ++i) {
        TL_EXPECT_EQ(TestAllKinds_fields[i].offset, expect_off);
        expect_off += TestAllKinds_fields[i].size;
    }
    TL_EXPECT_EQ(expect_off, (u32)sizeof(TestAllKinds));
}

TL_TEST(reflect_wire_round_trip_and_pinned_le_layout, "core,ecs,reflect,wire,fast") {
    TestWire s;
    memset(&s, 0, sizeof(s));
    s.format_version = 1;
    s.kind16 = 0xBEEFu;
    s.tick = 0x1122334455667788ull;
    s.who = handle_make<Entity>(7u, 3u);
    s.vals[0] = -1; s.vals[1] = 0; s.vals[2] = 0x7FFFFFFF;

    u8 buf[64];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    wire_write_TestWire(&w, &s);
    TL_ASSERT_EQ(w.len, (u64)sizeof(TestWire));   // every byte accounted for, none extra

    // Stream layout is the field-list order, little-endian: format_version's 4 bytes, then
    // kind16's low byte at stream offset 4, the pad bytes, tick's low byte at offset 8.
    TL_EXPECT_EQ(buf[0], 1u);
    TL_EXPECT_EQ(buf[4], 0xEFu);
    TL_EXPECT_EQ(buf[5], 0xBEu);
    TL_EXPECT_EQ(buf[6], 0u);
    TL_EXPECT_EQ(buf[8], 0x88u);
    TL_EXPECT_EQ(buf[15], 0x11u);

    TestWire back;
    ByteReader r;
    br_init(&r, buf, w.len);
    TL_ASSERT_EQ(wire_read_TestWire(&r, &back), ERR_OK);
    TL_EXPECT_TRUE(memcmp(&s, &back, sizeof(TestWire)) == 0);
    TL_EXPECT_EQ(r.pos, r.len);
}

TL_TEST(reflect_wire_reader_refuses_truncation_and_nonzero_pads, "core,ecs,reflect,wire,edge,fast") {
    TestWire s;
    memset(&s, 0, sizeof(s));
    s.format_version = 1;
    s.kind16 = 0x0102u;
    s.tick = 42;
    u8 buf[sizeof(TestWire)];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    wire_write_TestWire(&w, &s);

    // Truncated at every interesting boundary: mid-header, mid-field, one byte short.
    const u64 cuts[4] = { 0, 3, 9, sizeof(TestWire) - 1 };
    for (u32 c = 0; c < 4u; ++c) {
        TestWire back;
        memset(&back, 0xAA, sizeof(back));
        ByteReader r;
        br_init(&r, buf, cuts[c]);
        TL_EXPECT_EQ(wire_read_TestWire(&r, &back), ERR_BYTES_TRUNCATED);
        // decoded-or-zero: nothing of the 0xAA prefill survives a failed read.
        const u8* p = (const u8*)&back;
        bool any_aa = false;
        for (u32 i = 0; i < sizeof(back); ++i) { any_aa = any_aa || p[i] == 0xAAu; }
        TL_EXPECT_FALSE(any_aa);
    }

    // A nonzero explicit pad on the wire is refused (docs/NETCODE.md §20.2).
    buf[6] = 0x01u;   // _pad0[0]'s stream position
    TestWire back;
    ByteReader r;
    br_init(&r, buf, sizeof(buf));
    TL_EXPECT_EQ(wire_read_TestWire(&r, &back), ERR_WIRE_PAD_NONZERO);
}

TL_TEST(reflect_component_hash_sees_names_kinds_and_layout, "core,ecs,reflect,determinism,fast") {
    // Stable across calls; different across components; and a FIELD RENAME alone moves it
    // (TestHealth vs TestHealth2 share byte layout, kinds and sizes - only one name differs).
    const u64 h1 = tl_reflect_component_hash(&TestHealth_info);
    const u64 h1b = tl_reflect_component_hash(&TestHealth_info);
    const u64 h2 = tl_reflect_component_hash(&TestHealth2_info);
    const u64 h3 = tl_reflect_component_hash(&TestAllKinds_info);
    TL_EXPECT_EQ(h1, h1b);
    TL_EXPECT_NE(h1, h2);
    TL_EXPECT_NE(h1, h3);
    TL_EXPECT_NE(h2, h3);
}
