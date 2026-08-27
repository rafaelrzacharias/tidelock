// save.test.cpp - docs/ASSETS-AND-DATA.md §8.5: round-trip equality per arena (a REFLECTED
// singleton and an ECS_COLUMN component), truncated/corrupt file refusal.
#include "runner/tl_test.h"
#include "platform/platform_test_util.h"
#include "core/world_test_util.h"
#include "core/save.h"

#include <stdio.h>

// Round 1 review D9: SaveDesc::aliases/migrations had zero tests, though docs/ASSETS-AND-DATA.md
// §8.5 names three cases explicitly (rename via alias; kind change -> refusal; kind change ->
// migration fn path - "added field via default" is encoder.h's own, already-tested territory,
// not save.h's). Two same-layout-different-name components simulate "an old build wrote this
// file"; two same-name-different-kind components simulate a schema's field changing width.
#define TL_FIELDS_SaveAliasOld(X, XA, XH) X(u32, hp)
TL_COMPONENT_FLAGS(SaveAliasOld, COMP_SINGLETON)

#define TL_FIELDS_SaveAliasNew(X, XA, XH) X(u32, health)
TL_COMPONENT_FLAGS(SaveAliasNew, COMP_SINGLETON)

#define TL_FIELDS_SaveKindOld(X, XA, XH) X(u32, val)
TL_COMPONENT_FLAGS(SaveKindOld, COMP_SINGLETON)

#define TL_FIELDS_SaveKindNew(X, XA, XH) X(u64, val)
TL_COMPONENT_FLAGS(SaveKindNew, COMP_SINGLETON)

namespace {

const char* SAVE_PATH = "tl_save_test.bin";

void write_raw(TestCtx* t, const char* path, const void* data, usize n) {
    FILE* f = fopen(path, "wb");
    TL_ASSERT_TRUE(f != nullptr);
    fwrite(data, 1, n, f);
    fclose(f);
}

usize read_raw(const char* path, u8* out, usize cap) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) { return 0u; }
    usize n = fread(out, 1, cap, f);
    fclose(f);
    return n;
}

// Round 1 review D5/D6/D7: recomputes the trailer crc32 (now over the WHOLE file, D6) and writes
// `buf` back out, so a test that corrupts one field in isolation (a block's byte_len, a kind
// byte) exercises THAT field's own check rather than tripping the CRC check first - the
// reviewer's own methodology ("measured on a CRC-corrected file").
void recrc_and_write(TestCtx* t, const char* path, u8* buf, usize n) {
    u32 crc = crc32(buf, (u64)n - 4u);
    buf[n - 4] = (u8)(crc & 0xFFu);
    buf[n - 3] = (u8)((crc >> 8) & 0xFFu);
    buf[n - 2] = (u8)((crc >> 16) & 0xFFu);
    buf[n - 1] = (u8)((crc >> 24) & 0xFFu);
    write_raw(t, path, buf, n);
}

// A versioned migration for the SaveKindOld(u32 val) -> SaveKindNew(u64 val) width change: skips
// the stored field table (this fn owns interpreting the OLD layout, per save.h's own contract),
// then widens each stored u32 into the live u64 field's low bytes (little-endian, docs/CANON.md).
Result<u32> migrate_kind_widen(ByteReader* r, const ComponentInfo* live_info, void* out_rows, u32 max_rows) {
    u32 field_count = br_get_u32(r);
    for (u32 i = 0; i < field_count; ++i) {
        br_get_u64(r); br_get_u8(r); br_get_u8(r); br_get_u16(r); br_get_u32(r);   // one stored FieldInfo entry
    }
    u32 row_count = br_get_u32(r);
    if (!br_ok(r)) { return Result<u32>{ 0u, ERR_BYTES_TRUNCATED }; }
    if (row_count > max_rows) { return Result<u32>{ 0u, ERR_ENC_OVERFLOW }; }
    u8* dst = (u8*)out_rows;
    memset(dst, 0, (u64)max_rows * live_info->size);
    for (u32 i = 0; i < row_count; ++i) {
        u32 old_val = br_get_u32(r);
        memcpy(dst + (u64)i * live_info->size, &old_val, 4u);
    }
    if (!br_ok(r)) { return Result<u32>{ 0u, ERR_BYTES_TRUNCATED }; }
    return Result<u32>{ row_count, ERR_OK };
}

SaveDesc base_desc(const ArenaRegistry* registry, World* world) {
    SaveDesc d{};
    d.registry = registry;
    d.world = world;
    d.seed = 123u;
    d.tick = 456u;
    return d;
}

}  // namespace

TL_TEST(save_reflected_singleton_round_trip, "core,save") {
    const PlatformApi* platform = platform_test_init();
    TL_ASSERT_NOT_NULL(platform);
    VMemArena scratch;
    TL_ASSERT_EQ(vmem_arena_init(&scratch, "save_test_scratch"_id, 16u * 1024u * 1024u, 0u, &platform->vmem), ERR_OK);

    WorldFixture* f = wt_fixture(0);
    TL_ASSERT_TRUE(world_fixture_init(f, 1u));
    world_register_component(&f->w, &WCfg_info);
    world_build_schedule(&f->w);

    WCfg* cfg = world_singleton<WCfg>(&f->w);
    cfg->gravity = 42u;
    cfg->mode = 7u;

    u32 wcfg_id = world_component_id<WCfg>(&f->w);
    NameHash wcfg_arena_id = f->w.comps[wcfg_id].dense_arena.id;

    SaveArenaDesc ad{};
    ad.arena_id = wcfg_arena_id;
    ad.kind = SAVE_ENC_REFLECTED;
    ad.info = &WCfg_info;
    ad.max_rows = 1u;

    SaveDesc d = base_desc(&f->reg, &f->w);
    d.arena_descs = Span<const SaveArenaDesc>{ &ad, 1u };

    TL_ASSERT_EQ(save_write(&d, platform, sv(SAVE_PATH), &scratch), ERR_OK);

    // mutate the live singleton so the load is observably restoring, not a no-op
    cfg->gravity = 999u;
    cfg->mode = 999u;

    u64 out_seed = 0u, out_tick = 0u;
    TL_ASSERT_EQ(save_read(&d, platform, sv(SAVE_PATH), &scratch, &out_seed, &out_tick), ERR_OK);
    TL_EXPECT_EQ(out_seed, 123u);
    TL_EXPECT_EQ(out_tick, 456u);
    TL_EXPECT_EQ(cfg->gravity, 42u);
    TL_EXPECT_EQ(cfg->mode, 7u);

    remove(SAVE_PATH);
    platform_test_shutdown(platform);
}

TL_TEST(save_ecs_column_round_trip, "core,save") {
    const PlatformApi* platform = platform_test_init();
    TL_ASSERT_NOT_NULL(platform);
    VMemArena scratch;
    TL_ASSERT_EQ(vmem_arena_init(&scratch, "save_test_scratch2"_id, 16u * 1024u * 1024u, 0u, &platform->vmem), ERR_OK);

    WorldFixture* f = wt_fixture(1);
    TL_ASSERT_TRUE(world_fixture_init(f, 1u));
    world_register_component(&f->w, &WPos_info);
    world_build_schedule(&f->w);

    Entity e1 = world_spawn(&f->w);
    Entity e2 = world_spawn(&f->w);
    world_flush(&f->w);
    world_add<WPos>(&f->w, e1, WPos{ 1, 2 });
    world_add<WPos>(&f->w, e2, WPos{ 3, 4 });
    world_flush(&f->w);

    u32 wpos_id = world_component_id<WPos>(&f->w);
    NameHash wpos_arena_id = f->w.comps[wpos_id].dense_arena.id;

    SaveArenaDesc ad{};
    ad.arena_id = wpos_arena_id;
    ad.kind = SAVE_ENC_ECS_COLUMN;
    ad.info = &WPos_info;
    ad.max_rows = 16u;
    ad.comp = (ComponentId)wpos_id;

    SaveDesc d = base_desc(&f->reg, &f->w);
    d.arena_descs = Span<const SaveArenaDesc>{ &ad, 1u };

    TL_ASSERT_EQ(save_write(&d, platform, sv(SAVE_PATH), &scratch), ERR_OK);

    // remove the component from both entities so the reload is observably restoring
    world_remove(&f->w, e1, (ComponentId)wpos_id);
    world_remove(&f->w, e2, (ComponentId)wpos_id);
    world_flush(&f->w);
    TL_EXPECT_NULL(world_get<WPos>(&f->w, e1));
    TL_EXPECT_NULL(world_get<WPos>(&f->w, e2));

    u64 out_seed = 0u, out_tick = 0u;
    TL_ASSERT_EQ(save_read(&d, platform, sv(SAVE_PATH), &scratch, &out_seed, &out_tick), ERR_OK);
    world_flush(&f->w);   // world_add_raw only RECORDS CMD_ADD (docs/ECS.md §4) - apply it

    const WPos* p1 = world_get<WPos>(&f->w, e1);
    const WPos* p2 = world_get<WPos>(&f->w, e2);
    TL_ASSERT_NOT_NULL(p1);
    TL_ASSERT_NOT_NULL(p2);
    TL_EXPECT_TRUE((p1->x == 1 && p1->y == 2 && p2->x == 3 && p2->y == 4) ||
                   (p1->x == 3 && p1->y == 4 && p2->x == 1 && p2->y == 2));   // column order is not part of the contract

    remove(SAVE_PATH);
    platform_test_shutdown(platform);
}

TL_TEST(save_read_bad_magic_refused, "core,save") {
    const PlatformApi* platform = platform_test_init();
    TL_ASSERT_NOT_NULL(platform);
    VMemArena scratch;
    TL_ASSERT_EQ(vmem_arena_init(&scratch, "save_test_scratch3"_id, 1u * 1024u * 1024u, 0u, &platform->vmem), ERR_OK);

    u8 junk[200];
    for (u32 i = 0; i < sizeof junk; ++i) { junk[i] = (u8)i; }
    write_raw(t, SAVE_PATH, junk, sizeof junk);

    ArenaRegistry reg{};
    SaveDesc d = base_desc(&reg, nullptr);
    u64 out_seed = 0u, out_tick = 0u;
    TL_EXPECT_EQ(save_read(&d, platform, sv(SAVE_PATH), &scratch, &out_seed, &out_tick), ERR_SAVE_BAD_MAGIC);

    remove(SAVE_PATH);
    platform_test_shutdown(platform);
}

TL_TEST(save_read_truncated_refused, "core,save") {
    const PlatformApi* platform = platform_test_init();
    TL_ASSERT_NOT_NULL(platform);
    VMemArena scratch;
    TL_ASSERT_EQ(vmem_arena_init(&scratch, "save_test_scratch4"_id, 1u * 1024u * 1024u, 0u, &platform->vmem), ERR_OK);

    u8 tiny[10] = {};
    write_raw(t, SAVE_PATH, tiny, sizeof tiny);

    ArenaRegistry reg{};
    SaveDesc d = base_desc(&reg, nullptr);
    u64 out_seed = 0u, out_tick = 0u;
    TL_EXPECT_EQ(save_read(&d, platform, sv(SAVE_PATH), &scratch, &out_seed, &out_tick), ERR_SAVE_TRUNCATED);

    remove(SAVE_PATH);
    platform_test_shutdown(platform);
}

TL_TEST(save_read_crc_mismatch_refused, "core,save") {
    const PlatformApi* platform = platform_test_init();
    TL_ASSERT_NOT_NULL(platform);
    VMemArena scratch;
    TL_ASSERT_EQ(vmem_arena_init(&scratch, "save_test_scratch5"_id, 16u * 1024u * 1024u, 0u, &platform->vmem), ERR_OK);

    WorldFixture* f = wt_fixture(2);
    TL_ASSERT_TRUE(world_fixture_init(f, 1u));
    world_register_component(&f->w, &WCfg_info);
    world_build_schedule(&f->w);

    u32 wcfg_id = world_component_id<WCfg>(&f->w);
    SaveArenaDesc ad{};
    ad.arena_id = f->w.comps[wcfg_id].dense_arena.id;
    ad.kind = SAVE_ENC_REFLECTED;
    ad.info = &WCfg_info;
    ad.max_rows = 1u;

    SaveDesc d = base_desc(&f->reg, &f->w);
    d.arena_descs = Span<const SaveArenaDesc>{ &ad, 1u };
    TL_ASSERT_EQ(save_write(&d, platform, sv(SAVE_PATH), &scratch), ERR_OK);

    // flip a payload byte (just past the fixed 160 B header) without touching the trailer
    u8 buf[512];
    usize n = read_raw(SAVE_PATH, buf, sizeof buf);
    TL_ASSERT_TRUE(n > 164u);
    buf[160] ^= 0xFFu;
    write_raw(t, SAVE_PATH, buf, n);

    u64 out_seed = 0u, out_tick = 0u;
    TL_EXPECT_EQ(save_read(&d, platform, sv(SAVE_PATH), &scratch, &out_seed, &out_tick), ERR_SAVE_CRC_MISMATCH);

    remove(SAVE_PATH);
    platform_test_shutdown(platform);
}

// Round 1 review D6: the crc32 window used to start at byte 160 (after SaveHeader), leaving
// seed/tick/format_version/origin/name_table_len/arena_count unprotected - measured: a corrupted
// `tick` byte (SaveHeader offset 80, docs/ASSETS-AND-DATA.md §8.4) loaded as ERR_OK with the
// wrong tick. Fixed to cover the whole file; this is the same byte the reviewer flipped, now
// caught the same way any other corruption already was.
TL_TEST(save_read_header_byte_corruption_is_crc_protected, "core,save") {
    const PlatformApi* platform = platform_test_init();
    TL_ASSERT_NOT_NULL(platform);
    VMemArena scratch;
    TL_ASSERT_EQ(vmem_arena_init(&scratch, "save_test_scratch6"_id, 16u * 1024u * 1024u, 0u, &platform->vmem), ERR_OK);

    WorldFixture* f = wt_fixture(3);
    TL_ASSERT_TRUE(world_fixture_init(f, 1u));
    world_register_component(&f->w, &WCfg_info);
    world_build_schedule(&f->w);

    u32 wcfg_id = world_component_id<WCfg>(&f->w);
    SaveArenaDesc ad{};
    ad.arena_id = f->w.comps[wcfg_id].dense_arena.id;
    ad.kind = SAVE_ENC_REFLECTED;
    ad.info = &WCfg_info;
    ad.max_rows = 1u;

    SaveDesc d = base_desc(&f->reg, &f->w);
    d.arena_descs = Span<const SaveArenaDesc>{ &ad, 1u };
    TL_ASSERT_EQ(save_write(&d, platform, sv(SAVE_PATH), &scratch), ERR_OK);

    u8 buf[512];
    usize n = read_raw(SAVE_PATH, buf, sizeof buf);
    TL_ASSERT_TRUE(n > 164u);
    buf[80] ^= 0xFFu;   // SaveHeader.tick, TL_OFFSETS_SaveHeader(tick, 80) - no crc fixup: this IS
                        // the corruption the crc trailer must now catch, not something to hide
    write_raw(t, SAVE_PATH, buf, n);

    u64 out_seed = 0u, out_tick = 0u;
    TL_EXPECT_EQ(save_read(&d, platform, sv(SAVE_PATH), &scratch, &out_seed, &out_tick), ERR_SAVE_CRC_MISMATCH);

    remove(SAVE_PATH);
    platform_test_shutdown(platform);
}

// Round 1 review D5: byte_len (the per-block payload length) was never checked against the bytes
// actually remaining in the file - a forged, inflated byte_len let block_r read past the real
// payload into whatever scratch-arena memory happened to follow it. CRC-corrected first (this
// isolates D5's own bound from D6's CRC window, the reviewer's own methodology).
TL_TEST(save_read_forged_block_byte_len_refused, "core,save") {
    const PlatformApi* platform = platform_test_init();
    TL_ASSERT_NOT_NULL(platform);
    VMemArena scratch;
    TL_ASSERT_EQ(vmem_arena_init(&scratch, "save_test_scratch7"_id, 16u * 1024u * 1024u, 0u, &platform->vmem), ERR_OK);

    WorldFixture* f = wt_fixture(3);
    TL_ASSERT_TRUE(world_fixture_init(f, 1u));
    world_register_component(&f->w, &WCfg_info);
    world_build_schedule(&f->w);

    u32 wcfg_id = world_component_id<WCfg>(&f->w);
    SaveArenaDesc ad{};
    ad.arena_id = f->w.comps[wcfg_id].dense_arena.id;
    ad.kind = SAVE_ENC_REFLECTED;
    ad.info = &WCfg_info;
    ad.max_rows = 1u;

    SaveDesc d = base_desc(&f->reg, &f->w);
    d.arena_descs = Span<const SaveArenaDesc>{ &ad, 1u };
    TL_ASSERT_EQ(save_write(&d, platform, sv(SAVE_PATH), &scratch), ERR_OK);

    u8 buf[512];
    usize n = read_raw(SAVE_PATH, buf, sizeof buf);
    TL_ASSERT_TRUE(n > 176u);
    // byte_len sits right after the block's arena_id(8)+kind(1)+pad(3), at buffer offset
    // 160 + 8 + 1 + 3 = 172 for this file's one and only block.
    buf[172] = 0x00u; buf[173] = 0x00u; buf[174] = 0x01u; buf[175] = 0x00u;   // 65536, far past real
    recrc_and_write(t, SAVE_PATH, buf, n);

    u64 out_seed = 0u, out_tick = 0u;
    TL_EXPECT_EQ(save_read(&d, platform, sv(SAVE_PATH), &scratch, &out_seed, &out_tick), ERR_SAVE_TRUNCATED);

    remove(SAVE_PATH);
    platform_test_shutdown(platform);
}

// Round 1 review D7: the file's kind byte used to drive decode dispatch with no check against
// what the caller actually registered for that arena_id - a mismatch (or a byte outside
// SaveEncoderKind's own range, which used to reach TL_FATAL from file content) could decode via
// the wrong encoder and, on apply, target the wrong component through `ad->comp`.
TL_TEST(save_read_kind_mismatch_refused, "core,save") {
    const PlatformApi* platform = platform_test_init();
    TL_ASSERT_NOT_NULL(platform);
    VMemArena scratch;
    TL_ASSERT_EQ(vmem_arena_init(&scratch, "save_test_scratch8"_id, 16u * 1024u * 1024u, 0u, &platform->vmem), ERR_OK);

    WorldFixture* f = wt_fixture(3);
    TL_ASSERT_TRUE(world_fixture_init(f, 1u));
    world_register_component(&f->w, &WCfg_info);
    world_build_schedule(&f->w);

    u32 wcfg_id = world_component_id<WCfg>(&f->w);
    SaveArenaDesc ad{};
    ad.arena_id = f->w.comps[wcfg_id].dense_arena.id;
    ad.kind = SAVE_ENC_REFLECTED;   // = 1
    ad.info = &WCfg_info;
    ad.max_rows = 1u;

    SaveDesc d = base_desc(&f->reg, &f->w);
    d.arena_descs = Span<const SaveArenaDesc>{ &ad, 1u };
    TL_ASSERT_EQ(save_write(&d, platform, sv(SAVE_PATH), &scratch), ERR_OK);

    u8 buf[512];
    usize n = read_raw(SAVE_PATH, buf, sizeof buf);
    TL_ASSERT_TRUE(n > 176u);
    // kind sits right after the block's arena_id(8), at buffer offset 160 + 8 = 168.

    // Case A: a VALID SaveEncoderKind that just isn't what the caller registered.
    buf[168] = (u8)SAVE_ENC_ECS_COLUMN;   // = 2, ad->kind is REFLECTED
    recrc_and_write(t, SAVE_PATH, buf, n);
    u64 out_seed = 0u, out_tick = 0u;
    TL_EXPECT_EQ(save_read(&d, platform, sv(SAVE_PATH), &scratch, &out_seed, &out_tick), ERR_SAVE_KIND_MISMATCH);

    // Case B: a byte outside SaveEncoderKind's own range entirely - used to reach TL_FATAL from
    // file content; must be the SAME named error, not a crash.
    buf[168] = 200u;
    recrc_and_write(t, SAVE_PATH, buf, n);
    TL_EXPECT_EQ(save_read(&d, platform, sv(SAVE_PATH), &scratch, &out_seed, &out_tick), ERR_SAVE_KIND_MISMATCH);

    remove(SAVE_PATH);
    platform_test_shutdown(platform);
}

// Round 1 review D8: hdr.name_table_len had no bound of its own and the skip loop never checked
// br_ok - a hostile 0xFFFFFFFF spun ~4G no-op iterations before the block loop finally reported
// ERR_SAVE_TRUNCATED. This pins the OUTCOME (still ERR_SAVE_TRUNCATED - the fix is the loop
// breaking out immediately instead of a free stall, which a unit test cannot time-bound
// portably, but a hostile name_table_len must still end up refused rather than accepted).
TL_TEST(save_read_bogus_name_table_len_refused, "core,save") {
    const PlatformApi* platform = platform_test_init();
    TL_ASSERT_NOT_NULL(platform);
    VMemArena scratch;
    TL_ASSERT_EQ(vmem_arena_init(&scratch, "save_test_scratch9"_id, 16u * 1024u * 1024u, 0u, &platform->vmem), ERR_OK);

    WorldFixture* f = wt_fixture(3);
    TL_ASSERT_TRUE(world_fixture_init(f, 1u));
    world_register_component(&f->w, &WCfg_info);
    world_build_schedule(&f->w);

    u32 wcfg_id = world_component_id<WCfg>(&f->w);
    SaveArenaDesc ad{};
    ad.arena_id = f->w.comps[wcfg_id].dense_arena.id;
    ad.kind = SAVE_ENC_REFLECTED;
    ad.info = &WCfg_info;
    ad.max_rows = 1u;

    SaveDesc d = base_desc(&f->reg, &f->w);
    d.arena_descs = Span<const SaveArenaDesc>{ &ad, 1u };
    TL_ASSERT_EQ(save_write(&d, platform, sv(SAVE_PATH), &scratch), ERR_OK);

    u8 buf[512];
    usize n = read_raw(SAVE_PATH, buf, sizeof buf);
    TL_ASSERT_TRUE(n > 164u);
    // name_table_len, TL_OFFSETS_SaveHeader(name_table_len, 96): 0xFFFFFFFF.
    buf[96] = 0xFFu; buf[97] = 0xFFu; buf[98] = 0xFFu; buf[99] = 0xFFu;
    recrc_and_write(t, SAVE_PATH, buf, n);

    u64 out_seed = 0u, out_tick = 0u;
    TL_EXPECT_EQ(save_read(&d, platform, sv(SAVE_PATH), &scratch, &out_seed, &out_tick), ERR_SAVE_TRUNCATED);

    remove(SAVE_PATH);
    platform_test_shutdown(platform);
}

// Round 1 review D9, docs/ASSETS-AND-DATA.md §8.5 "rename via alias": the file was written with a
// field named "hp" (SaveAliasOld); the live schema (SaveAliasNew) calls the same bytes "health".
// Both components share one layout (a single u32 at offset 0), so this is a pure field-rename,
// not a kind change - the write step is genuinely valid, only the read-side schema's NAME moved.
TL_TEST(save_read_field_rename_via_alias_round_trip, "core,save") {
    const PlatformApi* platform = platform_test_init();
    TL_ASSERT_NOT_NULL(platform);
    VMemArena scratch;
    TL_ASSERT_EQ(vmem_arena_init(&scratch, "save_test_scratch10"_id, 16u * 1024u * 1024u, 0u, &platform->vmem), ERR_OK);

    WorldFixture* f = wt_fixture(3);
    TL_ASSERT_TRUE(world_fixture_init(f, 1u));
    world_register_component(&f->w, &SaveAliasNew_info);
    world_build_schedule(&f->w);

    SaveAliasNew* live = world_singleton<SaveAliasNew>(&f->w);
    live->health = 77u;

    u32 comp_id = world_component_id<SaveAliasNew>(&f->w);
    NameHash arena_id = f->w.comps[comp_id].dense_arena.id;

    // Write AS IF an old build called this field "hp" - real bytes, an old field NAME.
    SaveArenaDesc ad_write{};
    ad_write.arena_id = arena_id;
    ad_write.kind = SAVE_ENC_REFLECTED;
    ad_write.info = &SaveAliasOld_info;
    ad_write.max_rows = 1u;
    SaveDesc d_write = base_desc(&f->reg, &f->w);
    d_write.arena_descs = Span<const SaveArenaDesc>{ &ad_write, 1u };
    TL_ASSERT_EQ(save_write(&d_write, platform, sv(SAVE_PATH), &scratch), ERR_OK);

    live->health = 0u;   // mutate so the reload is observably restoring, not a no-op

    FieldAlias fa{ SaveAliasOld_info.fields[0].name_hash, SaveAliasNew_info.fields[0].name_hash };
    SaveComponentAliases sca{ SaveAliasNew_info.name_hash, Span<const FieldAlias>{ &fa, 1u } };

    SaveArenaDesc ad_read{};
    ad_read.arena_id = arena_id;
    ad_read.kind = SAVE_ENC_REFLECTED;
    ad_read.info = &SaveAliasNew_info;
    ad_read.max_rows = 1u;
    SaveDesc d_read = base_desc(&f->reg, &f->w);
    d_read.arena_descs = Span<const SaveArenaDesc>{ &ad_read, 1u };
    d_read.aliases = Span<const SaveComponentAliases>{ &sca, 1u };

    u64 out_seed2 = 0u, out_tick2 = 0u;
    TL_ASSERT_EQ(save_read(&d_read, platform, sv(SAVE_PATH), &scratch, &out_seed2, &out_tick2), ERR_OK);
    TL_EXPECT_EQ(live->health, 77u);

    remove(SAVE_PATH);
    platform_test_shutdown(platform);
}

// Round 1 review D9, §8.5 "kind change -> ERR_SAVE_FIELD_KIND": the file stores "val" as a u32
// (SaveKindOld); the live schema (SaveKindNew) declares the SAME name as a u64. No migration is
// registered for this pair, so the width change must refuse, not silently reinterpret bytes.
TL_TEST(save_read_kind_change_without_migration_is_refused, "core,save") {
    const PlatformApi* platform = platform_test_init();
    TL_ASSERT_NOT_NULL(platform);
    VMemArena scratch;
    TL_ASSERT_EQ(vmem_arena_init(&scratch, "save_test_scratch11"_id, 16u * 1024u * 1024u, 0u, &platform->vmem), ERR_OK);

    WorldFixture* f = wt_fixture(3);
    TL_ASSERT_TRUE(world_fixture_init(f, 1u));
    world_register_component(&f->w, &SaveKindOld_info);
    world_build_schedule(&f->w);

    SaveKindOld* live = world_singleton<SaveKindOld>(&f->w);
    live->val = 42u;

    u32 comp_id = world_component_id<SaveKindOld>(&f->w);
    NameHash arena_id = f->w.comps[comp_id].dense_arena.id;

    SaveArenaDesc ad_write{};
    ad_write.arena_id = arena_id;
    ad_write.kind = SAVE_ENC_REFLECTED;
    ad_write.info = &SaveKindOld_info;
    ad_write.max_rows = 1u;
    SaveDesc d_write = base_desc(&f->reg, &f->w);
    d_write.arena_descs = Span<const SaveArenaDesc>{ &ad_write, 1u };
    TL_ASSERT_EQ(save_write(&d_write, platform, sv(SAVE_PATH), &scratch), ERR_OK);

    SaveArenaDesc ad_read{};
    ad_read.arena_id = arena_id;
    ad_read.kind = SAVE_ENC_REFLECTED;
    ad_read.info = &SaveKindNew_info;   // same name, different kind/size - no migration registered
    ad_read.max_rows = 1u;
    SaveDesc d_read = base_desc(&f->reg, &f->w);
    d_read.arena_descs = Span<const SaveArenaDesc>{ &ad_read, 1u };

    u64 out_seed2 = 0u, out_tick2 = 0u;
    TL_EXPECT_EQ(save_read(&d_read, platform, sv(SAVE_PATH), &scratch, &out_seed2, &out_tick2), ERR_SAVE_FIELD_KIND);

    remove(SAVE_PATH);
    platform_test_shutdown(platform);
}

// Round 1 review D9, §8.5 "kind change -> the migration fn path": the SAME width change as
// above, but with migrate_kind_widen registered for (SaveKindNew's name hash, format_version 1) -
// self-found while writing this test: save_write never stamped hdr.format_version at all (always
// 0, regardless of SAVE_FORMAT_VERSION), which would have made a version-keyed migration
// unreachable; fixed in save.cpp in the same commit.
TL_TEST(save_read_kind_change_via_migration_fn, "core,save") {
    const PlatformApi* platform = platform_test_init();
    TL_ASSERT_NOT_NULL(platform);
    VMemArena scratch;
    TL_ASSERT_EQ(vmem_arena_init(&scratch, "save_test_scratch12"_id, 16u * 1024u * 1024u, 0u, &platform->vmem), ERR_OK);

    WorldFixture* f = wt_fixture(3);
    TL_ASSERT_TRUE(world_fixture_init(f, 1u));
    world_register_component(&f->w, &SaveKindOld_info);
    world_build_schedule(&f->w);

    SaveKindOld* live = world_singleton<SaveKindOld>(&f->w);
    live->val = 42u;

    u32 comp_id = world_component_id<SaveKindOld>(&f->w);
    NameHash arena_id = f->w.comps[comp_id].dense_arena.id;

    SaveArenaDesc ad_write{};
    ad_write.arena_id = arena_id;
    ad_write.kind = SAVE_ENC_REFLECTED;
    ad_write.info = &SaveKindOld_info;
    ad_write.max_rows = 1u;
    SaveDesc d_write = base_desc(&f->reg, &f->w);
    d_write.arena_descs = Span<const SaveArenaDesc>{ &ad_write, 1u };
    TL_ASSERT_EQ(save_write(&d_write, platform, sv(SAVE_PATH), &scratch), ERR_OK);

    SaveComponentMigration mig{ SaveKindNew_info.name_hash, SAVE_FORMAT_VERSION, &migrate_kind_widen };

    SaveArenaDesc ad_read{};
    ad_read.arena_id = arena_id;
    ad_read.kind = SAVE_ENC_REFLECTED;
    ad_read.info = &SaveKindNew_info;
    ad_read.max_rows = 1u;
    SaveDesc d_read = base_desc(&f->reg, &f->w);
    d_read.arena_descs = Span<const SaveArenaDesc>{ &ad_read, 1u };
    d_read.migrations = Span<const SaveComponentMigration>{ &mig, 1u };

    u64 out_seed2 = 0u, out_tick2 = 0u;
    TL_ASSERT_EQ(save_read(&d_read, platform, sv(SAVE_PATH), &scratch, &out_seed2, &out_tick2), ERR_OK);
    TL_EXPECT_EQ(live->val, 42u);   // world's singleton is still SaveKindOld-shaped: migration
                                    // wrote its low 4 bytes, which alias live->val exactly

    remove(SAVE_PATH);
    platform_test_shutdown(platform);
}
