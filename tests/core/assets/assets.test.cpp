// assets.test.cpp - docs/ASSETS-AND-DATA.md §8.5: load/dedup/refcount/free/stale-handle for
// textures; missing file and malformed image -> named errors; streaming textures.
#include "runner/tl_test.h"
#include "platform/platform_test_util.h"
#include "vendor_glue/pool_vendor_test_api.h"
#include "core/assets.h"
#include "foundation/vmem_arena.h"

#include <stdio.h>

namespace {

const char* TEX_PATH = "tl_assets_test_1x1.bmp";

// The same hand-built 1x1 24bpp uncompressed BMP tests/vendor_glue/stb_glue.test.cpp uses (one
// pure-red pixel; stb_image decodes BMP with no extra format library).
const u8 BMP_1X1[] = {
    'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0,
    1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0, 0, 4, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x00, 0x00, 0xFF, 0x00,
};

void write_file(TestCtx* t, const char* path, const void* data, usize n) {
    FILE* f = fopen(path, "wb");
    TL_ASSERT_TRUE(f != nullptr);
    fwrite(data, 1, n, f);
    fclose(f);
}

struct Fixture {
    const PlatformApi* platform;
    AssetRegistry reg;
    VMemArena scratch;
};

// Out-param, not a return value: the TL_ASSERT_* macros this needs expand to a bare `return;`,
// which only fits a void function (the same reason every TL_TEST body is void).
void fixture_init(TestCtx* t, Fixture* f) {
    vendor_glue_test_ensure_pool_vendor(t);   // stb_image's STBI_MALLOC is hooked to pool_vendor
    f->platform = platform_test_init();
    TL_ASSERT_NOT_NULL(f->platform);
    TL_ASSERT_EQ(asset_registry_init(&f->reg, &f->platform->vmem), ERR_OK);
    TL_ASSERT_EQ(vmem_arena_init(&f->scratch, "assets_test_scratch"_id, 4u * 1024u * 1024u, 0u,
                                 &f->platform->vmem), ERR_OK);
}

void fixture_shutdown(Fixture* f) { platform_test_shutdown(f->platform); }

}  // namespace

TL_TEST(asset_load_dedup_refcount_release_stale_handle, "core,assets") {
    Fixture f{};
    fixture_init(t, &f);
    write_file(t, TEX_PATH, BMP_1X1, sizeof BMP_1X1);

    Result<TexHandle> r1 = asset_load_texture(&f.reg, f.platform, &f.scratch, sv(TEX_PATH));
    TL_ASSERT_EQ(r1.err, ERR_OK);
    const AssetRec* rec1 = asset_get_texture(&f.reg, r1.value);
    TL_ASSERT_NOT_NULL(rec1);
    TL_EXPECT_EQ(rec1->refcount, 1u);
    TL_EXPECT_EQ(rec1->w, 1u);
    TL_EXPECT_EQ(rec1->h, 1u);
    TL_EXPECT_EQ(rec1->kind, (u8)ASSET_IMAGE);

    // dedup: a second load of the same name returns the SAME handle, refcount 2, no second read.
    Result<TexHandle> r2 = asset_load_texture(&f.reg, f.platform, &f.scratch, sv(TEX_PATH));
    TL_ASSERT_EQ(r2.err, ERR_OK);
    TL_EXPECT_EQ(r2.value.bits, r1.value.bits);
    const AssetRec* rec2 = asset_get_texture(&f.reg, r1.value);
    TL_ASSERT_NOT_NULL(rec2);
    TL_EXPECT_EQ(rec2->refcount, 2u);

    // release once: still alive, refcount 1.
    asset_release_texture(&f.reg, f.platform, r1.value);
    const AssetRec* rec3 = asset_get_texture(&f.reg, r1.value);
    TL_ASSERT_NOT_NULL(rec3);
    TL_EXPECT_EQ(rec3->refcount, 1u);

    // release again: refcount 0, slot freed - the handle is now stale.
    asset_release_texture(&f.reg, f.platform, r1.value);
    TL_EXPECT_NULL(asset_get_texture(&f.reg, r1.value));

    // a fresh load after full release re-reads from disk and mints a NEW handle (the old slot
    // was zeroed and freed, not reused with the same generation - docs/CONTAINERS.md §8.2).
    Result<TexHandle> r3 = asset_load_texture(&f.reg, f.platform, &f.scratch, sv(TEX_PATH));
    TL_ASSERT_EQ(r3.err, ERR_OK);
    const AssetRec* rec4 = asset_get_texture(&f.reg, r3.value);
    TL_ASSERT_NOT_NULL(rec4);
    TL_EXPECT_EQ(rec4->refcount, 1u);
    asset_release_texture(&f.reg, f.platform, r3.value);

    remove(TEX_PATH);
    fixture_shutdown(&f);
}

TL_TEST(asset_load_missing_file_named_error, "core,assets") {
    Fixture f{};
    fixture_init(t, &f);
    Result<TexHandle> r = asset_load_texture(&f.reg, f.platform, &f.scratch,
                                             sv("tl_assets_test_does_not_exist.bmp"));
    TL_EXPECT_EQ(r.err, ERR_ASSET_NOT_FOUND);
    fixture_shutdown(&f);
}

TL_TEST(asset_load_malformed_image_named_error, "core,assets") {
    Fixture f{};
    fixture_init(t, &f);
    const char* path = "tl_assets_test_garbage.bmp";
    static const u8 garbage[] = { 0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD, 0xFC };
    write_file(t, path, garbage, sizeof garbage);

    Result<TexHandle> r = asset_load_texture(&f.reg, f.platform, &f.scratch, sv(path));
    TL_EXPECT_EQ(r.err, ERR_ASSET_IMAGE_DECODE);

    remove(path);
    fixture_shutdown(&f);
}

TL_TEST(asset_load_bad_arg_empty_name, "core,assets") {
    Fixture f{};
    fixture_init(t, &f);
    Result<TexHandle> r = asset_load_texture(&f.reg, f.platform, &f.scratch, StrView{ "", 0 });
    TL_EXPECT_EQ(r.err, ERR_ASSET_BAD_ARG);
    fixture_shutdown(&f);
}

TL_TEST(asset_streaming_create_and_release, "core,assets") {
    Fixture f{};
    fixture_init(t, &f);

    Result<TexHandle> bad = asset_create_streaming(&f.reg, f.platform, 0u, 4u, PIXFMT_RGBA8);
    TL_EXPECT_EQ(bad.err, ERR_ASSET_BAD_ARG);

    Result<TexHandle> tex = asset_create_streaming(&f.reg, f.platform, 4u, 4u, PIXFMT_RGBA8);
    TL_ASSERT_EQ(tex.err, ERR_OK);
    const AssetRec* rec = asset_get_texture(&f.reg, tex.value);
    TL_ASSERT_NOT_NULL(rec);
    TL_EXPECT_EQ(rec->kind, (u8)ASSET_STREAMING);
    TL_EXPECT_EQ(rec->refcount, 1u);

    // a second streaming create is a DIFFERENT handle, even with identical dimensions (never
    // deduped by name - streaming textures have none, docs/ASSETS-AND-DATA.md §8.2).
    Result<TexHandle> tex2 = asset_create_streaming(&f.reg, f.platform, 4u, 4u, PIXFMT_RGBA8);
    TL_ASSERT_EQ(tex2.err, ERR_OK);
    TL_EXPECT_NE(tex2.value.bits, tex.value.bits);

    asset_release_texture(&f.reg, f.platform, tex.value);
    TL_EXPECT_NULL(asset_get_texture(&f.reg, tex.value));
    asset_release_texture(&f.reg, f.platform, tex2.value);

    fixture_shutdown(&f);
}

TL_TEST(asset_font_release_and_get_slot_lifecycle, "core,assets") {
    // asset_load_font is not yet implemented (deferred to render2d's render/text.cpp landing,
    // docs/ASSETS-AND-DATA.md §8.2 note); asset_release_font/asset_get_font are pure SlotMap
    // operations this lane ships now and tests directly against a hand-inserted slot.
    Fixture f{};
    fixture_init(t, &f);

    AssetRec rec{};
    rec.name = "test_font"_id;
    rec.refcount = 2u;
    rec.kind = ASSET_FONT;
    rec.state = ASSET_STATE_RESIDENT;
    FontHandle h = slotmap_insert(&f.reg.fonts, &rec);
    map_put(&f.reg.by_name, rec.name, ((u32)ASSET_FONT << 16) | (u32)h.bits);

    TL_ASSERT_NOT_NULL(asset_get_font(&f.reg, h));
    TL_EXPECT_EQ(asset_get_font(&f.reg, h)->refcount, 2u);

    asset_release_font(&f.reg, f.platform, h);   // 2 -> 1, still alive
    TL_ASSERT_NOT_NULL(asset_get_font(&f.reg, h));
    TL_EXPECT_EQ(asset_get_font(&f.reg, h)->refcount, 1u);

    asset_release_font(&f.reg, f.platform, h);   // 1 -> 0, freed
    TL_EXPECT_NULL(asset_get_font(&f.reg, h));

    fixture_shutdown(&f);
}
