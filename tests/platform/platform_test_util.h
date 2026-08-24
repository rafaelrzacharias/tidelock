#pragma once
// platform_test_util.h - a fresh headless PlatformApi per test (docs/TESTING.md §7 rubric #1).
// tests/ carries the io/process exemption of docs/TESTING.md §8 R-2, but this is a thin wrapper
// over the real public contract (platform_headless_init/shutdown) - not a test-only shortcut.
#include "platform/platform.h"

inline const PlatformApi* platform_test_init() {
    PlatformConfig cfg{};
    cfg.title = sv("tl_tests");
    cfg.org = sv("tidelock");
    cfg.app = sv("tests");
    cfg.window_w = 0;   // -> default 1280x720
    cfg.window_h = 0;
    cfg.on_resize = nullptr;
    cfg.on_quit = nullptr;
    cfg.callback_ctx = nullptr;
    cfg.fullscreen = 0; cfg.vsync = 0; cfg.resizable = 0; cfg.high_dpi = 0; cfg.software_renderer = 0;
    cfg._pad0 = 0;
    cfg.event_ring_cap_log2 = 0;   // -> default 1024
    return platform_headless_init(&cfg);
}

inline void platform_test_shutdown(const PlatformApi* api) {
    platform_headless_shutdown(api);
}
