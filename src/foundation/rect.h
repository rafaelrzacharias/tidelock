#pragma once
// ---------------------------------------------------------------------------------------------
// rect.h - Rect_f32 / Rect_i32 / Rect_u16: the three concrete rect structs.
//
// Spec: docs/RENDER2D.md §9.2 (pinned shape); docs/CANON.md ("Types").
// Purpose: one rect shape per storage width - render/camera/layout work in f32, platform/window
//   pixel geometry works in i32, packed UV rects work in u16. Not a template: three concrete
//   structs so they can never silently cross-convert.
// Invariants: fields are always `x, y, w, h` (origin + extent); a consumer derives `min = x,y`
//   and `max = x+w, y+h` itself - this header stores no derived accessor, and the type never
//   spells `top`/`bottom`. RENDER2D.md §9.2 additionally promises min/max/overlap helpers and an
//   X-macro generator; neither is needed by anything landing today (see the note below), so
//   neither ships here - adding either is the render2d lane's call, not a stopgap guess.
// Determinism: `Rect_f32` carries `f32` - render/editor/platform/tools only, banned in sim TUs.
//   `Rect_i32`/`Rect_u16` are integer-only and would be sim-safe alone, but the three share one
//   header, so the whole file is classified with the render-side non-det half of
//   `src/foundation/` (`TL_FOUNDATION_NONDET` in `src/foundation/CMakeLists.txt`) rather than
//   split across files for two structs nothing in `sim/` currently needs.
// Threading: none - value types, no state.
// Includes: foundation/tl_types.h only.
//
// Landed from the W1 platform lane (2026-08-24): PLATFORM.md §9's contract header
// (`DrawApi::set_clip`, `RawEvent`) needs `Rect_i32` and no lane's "Builds" column in
// docs/ROADMAP.md §2 claims `rect.h` - RENDER2D.md §9.2 is the only doc that pins its shape, and
// that lane starts in W3. Transcribed verbatim from RENDER2D.md §9.2's struct line only - same
// precedent as tl_assert.h landing from the fx lane (LESSONS.md). The render2d lane owns this
// file, and the min/max/overlap helpers §9.2 describes, from the moment it starts.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

struct Rect_f32 { f32 x, y, w, h; };
struct Rect_i32 { i32 x, y, w, h; };
struct Rect_u16 { u16 x, y, w, h; };

static_assert(__is_trivially_copyable(Rect_f32) && __is_trivially_copyable(Rect_i32) &&
              __is_trivially_copyable(Rect_u16), "");
static_assert(sizeof(Rect_f32) == 16 && sizeof(Rect_i32) == 16 && sizeof(Rect_u16) == 8, "");
