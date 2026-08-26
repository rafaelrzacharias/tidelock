// text.cpp - the reserved stub. Spec: docs/RENDER2D.md §7, §9.1.
#include "render/text.h"

ErrCode text_layout(World* w, StrView text, Rect_u16* out, u32 out_cap, u32* out_count) {
    (void)w; (void)text; (void)out; (void)out_cap;
    *out_count = 0;
    return ERR_RENDER_UNSUPPORTED;
}
