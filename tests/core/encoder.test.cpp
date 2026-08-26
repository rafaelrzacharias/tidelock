// encoder.h - the name-keyed payload engine: every kind round-trips (the §10.8 reflect line's
// encoder half), byte stability, rename-via-alias, added-field defaults, dropped-field skip,
// kind/count-change refusal, malformed/overflow rejects, and the ECS column payload.
// Spec: docs/ASSETS-AND-DATA.md §5/§8.4; docs/ECS.md §8, §10.8. Rubric: docs/TESTING.md §7.
#include "world_test_util.h"
#include "core/encoder.h"
#include "foundation/interner.h"   // StrId - the EncAll kind sweep spells one

// Every kind with a C++ type today (the same shape reflect.test.cpp pins for the tables).
#define TL_FIELDS_EncAll(X, XA, XH)                                                  \
    X(i64, e_i64) X(u64, e_u64) X(i32, e_i32) X(u32, e_u32)                          \
    X(pos_t, e_pos) X(vel_t, e_vel) X(invmass_t, e_invmass) X(stiff_t, e_stiff)      \
    X(q_t, e_q) X(angle_t, e_angle) X(omega_t, e_omega) X(dt_t, e_dt)                \
    X(scalar_t, e_scalar) XH(Entity, e_entity)                                       \
    X(i16, e_i16) X(u16, e_u16) X(StrId, e_strid) X(i8, e_i8) X(u8, e_u8)            \
    X(bool, e_bool) XA(u8, e_arr, 3) XA(u8, _pad0, 4)
TL_COMPONENT(EncAll)

// The rename pair: the stored (old) schema spells the field `hp`; the live spells `hp_points`.
#define TL_FIELDS_EncOldHp(X, XA, XH) \
    X(i32, hp) X(u32, tag)
TL_COMPONENT(EncOldHp)
#define TL_FIELDS_EncNewHp(X, XA, XH) \
    X(i32, hp_points) X(u32, tag)
TL_COMPONENT(EncNewHp)

// The dropped/added pair: stored has `legacy`; live drops it and adds `fresh`.
#define TL_FIELDS_EncStoredV1(X, XA, XH) \
    X(u32, keep) XA(u8, _pad0, 4) X(u64, legacy)
TL_COMPONENT(EncStoredV1)
#define TL_FIELDS_EncLiveV2(X, XA, XH) \
    X(u32, keep) X(u32, fresh)
TL_COMPONENT(EncLiveV2)

// The kind-change pair (same name, u32 -> i64) and the count-change pair (u8[2] -> u8[4]).
#define TL_FIELDS_EncKindA(X, XA, XH) \
    X(u32, v) XA(u8, arr, 2) XA(u8, _pad0, 2)
TL_COMPONENT(EncKindA)
#define TL_FIELDS_EncKindB(X, XA, XH) \
    X(i64, v) XA(u8, arr, 2) XA(u8, _pad0, 6)
TL_COMPONENT(EncKindB)
#define TL_FIELDS_EncCountB(X, XA, XH) \
    X(u32, v) XA(u8, arr, 4)
TL_COMPONENT(EncCountB)

namespace {

Span<const FieldAlias> no_aliases() { return Span<const FieldAlias>{ nullptr, 0 }; }

}  // namespace

TL_TEST(encoder_every_kind_round_trips_byte_stable, "core,ecs,encoder,smoke,fast") {
    EncAll rows[2];
    memset(rows, 0, sizeof(rows));
    rows[0].e_i64 = -0x123456789ALL;
    rows[0].e_u64 = 0xFFFFFFFFFFFFFFFFull;
    rows[0].e_i32 = -7;
    rows[0].e_u32 = 0xDEADBEEFu;
    rows[0].e_pos = fx::fx_raw<pos_t>(-123456);
    rows[0].e_vel = fx::fx_raw<vel_t>(999);
    rows[0].e_invmass = fx::fx_raw<invmass_t>(1 << 18);
    rows[0].e_stiff = fx::fx_raw<stiff_t>(-5);
    rows[0].e_q = fx::fx_raw<q_t>(1 << 30 >> 1);
    rows[0].e_angle = fx::fx_raw<angle_t>(-(1 << 29));
    rows[0].e_omega = fx::fx_raw<omega_t>(42);
    rows[0].e_dt = fx::fx_raw<dt_t>(2236962);
    rows[0].e_scalar = fx::fx_raw<scalar_t>(-32768);
    rows[0].e_entity = handle_make<Entity>(77u, 5u);
    rows[0].e_i16 = -32768;
    rows[0].e_u16 = 0xFFFFu;
    rows[0].e_strid = (StrId)3u;
    rows[0].e_i8 = -128;
    rows[0].e_u8 = 255u;
    rows[0].e_bool = true;
    rows[0].e_arr[0] = 1u; rows[0].e_arr[1] = 2u; rows[0].e_arr[2] = 3u;
    rows[1] = rows[0];
    rows[1].e_i32 = 12345;

    u8 buf[1024];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encoder_write_rows(&w, &EncAll_info, rows, 2u);
    // Layout arithmetic: 4 + 22 fields * 16 + 4 + 2 rows * sizeof(EncAll).
    TL_ASSERT_EQ(w.len, 4u + 22u * 16u + 4u + 2u * sizeof(EncAll));

    // Byte stability: a second encode of the same state is byte-identical.
    u8 buf2[1024];
    ByteWriter w2;
    bw_init(&w2, buf2, sizeof(buf2));
    encoder_write_rows(&w2, &EncAll_info, rows, 2u);
    TL_EXPECT_TRUE(w2.len == w.len && memcmp(buf, buf2, (usize)w.len) == 0);

    EncAll back[2];
    memset(back, 0xAA, sizeof(back));
    ByteReader r;
    br_init(&r, buf, w.len);
    Result<u32> res = encoder_read_rows(&r, &EncAll_info, no_aliases(), back, 2u);
    TL_ASSERT_EQ(res.err, ERR_OK);
    TL_ASSERT_EQ(res.value, 2u);
    TL_EXPECT_TRUE(memcmp(back, rows, sizeof(rows)) == 0);
    TL_EXPECT_EQ(r.pos, r.len);
}

TL_TEST(encoder_rename_lands_via_alias, "core,ecs,encoder,fast") {
    EncOldHp old_row = { 55, 0xABCDu };
    u8 buf[256];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encoder_write_rows(&w, &EncOldHp_info, &old_row, 1u);

    // Without the alias, `hp` is a dropped field: tag still lands, hp_points defaults to 0.
    EncNewHp plain;
    ByteReader r;
    br_init(&r, buf, w.len);
    Result<u32> res = encoder_read_rows(&r, &EncNewHp_info, no_aliases(), &plain, 1u);
    TL_ASSERT_EQ(res.err, ERR_OK);
    TL_EXPECT_EQ(plain.hp_points, 0);
    TL_EXPECT_EQ(plain.tag, 0xABCDu);

    // With { "hp" -> "hp_points" } the stored value lands in the renamed field.
    const FieldAlias alias = { "hp"_id, "hp_points"_id };
    EncNewHp renamed;
    br_init(&r, buf, w.len);
    res = encoder_read_rows(&r, &EncNewHp_info, Span<const FieldAlias>{ &alias, 1u }, &renamed, 1u);
    TL_ASSERT_EQ(res.err, ERR_OK);
    TL_EXPECT_EQ(renamed.hp_points, 55);
    TL_EXPECT_EQ(renamed.tag, 0xABCDu);
}

TL_TEST(encoder_dropped_field_skipped_added_field_defaulted, "core,ecs,encoder,fast") {
    // Stored v1 rows carry `legacy` (dropped in v2); live v2 adds `fresh` with a default from
    // a default_row (a live info wearing one - the luacomp path builds these for Luau comps).
    EncStoredV1 v1[2];
    memset(v1, 0, sizeof(v1));
    v1[0].keep = 10u; v1[0].legacy = 0x1111111111111111ull;
    v1[1].keep = 20u; v1[1].legacy = 0x2222222222222222ull;
    u8 buf[512];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encoder_write_rows(&w, &EncStoredV1_info, v1, 2u);

    EncLiveV2 def = { 0u, 777u };
    ComponentInfo live = EncLiveV2_info;
    live.default_row = &def;

    EncLiveV2 back[2];
    memset(back, 0xAA, sizeof(back));
    ByteReader r;
    br_init(&r, buf, w.len);
    Result<u32> res = encoder_read_rows(&r, &live, no_aliases(), back, 2u);
    TL_ASSERT_EQ(res.err, ERR_OK);
    TL_ASSERT_EQ(res.value, 2u);
    TL_EXPECT_EQ(back[0].keep, 10u);
    TL_EXPECT_EQ(back[1].keep, 20u);
    TL_EXPECT_EQ(back[0].fresh, 777u);   // added field: the declared default
    TL_EXPECT_EQ(back[1].fresh, 777u);
    TL_EXPECT_EQ(r.pos, r.len);          // the dropped field's bytes were consumed, not left over
}

TL_TEST(encoder_kind_and_count_changes_are_refused, "core,ecs,encoder,edge,fast") {
    EncKindA a;
    memset(&a, 0, sizeof(a));
    a.v = 9u;
    u8 buf[256];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encoder_write_rows(&w, &EncKindA_info, &a, 1u);

    // Same name `v`, kind u32 -> i64: refused, never coerced.
    EncKindB kb;
    ByteReader r;
    br_init(&r, buf, w.len);
    TL_EXPECT_EQ(encoder_read_rows(&r, &EncKindB_info, no_aliases(), &kb, 1u).err, ERR_ENC_FIELD_KIND);

    // Same name `arr`, count 2 -> 4: refused the same way.
    EncCountB cb;
    br_init(&r, buf, w.len);
    TL_EXPECT_EQ(encoder_read_rows(&r, &EncCountB_info, no_aliases(), &cb, 1u).err, ERR_ENC_FIELD_KIND);
}

TL_TEST(encoder_malformed_truncated_and_overflow_rejects, "core,ecs,encoder,edge,fast") {
    EncOldHp row = { 1, 2u };
    u8 buf[256];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encoder_write_rows(&w, &EncOldHp_info, &row, 1u);

    EncOldHp back;
    ByteReader r;

    // Truncation at every region: field meta, row count, mid-row.
    const u64 cuts[3] = { 2u, 4u + 2u * 16u + 2u, w.len - 2u };
    for (u32 c = 0; c < 3u; ++c) {
        br_init(&r, buf, cuts[c]);
        TL_EXPECT_EQ(encoder_read_rows(&r, &EncOldHp_info, no_aliases(), &back, 1u).err,
                     ERR_BYTES_TRUNCATED);
    }

    // A kind byte outside the closed enum is malformed, not a skip.
    u8 evil[256];
    memcpy(evil, buf, (usize)w.len);
    evil[4u + 8u] = 0xEEu;   // first field's kind byte
    br_init(&r, evil, w.len);
    TL_EXPECT_EQ(encoder_read_rows(&r, &EncOldHp_info, no_aliases(), &back, 1u).err,
                 ERR_ENC_MALFORMED);

    // A size that disagrees with kind * count is malformed.
    memcpy(evil, buf, (usize)w.len);
    evil[4u + 12u] = 0x05u;   // first field's size low byte (4 -> 5)
    br_init(&r, evil, w.len);
    TL_EXPECT_EQ(encoder_read_rows(&r, &EncOldHp_info, no_aliases(), &back, 1u).err,
                 ERR_ENC_MALFORMED);

    // More stored rows than the caller's buffer: refused before anything is written.
    br_init(&r, buf, w.len);
    TL_EXPECT_EQ(encoder_read_rows(&r, &EncOldHp_info, no_aliases(), &back, 0u).err,
                 ERR_ENC_OVERFLOW);
}

TL_TEST(encoder_column_payload_round_trips_rows_and_entities, "core,ecs,encoder,fast") {
    // A real column through the world door, encoded and decoded with its entity list.
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 5u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);
    Entity e0 = world_spawn(&f.w);
    Entity e1 = world_spawn(&f.w);
    WPos p0 = { 3, -4 };
    WPos p1 = { -5, 6 };
    world_add<WPos>(&f.w, e0, p0);
    world_add<WPos>(&f.w, e1, p1);
    world_flush(&f.w);

    u8 buf[512];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    encoder_write_column(&w, &f.w.comps[world_component_id<WPos>(&f.w)]);

    WPos rows[4];
    Entity ents[4];
    ByteReader r;
    br_init(&r, buf, w.len);
    Result<u32> res = encoder_read_column(&r, &WPos_info, no_aliases(), rows, ents, 4u);
    TL_ASSERT_EQ(res.err, ERR_OK);
    TL_ASSERT_EQ(res.value, 2u);
    TL_EXPECT_EQ(rows[0].x, 3);
    TL_EXPECT_EQ(rows[1].y, 6);
    TL_EXPECT_EQ(ents[0].bits, e0.bits);
    TL_EXPECT_EQ(ents[1].bits, e1.bits);
    TL_EXPECT_EQ(r.pos, r.len);
}
