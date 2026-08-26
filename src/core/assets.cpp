// ---------------------------------------------------------------------------------------------
// assets.cpp - AssetRegistry init/release/get. asset_load_texture is loaders/image.cpp's;
//   asset_load_font is loaders/font.cpp's (docs/ASSETS-AND-DATA.md §8.1 file layout).
//
// Header-first stub (docs/ROADMAP.md §0 rule 1): every body TL_FATAL("unimplemented") until the
// next lane commit fills it in. See assets.h for the contract each function must meet.
// ---------------------------------------------------------------------------------------------
#include "core/assets.h"

ErrCode asset_registry_init(AssetRegistry*, NameHash, const VMemApi*) {
    TL_FATAL("unimplemented");
}

Result<TexHandle> asset_create_streaming(AssetRegistry*, const PlatformApi*, u16, u16, PixelFmt) {
    TL_FATAL("unimplemented");
}

void asset_release_texture(AssetRegistry*, const PlatformApi*, TexHandle) {
    TL_FATAL("unimplemented");
}

void asset_release_font(AssetRegistry*, const PlatformApi*, FontHandle) {
    TL_FATAL("unimplemented");
}

const AssetRec* asset_get_texture(const AssetRegistry*, TexHandle) {
    TL_FATAL("unimplemented");
}

const AssetRec* asset_get_font(const AssetRegistry*, FontHandle) {
    TL_FATAL("unimplemented");
}
