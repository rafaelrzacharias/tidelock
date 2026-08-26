// ---------------------------------------------------------------------------------------------
// loaders/image.cpp - asset_load_texture: stb_image -> platform DrawApi texture.
//
// Spec: docs/ASSETS-AND-DATA.md §8.2. Header-first stub (docs/ROADMAP.md §0 rule 1): TL_FATAL
// until the next lane commit fills it in with the real stb_image + DrawApi call sequence.
// ---------------------------------------------------------------------------------------------
#include "core/assets.h"

Result<TexHandle> asset_load_texture(AssetRegistry*, const PlatformApi*, VMemArena*, NameHash) {
    TL_FATAL("unimplemented");
}
