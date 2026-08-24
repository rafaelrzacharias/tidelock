// window.cpp - the headless WindowApi (docs/PLATFORM.md §9.4 "window": size/drawable_size return
// config.window_w/h; set_* return OK and record; has_focus == 1).
#include "platform/impl_headless/headless_apis.h"

namespace {

void hw_size(void* ctx, i32* w, i32* h) {
    HeadlessState* s = (HeadlessState*)ctx;
    if (w) { *w = s->window_w; }
    if (h) { *h = s->window_h; }
}

void hw_drawable_size(void* ctx, i32* w, i32* h) {
    hw_size(ctx, w, h);   // no DPI scaling without a real display
}

ErrCode hw_set_fullscreen(void* ctx, u8 on) {
    ((HeadlessState*)ctx)->fullscreen = on;
    return ERR_OK;
}

ErrCode hw_set_vsync(void* ctx, u8 on) {
    ((HeadlessState*)ctx)->vsync = on;
    return ERR_OK;
}

void hw_set_title(void*, StrView) {
    // recorded nowhere - nothing reads a headless window title (docs/PLATFORM.md §9.4)
}

u8 hw_has_focus(void*) {
    return 1u;
}

}  // namespace

WindowApi headless_window_api(HeadlessState* s) {
    return WindowApi{ s, hw_size, hw_drawable_size, hw_set_fullscreen, hw_set_vsync, hw_set_title, hw_has_focus };
}
