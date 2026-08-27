// ---------------------------------------------------------------------------------------------
// loaders/image.cpp - asset_load_texture: stb_image -> platform DrawApi texture.
//
// Spec: docs/ASSETS-AND-DATA.md §8.2. `<stb_image.h>` is included here in DECLARATION-ONLY mode
// (no *_IMPLEMENTATION macro) - `tools/audit/includes.py`'s BACKEND_HEADERS grants the "stb_"
// token to src/core for exactly this; the one real implementation TU (vendor/stb/stb_impl.c,
// STBI_MALLOC hooked to pool_vendor) is linked in via the `stb` CMake target
// (src/core/CMakeLists.txt), the same declaration-only shape src/vendor_glue/stb_glue.cpp uses.
// ---------------------------------------------------------------------------------------------
#include "core/assets.h"
#include <stb_image.h>

Result<TexHandle> asset_load_texture(AssetRegistry* reg, const PlatformApi* platform,
                                     VMemArena* scratch, StrView name) {
    if (name.len == 0u) { return Result<TexHandle>{ TexHandle{}, ERR_ASSET_BAD_ARG }; }
    NameHash name_hash = sv_hash(name);

    u32* packed = map_get(&reg->by_name, name_hash);
    if (packed != nullptr) {
        u8 kind = (u8)(*packed >> 16);
        TL_CHECK(kind == ASSET_IMAGE);
        TexHandle h{}; h.bits = (TexHandle::rep)(*packed & 0xFFFFu);
        AssetRec* rec = slotmap_get(&reg->textures, h);
        rec->refcount += 1u;
        return Result<TexHandle>{ h, ERR_OK };
    }

    // Domain full: no reusable slot on the free list and every one of the 4096 (Handle<_,12,4>)
    // slots has been allocated at least once (docs/CONTAINERS.md §8.2 - the same check
    // assets.cpp's asset_create_streaming makes; not shared across TUs since each is three lines).
    if (reg->textures.free_list.count == 0u && slotmap_slot_cap(&reg->textures) >= 4096u) {
        return Result<TexHandle>{ TexHandle{}, ERR_ASSET_LIMIT };
    }

    Result<Span<u8>> bytes = platform->file.read_all(platform->file.ctx, name, scratch);
    if (bytes.err != ERR_OK) {
        ErrCode e = bytes.err == (ErrCode)ERR_PLATFORM_FILE_NOT_FOUND ? ERR_ASSET_NOT_FOUND : ERR_ASSET_FILE_IO;
        return Result<TexHandle>{ TexHandle{}, e };
    }

    int w = 0, h = 0, channels_in_file = 0;
    u8* pixels = stbi_load_from_memory(bytes.value.data, (int)bytes.value.count, &w, &h,
                                       &channels_in_file, 4);
    if (pixels == nullptr) { return Result<TexHandle>{ TexHandle{}, ERR_ASSET_IMAGE_DECODE }; }
    if (w <= 0 || h <= 0 || w > 0xFFFF || h > 0xFFFF) {
        stbi_image_free(pixels);
        return Result<TexHandle>{ TexHandle{}, ERR_ASSET_IMAGE_DECODE };
    }

    Result<TexHandle> dev = platform->draw.texture_create(platform->draw.ctx, (u16)w, (u16)h,
                                                           PIXFMT_RGBA8, TEX_STATIC);
    if (dev.err != ERR_OK) {
        stbi_image_free(pixels);
        return Result<TexHandle>{ TexHandle{}, dev.err };
    }
    ErrCode upload_err = platform->draw.texture_upload(platform->draw.ctx, dev.value, pixels,
                                                        (u32)w * 4u);
    stbi_image_free(pixels);
    if (upload_err != ERR_OK) {
        platform->draw.texture_destroy(platform->draw.ctx, dev.value);
        return Result<TexHandle>{ TexHandle{}, upload_err };
    }

    AssetRec rec{};
    rec.name = name_hash;
    rec.refcount = 1u;
    rec.kind_specific = (u32)dev.value.bits;
    rec.w = (u16)w; rec.h = (u16)h;
    rec.kind = ASSET_IMAGE;
    rec.state = ASSET_STATE_RESIDENT;
    TexHandle h_out = slotmap_insert(&reg->textures, &rec);
    map_put(&reg->by_name, name_hash, ((u32)ASSET_IMAGE << 16) | (u32)h_out.bits);
    return Result<TexHandle>{ h_out, ERR_OK };
}
