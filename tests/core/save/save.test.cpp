// save.test.cpp - docs/ASSETS-AND-DATA.md §8.5: round-trip equality per arena (a REFLECTED
// singleton and an ECS_COLUMN component), truncated/corrupt file refusal.
#include "runner/tl_test.h"
#include "platform/platform_test_util.h"
#include "core/world_test_util.h"
#include "core/save.h"

#include <stdio.h>

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
