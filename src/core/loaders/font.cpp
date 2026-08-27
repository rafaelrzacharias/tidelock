// ---------------------------------------------------------------------------------------------
// loaders/font.cpp - asset_load_font: bytes -> SDL_ttf face handle.
//
// Spec: docs/ASSETS-AND-DATA.md §8.2 ("FONT (SDL_ttf face, dev text + later game text); glyph
// atlas lives in render/text.cpp"). Header-first stub (docs/ROADMAP.md §0 rule 1): TL_FATAL
// until the next lane commit fills it in.
// ---------------------------------------------------------------------------------------------
#include "core/assets.h"

Result<FontHandle> asset_load_font(AssetRegistry*, const PlatformApi*, VMemArena*, NameHash) {
    TL_FATAL("unimplemented");
}
