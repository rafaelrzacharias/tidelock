// ---------------------------------------------------------------------------------------------
// assets.cpp - AssetRegistry init/release/get; asset_create_streaming.
//
// asset_load_texture is loaders/image.cpp's; asset_load_font is loaders/font.cpp's stub
// (docs/ASSETS-AND-DATA.md §8.1 file layout). See assets.h for the contract each function meets.
// ---------------------------------------------------------------------------------------------
#include "core/assets.h"

// True once the domain (Handle<_,12,4>'s 4096 slots) has no reusable slot left: the free list is
// empty AND every one of the 4096 slots has been allocated at least once (slotmap_insert appends
// past the free list only up to the arena's reserved cap, docs/CONTAINERS.md §8.2).
static bool asset_domain_full(u32 slot_cap, u32 free_count) {
    return free_count == 0u && slot_cap >= 4096u;
}

ErrCode asset_registry_init(AssetRegistry* reg, const VMemApi* os) {
    ErrCode e;
    e = slotmap_init(&reg->textures, "assets.tex.slots"_id, "assets.tex.gen"_id,
                     "assets.tex.free"_id, "assets.tex.live"_id, os);
    if (e != ERR_OK) { return e; }
    e = slotmap_init(&reg->fonts, "assets.font.slots"_id, "assets.font.gen"_id,
                     "assets.font.free"_id, "assets.font.live"_id, os);
    if (e != ERR_OK) { return e; }
    e = vmem_arena_init(&reg->arena, "assets.by_name.arena"_id, 4u * 1024u * 1024u,
                        ARENA_ZERO_ON_PUSH, os);
    if (e != ERR_OK) { return e; }
    map_init(&reg->by_name, &reg->arena, 1024u);
    return ERR_OK;
}

Result<TexHandle> asset_create_streaming(AssetRegistry* reg, const PlatformApi* platform,
                                         u16 w, u16 h, PixelFmt fmt) {
    if (w == 0u || h == 0u) { return Result<TexHandle>{ TexHandle{}, ERR_ASSET_BAD_ARG }; }
    if (asset_domain_full(slotmap_slot_cap(&reg->textures), reg->textures.free_list.count)) {
        return Result<TexHandle>{ TexHandle{}, ERR_ASSET_LIMIT };
    }
    Result<TexHandle> dev = platform->draw.texture_create(platform->draw.ctx, w, h, (u8)fmt, TEX_STREAMING);
    if (dev.err != ERR_OK) { return Result<TexHandle>{ TexHandle{}, dev.err }; }
    AssetRec rec{};
    rec.name = 0;
    rec.refcount = 1u;
    rec.kind_specific = (u32)dev.value.bits;
    rec.w = w; rec.h = h;
    rec.kind = ASSET_STREAMING;
    rec.state = ASSET_STATE_RESIDENT;
    TexHandle h_out = slotmap_insert(&reg->textures, &rec);
    return Result<TexHandle>{ h_out, ERR_OK };
}

void asset_release_texture(AssetRegistry* reg, const PlatformApi* platform, TexHandle h) {
    AssetRec* rec = slotmap_get(&reg->textures, h);
    if (rec == nullptr) { return; }
    TL_ASSERT(rec->refcount > 0u);
    rec->refcount -= 1u;
    if (rec->refcount != 0u) { return; }
    TexHandle dev{}; dev.bits = (TexHandle::rep)rec->kind_specific;
    u8 kind = rec->kind;
    NameHash name = rec->name;
    platform->draw.texture_destroy(platform->draw.ctx, dev);
    if (kind != ASSET_STREAMING) { map_remove(&reg->by_name, name); }
    slotmap_remove(&reg->textures, h);
}

void asset_release_font(AssetRegistry* reg, const PlatformApi*, FontHandle h) {
    AssetRec* rec = slotmap_get(&reg->fonts, h);
    if (rec == nullptr) { return; }
    TL_ASSERT(rec->refcount > 0u);
    rec->refcount -= 1u;
    if (rec->refcount != 0u) { return; }
    NameHash name = rec->name;
    map_remove(&reg->by_name, name);
    slotmap_remove(&reg->fonts, h);
}

const AssetRec* asset_get_texture(const AssetRegistry* reg, TexHandle h) {
    if (!slotmap_alive(&reg->textures, h)) { return nullptr; }
    return &reg->textures.slots.data[handle_index(h)];
}

const AssetRec* asset_get_font(const AssetRegistry* reg, FontHandle h) {
    if (!slotmap_alive(&reg->fonts, h)) { return nullptr; }
    return &reg->fonts.slots.data[handle_index(h)];
}
