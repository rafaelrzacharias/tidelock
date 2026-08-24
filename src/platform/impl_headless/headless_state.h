#pragma once
// ---------------------------------------------------------------------------------------------
// headless_state.h - HeadlessState: the one struct every impl_headless/*.cpp shares via ctx.
//
// Spec: docs/PLATFORM.md §9 preamble ("the impl state is one struct allocated from the
//   platform's own VMemArena at init"), §9.4 (headless behaviour), §9.5 (init order).
// Purpose: private to impl_headless/ - platform_headless_init allocates one of these from the
//   16 MB platform arena and every sub-table's `ctx` points at it (or at a sub-member); no impl
//   TU has a static or namespace-scope mutable global (docs/CPP-SUBSET.md §1).
// Invariants: `tex`/`draw_log` slot management is hand-rolled here, not `SlotMap<T>`
//   (docs/CONTAINERS.md) - the containers lane has not landed; this is private bookkeeping, not
//   a second public container. `tex_gen[i]` starts at 0 (never issued) and is bumped only on
//   destroy, matching `Handle`'s "generation 0 never issued" rule.
// Determinism: none of this is sim state - platform is not symbol-audited
//   (`tl_register_lib(tl_platform_headless FALSE)`, `cmake/tier.cmake`).
// Threading: single-threaded from the caller's side except where a test spawns its own threads
//   through `ThreadApi` - those calls go straight to the OS, not through this struct's fields.
// Includes: foundation/{tl_types,rect,ring,handle,vmem_arena}.h, platform/{platform,entropy}.h.
//   Unlike platform.h, this internal header needs VMemArena/EntropyApi COMPLETE (arena_push,
//   entropy.fill), so it includes their real homes instead of relying on platform.h's opaque
//   forward declarations.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/rect.h"
#include "foundation/ring.h"
#include "foundation/handle.h"
#include "foundation/vmem_arena.h"
#include "platform/platform.h"
#include "platform/entropy.h"

// docs/PLATFORM.md §9.3 "draw": SlotMap<TexRec> cap 4096 - TexHandle is Handle<TexTag,12,4>, so
// 4096 is the index space, not a chosen limit.
enum : u32 { HEADLESS_MAX_TEX = 4096 };
// docs/PLATFORM.md §9.4: "a 65536-entry record ring".
enum : u32 { HEADLESS_DRAW_LOG_CAP = 65536 };
// docs/PLATFORM.md §9.3 "thread": ThreadRec[64], SemRec[256], MutexRec[256] (sdl3's table; the
// headless impl states "identical contracts and error codes", so the same caps apply).
enum : u32 { HEADLESS_MAX_THREADS = 64, HEADLESS_MAX_SEMS = 256, HEADLESS_MAX_MUTEXES = 256 };

// HeadlessDrawVerb and DrawCall are defined once, in headless_test_api.h (the test-facing
// contract) - this header includes it rather than redefining them, to avoid an ODR split.
#include "platform/impl_headless/headless_test_api.h"

struct HeadlessTexRec {
    u8 alive;
    u8 usage;    // TexUsage
    u8 fmt;      // PixelFmt
    u8 _pad0;
    u16 w, h;
    u32 pitch;   // streaming only: bytes per row
    u8* streaming_buf;  // non-null only while alive and usage == TEX_STREAMING
};

struct HeadlessThreadRec { u8 alive; ThreadFn fn; void* fn_ctx;
#ifdef _WIN32
    void* os_handle;   // HANDLE, kept as void* so this header stays OS-free
#else
    unsigned long long os_thread;  // pthread_t, widened - pthread_t's width is not fixed by POSIX
#endif
};
struct HeadlessSemRec { u8 alive; void* os_sem; };
struct HeadlessMutexRec { u8 alive; void* os_mutex; };

struct HeadlessState {
    VMemArena arena;      // the platform's own 16 MB arena (docs/PLATFORM.md §9.5 step 1)
    VMemApi vmem_table;   // stored here so its address is arena-owned, not a global
    EntropyApi entropy_table;

    // file (docs/PLATFORM.md §9.3 "file"): computed once at init, into the arena. Headless has no
    // SDL org/app scheme to derive a real pref path from, so both are the process's current
    // working directory - a documented simplification, not a spec'd distinction for this impl.
    StrView base_path, pref_path;

    // window (docs/PLATFORM.md §9.4 "window")
    i32 window_w, window_h;
    u8 fullscreen, vsync;

    // events (docs/PLATFORM.md §9.4 "events"): pump is a no-op; tests push directly via
    // headless_event_ring() (impl_headless/headless_test_api.h). `dropped_total` is NOT a
    // separate counter: nothing headless ever pops this ring (persistent-mode consumption is
    // peek-based, docs/CONTAINERS.md §4), so every `tail` advance is an overflow eviction and
    // `head - cap` (once `head > cap`) is exactly the drop count - events.cpp derives it.
    RingBuffer<RawEvent> events;

    // draw (docs/PLATFORM.md §9.4 "draw")
    HeadlessTexRec tex[HEADLESS_MAX_TEX];
    u16 tex_gen[HEADLESS_MAX_TEX];
    u32 tex_live_count;
    u8 has_target; TexHandle target;   // has_target == 0 means "the window" (null target)
    u8 has_clip;   Rect_i32 clip;
    RingBuffer<DrawCall> draw_log;

    // thread (docs/PLATFORM.md §9.3 "thread")
    u64 main_os_tid;   // the creating thread's id, captured at init; ThreadApi.is_main compares
    HeadlessThreadRec threads[HEADLESS_MAX_THREADS];
    u16 thread_gen[HEADLESS_MAX_THREADS];
    HeadlessSemRec sems[HEADLESS_MAX_SEMS];
    u16 sem_gen[HEADLESS_MAX_SEMS];
    HeadlessMutexRec mutexes[HEADLESS_MAX_MUTEXES];
    u16 mutex_gen[HEADLESS_MAX_MUTEXES];
};
