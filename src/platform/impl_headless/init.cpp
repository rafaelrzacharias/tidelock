// init.cpp - platform_headless_init/shutdown (docs/PLATFORM.md §9.5, headless steps 1,2,6,7,8
// only - no window/renderer, no dev tier). Links no SDL (docs/CANON.md "Platform extras").
#include "platform/platform.h"
#include "platform/os_vmem.h"
#include "platform/os_entropy.h"
#include "platform/impl_headless/headless_apis.h"

#include "foundation/tl_assert.h"
#include "foundation/hash.h"

namespace {

// CrashApi's OS half is step 5 (docs/PLATFORM.md §9.7 build order: "crash tables (OS half) - the
// writer arrives with TOOLING.md §9.3.9"); until then these are named, non-null, TL_FATAL stubs -
// the ROADMAP.md §0 rule-1 convention ("stubs that TL_FATAL('unimplemented')"), not a silent gap.
ErrCode crash_install_stub(void*, CrashWriterFn, void*) {
    TL_FATAL("PlatformApi.crash is not implemented yet (docs/PLATFORM.md §9.7 step 5, TODO.md)");
}
[[noreturn]] void crash_raise_fatal_stub(void*, const char*) {
    TL_FATAL("PlatformApi.crash is not implemented yet (docs/PLATFORM.md §9.7 step 5, TODO.md)");
}

}  // namespace

const PlatformApi* platform_headless_init(const PlatformConfig* config) {
    TL_ASSERT(config != nullptr);

    // Step 1 (docs/PLATFORM.md §9.5): build vmem (no state needed) -> reserve the platform arena.
    // `boot_table`/`boot_arena` are locals used only to bootstrap: state->vmem_table/state->arena
    // are the stable copies everything after this function returns actually uses.
    VMemApi boot_table;
    os_vmem_fill_table(&boot_table);

    VMemArena boot_arena;
    const ErrCode arena_err = vmem_arena_init(&boot_arena, "platform_headless_arena"_id,
                                               16u * 1024u * 1024u, 0u, &boot_table);
    TL_CHECK(arena_err == ERR_OK);

    HeadlessState* state = (HeadlessState*)arena_push(&boot_arena, sizeof(HeadlessState), alignof(HeadlessState));
    state->arena = boot_arena;
    state->vmem_table = boot_table;
    state->arena.os = &state->vmem_table;   // repoint off the about-to-die local

    // Step 7: entropy probe, fatal on failure (os_entropy_fill_table's fill() is already
    // TL_FATAL-on-failure internally, so a successful call here is the whole probe).
    os_entropy_fill_table(&state->entropy_table);
    { u8 probe[32]; state->entropy_table.fill(state->entropy_table.ctx, probe, 32u); }

    // Step 6: clock/thread/file tables (captured here: main thread id, base/pref paths).
    state->main_os_tid = headless_current_thread_id();
    headless_init_paths(state);

    state->window_w = config->window_w > 0 ? config->window_w : 1280;
    state->window_h = config->window_h > 0 ? config->window_h : 720;
    state->fullscreen = config->fullscreen;
    state->vsync = config->vsync;

    const u32 ring_log2 = config->event_ring_cap_log2 != 0u ? (u32)config->event_ring_cap_log2 : 10u;
    const u32 ring_cap = 1u << ring_log2;
    RawEvent* ev_data = (RawEvent*)arena_push(&state->arena, (u64)ring_cap * sizeof(RawEvent), alignof(RawEvent));
    state->events.data = ev_data;
    state->events.cap = ring_cap;
    state->events.overwrite_oldest = 1;

    DrawCall* dc_data = (DrawCall*)arena_push(&state->arena, (u64)HEADLESS_DRAW_LOG_CAP * sizeof(DrawCall), alignof(DrawCall));
    state->draw_log.data = dc_data;
    state->draw_log.cap = HEADLESS_DRAW_LOG_CAP;
    state->draw_log.overwrite_oldest = 1;   // never expected to fill in a frame; defensive, not a spec'd overflow policy

    // tex[]/tex_gen[]/threads[]/sems[]/mutexes[] and their *_gen arrays are already zero: fresh
    // arena pages are OS-zero (docs/PLATFORM.md §9.3 "vmem"), so `alive == 0` and every generation
    // reads 0 ("never issued") without a memset.

    PlatformApi* api = (PlatformApi*)arena_push(&state->arena, sizeof(PlatformApi), alignof(PlatformApi));
    api->abi_version = PLATFORM_ABI_VERSION;
    api->_pad0 = 0u;
    api->window = headless_window_api(state);
    api->events = headless_events_api(state);
    api->draw = headless_draw_api(state);
    api->file = headless_file_api(state);
    api->clock = headless_clock_api(state);
    api->vmem = state->vmem_table;
    api->entropy = &state->entropy_table;
    api->thread = headless_thread_api(state);
    api->crash = CrashApi{ state, crash_install_stub, crash_raise_fatal_stub };
    api->is_headless = 1u;
    for (u32 i = 0; i < 7u; ++i) { api->_pad1[i] = 0u; }
    return api;
}

void platform_headless_shutdown(const PlatformApi* api) {
    TL_ASSERT(api != nullptr);
    HeadlessState* state = (HeadlessState*)api->window.ctx;
    const VMemApi* os = state->arena.os;
    void* base = state->arena.base;
    const u64 reserved = state->arena.reserved;
    os->release(os->ctx, base, reserved);
}
