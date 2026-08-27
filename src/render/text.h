#pragma once
// ---------------------------------------------------------------------------------------------
// text.h - text_layout: the reserved game-text seam, stub only.
//
// Spec: docs/RENDER2D.md §7 ("Game text (reserved, docs/RESERVED-SEAMS.md §2): SDL_ttf
//   rasterizes glyphs into an engine-owned atlas texture; a stateless layout function writes
//   quads into a caller buffer (Layr's TextLayoutEngine shape); submitted as sprites."), §9.1
//   (file layout: "text.cpp | reserved stub: text_layout() returns ERR_RENDER_UNSUPPORTED").
// Purpose: hold the seam's name and file so a future lane implements it without guessing where
//   it lives (docs/RESERVED-SEAMS.md §2's trigger: an Overburden-class HUD). No atlas, no
//   SDL_ttf glue, no layout algorithm exist yet - none is speced beyond the one sentence above.
// Invariants: always returns ERR_RENDER_UNSUPPORTED; writes nothing to `out`.
// Determinism: none - reserved, unimplemented.
// Threading: none.
// Includes: render/render.h.
// ---------------------------------------------------------------------------------------------
#include "render/render.h"

// Reserved stub (docs/RENDER2D.md §9.1). `out_cap` bounds the caller's quad buffer; `out_count`
// receives 0. Always ERR_RENDER_UNSUPPORTED until the seam (docs/RESERVED-SEAMS.md §2) is built.
ErrCode text_layout(World* w, StrView text, Rect_u16* out, u32 out_cap, u32* out_count);
