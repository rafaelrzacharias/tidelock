#pragma once
// ---------------------------------------------------------------------------------------------
// platform.h - PlatformApi: the one porting seam. Struct-of-fn-ptrs, resolved once at boot.
//
// Spec: docs/PLATFORM.md §0, §9 (this is the §9.2 contract, transcribed); docs/CANON.md
//   ("Platform extras settled during the spec pass").
// Purpose: window/events/draw/file/clock/vmem/entropy/threads + the appended CrashApi behind one
//   fn-ptr table, so engine code above this seam never sees SDL_* or an OS header. Two impls:
//   platform_sdl3_init (the game) and platform_headless_init (tests, Hovel, CI) - the headless
//   impl is the second implementation that proves the seam holds.
// Invariants: every fn-ptr table takes its own `void* ctx` first - no static mutable state
//   anywhere in an impl (docs/CPP-SUBSET.md §1); impl state is one struct allocated from the
//   platform's own VMemArena at init. `abi_version` is bumped only by appending a field/table,
//   never reordering (CrashApi's own append, abi 1, is the precedent). TexHandles are minted only
//   by DrawApi - the asset registry maps name -> TexHandle, never a second id space.
// Determinism: this header is included from `core/`, `render/`, `net/`, `app/`, `editor/` -
//   never from `sim/` or `foundation/` (docs/ARCHITECTURE.md §1 rule 2). `entropy` is an opaque
//   forward-declared pointer here (`struct EntropyApi;`) - the verb only appears once
//   `platform/entropy.h` is included, and that header is restricted to `net/`/`app/`
//   (docs/PLATFORM.md §5). No floats reach a sim path through this file; `f32` here (DrawVertex,
//   the wheel event) is legal because `platform/` is not a sim TU (docs/CPP-SUBSET.md §1).
// Threading: `draw` is main-thread-only (`TL_ASSERT(thread.is_main)` in the impl, docs/PLATFORM.md
//   §9.3); every other table may be called from any thread the impl itself creates.
// Includes: foundation/{tl_types,handle,strview,ring,rect}.h only - never an OS or SDL header
//   (those live inside impl_sdl3/impl_headless TUs, docs/PLATFORM.md caption).
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/handle.h"
#include "foundation/strview.h"
#include "foundation/ring.h"
#include "foundation/rect.h"
#include "foundation/span.h"

// Defined in entropy.h, whose include path is restricted to net/ and app/ (docs/PLATFORM.md §5).
// Left opaque here so including the contract never exposes the verb.
struct EntropyApi;
// Defined by the mem lane (docs/MEMORY.md §8.2). FileApi::read_all takes it by pointer only, so
// the contract header never needs it complete - the impl TUs that call arena_push do.
struct VMemArena;

enum : u32 { PLATFORM_ABI_VERSION = 1 };

typedef Handle<struct TexTag, 12, 4>    TexHandle;     // minted by the platform (the device owns textures); the asset registry maps name->TexHandle+refcount, no second handle space
typedef Handle<struct ThreadTag, 12, 4> ThreadHandle;
typedef Handle<struct SemTag, 12, 4>    SemHandle;
typedef Handle<struct MutexTag, 12, 4>  MutexHandle;
typedef Handle<struct WatchTag, 12, 4>  WatchHandle;

enum PixelFmt : u8 { PIXFMT_RGBA8 = 0 };       // bytes R,G,B,A in memory = u32 0xAABBGGRR little-endian (SDL_PIXELFORMAT_RGBA32)
enum TexUsage : u8 { TEX_STATIC = 0, TEX_STREAMING = 1, TEX_TARGET = 2 };

struct DrawVertex { f32 x, y; f32 u, v; u32 rgba; };   // 20 B; x,y in current-target pixels; u,v normalized [0,1]

enum ErrPlatform : u16 {
    ERR_PLATFORM_OK = 0, ERR_PLATFORM_INIT, ERR_PLATFORM_WINDOW, ERR_PLATFORM_RENDERER,
    ERR_PLATFORM_TEX_LIMIT, ERR_PLATFORM_TEX_BAD_ARG, ERR_PLATFORM_TEX_USAGE, ERR_PLATFORM_TEX_STALE, ERR_PLATFORM_TEX_LOCK,
    ERR_PLATFORM_FILE_NOT_FOUND, ERR_PLATFORM_FILE_IO, ERR_PLATFORM_FILE_TOO_LARGE, ERR_PLATFORM_PATH_TOO_LONG,
    ERR_PLATFORM_VMEM, ERR_PLATFORM_THREAD_LIMIT, ERR_PLATFORM_THREAD_CREATE, ERR_PLATFORM_WATCH_LIMIT, ERR_PLATFORM_UNSUPPORTED
};
// Name table for the log - one entry per ErrPlatform value above, same order.
constexpr const char* ERR_PLATFORM_NAMES[] = {
    "OK", "INIT", "WINDOW", "RENDERER", "TEX_LIMIT", "TEX_BAD_ARG", "TEX_USAGE", "TEX_STALE", "TEX_LOCK",
    "FILE_NOT_FOUND", "FILE_IO", "FILE_TOO_LARGE", "PATH_TOO_LONG",
    "VMEM", "THREAD_LIMIT", "THREAD_CREATE", "WATCH_LIMIT", "UNSUPPORTED"
};

typedef void (*ResizeFn)(void* ctx, i32 w, i32 h, i32 draw_w, i32 draw_h);
typedef void (*QuitFn)(void* ctx);

struct PlatformConfig {                       // 88 B: 3xStrView(16) @0 - i32x2 @48 - 3 ptrs @56 - u8x6 @80 - u16 @86
    StrView title, org, app;                  // pref_path = <os pref root>/org/app/
    i32 window_w, window_h;                   // logical px; 0,0 -> 1280x720
    ResizeFn on_resize; QuitFn on_quit; void* callback_ctx;   // the only platform->engine callbacks (§2)
    u8 fullscreen, vsync, resizable, high_dpi, software_renderer, _pad0;
    u16 event_ring_cap_log2;                  // default 10 -> 1024 RawEvents
};

struct WindowApi {
    void* ctx;
    void    (*size)(void* ctx, i32* w, i32* h);            // logical
    void    (*drawable_size)(void* ctx, i32* w, i32* h);   // pixels (high-DPI)
    ErrCode (*set_fullscreen)(void* ctx, u8 on);           // 1 = borderless desktop
    ErrCode (*set_vsync)(void* ctx, u8 on);
    void    (*set_title)(void* ctx, StrView);
    u8      (*has_focus)(void* ctx);
};

enum RawEventKind : u8 {
    EV_NONE = 0, EV_KEY, EV_MOUSE_MOVE, EV_MOUSE_BUTTON, EV_WHEEL, EV_TEXT, EV_PAD_AXIS, EV_PAD_BUTTON,
    EV_PAD_CONNECT, EV_WINDOW_RESIZE, EV_WINDOW_FOCUS, EV_QUIT
};

struct RawEvent {                 // 32 B
    u64 timestamp_ticks;          // ClockApi.ticks domain; all events of one pump share the pump's stamp
    u8 kind; u8 _pad0; u16 _pad1; u32 _pad2;
    union {
        struct { u32 scancode; u16 mods; u8 down; u8 repeat; } key;             // scancode = SDL_Scancode value verbatim (USB HID usage); mods bit0 shift - bit1 ctrl - bit2 alt - bit3 gui
        struct { i32 x, y, dx, dy; } mouse_move;                                // logical window px
        struct { i32 x, y; u8 button, down, clicks, _pad; } mouse_button;       // button 1 left - 2 middle - 3 right - 4 x1 - 5 x2
        struct { f32 dx, dy; i32 x, y; } wheel;                                 // f32 is legal here; the Live producer quantizes
        struct { char utf8[16]; } text;                                         // NUL-terminated, <= 15 bytes
        struct { u8 pad, axis; i16 value; } pad_axis;                           // -32768..32767
        struct { u8 pad, button, down, _pad; } pad_button;
        struct { u8 pad, connected; } pad_connect;
        struct { i32 w, h, draw_w, draw_h; } window_resize;
        struct { u8 gained; } window_focus;
    } u;
};

struct EventApi {
    void* ctx;
    u32 (*pump)(void* ctx, RingBuffer<RawEvent>* out);      // returns events appended this call
    u32 (*dropped_total)(void* ctx);
};

struct DrawApi {
    void* ctx;
    Result<TexHandle> (*texture_create)(void* ctx, u16 w, u16 h, u8 fmt, u8 usage);
    ErrCode (*texture_upload)(void* ctx, TexHandle, const void* pixels, u32 pitch);   // TEX_STATIC only, whole texture
    Result<u8*> (*texture_lock)(void* ctx, TexHandle, u32* pitch_out);                // TEX_STREAMING only, whole texture, write-only memory
    void    (*texture_unlock)(void* ctx, TexHandle);
    void    (*texture_size)(void* ctx, TexHandle, u16* w, u16* h);
    void    (*texture_destroy)(void* ctx, TexHandle);
    ErrCode (*set_target)(void* ctx, TexHandle);                 // null = window; TEX_TARGET only
    void    (*set_clip)(void* ctx, const Rect_i32*);             // null = none; target px
    void    (*clear)(void* ctx, u32 rgba);
    ErrCode (*draw_geometry)(void* ctx, TexHandle tex, const DrawVertex* v, u32 n, const u32* idx, u32 m);  // m % 3 == 0; tex null = untextured; alpha blend always (v0)
    void    (*present)(void* ctx);
    u32     (*live_textures)(void* ctx);
};

struct FileEntry { char name[120]; u32 size; u8 is_dir; u8 _pad0; u16 _pad1; };  // 128 B
typedef void (*WatchFn)(void* ctx, StrView path);

struct FileApi {
    void* ctx;
    Result<Span<u8>> (*read_all)(void* ctx, StrView path, VMemArena* arena);   // pushes len+1 bytes (trailing NUL); count = len; <= 1 GB
    ErrCode (*write_all)(void* ctx, StrView path, Span<const u8>);
    ErrCode (*write_atomic)(void* ctx, StrView path, Span<const u8>);           // §9.3
    ErrCode (*append)(void* ctx, StrView path, Span<const u8>);                 // log/TSV sinks
    u8      (*exists)(void* ctx, StrView path);
    Result<u32> (*enumerate)(void* ctx, StrView dir, FileEntry* out, u32 cap); // non-recursive; sorted bytewise by name; count > cap -> ERR_PLATFORM_FILE_TOO_LARGE
    StrView (*base_path)(void* ctx);  StrView (*pref_path)(void* ctx);          // with trailing '/'
    Result<WatchHandle> (*watch)(void* ctx, StrView dir, WatchFn, void* wctx); // dev; netcode/ship: ERR_PLATFORM_UNSUPPORTED
    void    (*unwatch)(void* ctx, WatchHandle);
};

struct ClockApi { void* ctx; u64 (*ticks)(void* ctx); u64 (*frequency)(void* ctx); u64 (*wall_unix_ms)(void* ctx); };

struct VMemApi {
    void* ctx;
    void*   (*reserve)(void* ctx, u64 bytes);
    ErrCode (*commit)(void* ctx, void* base, u64 bytes);
    ErrCode (*decommit)(void* ctx, void* base, u64 bytes);
    void    (*release)(void* ctx, void* base, u64 bytes);
    u32 page_size; u32 _pad0;
};

typedef void (*ThreadFn)(void* ctx);

struct ThreadApi {
    void* ctx;
    Result<ThreadHandle> (*create)(void* ctx, ThreadFn, void* tctx, StrView name, u32 stack_bytes /*0 -> 1 MB*/);
    void (*join)(void* ctx, ThreadHandle);
    Result<SemHandle> (*sem_create)(void* ctx, u32 initial); void (*sem_wait)(void* ctx, SemHandle); u8 (*sem_try_wait)(void* ctx, SemHandle);
    void (*sem_post)(void* ctx, SemHandle); void (*sem_destroy)(void* ctx, SemHandle);
    Result<MutexHandle> (*mutex_create)(void* ctx); void (*mutex_lock)(void* ctx, MutexHandle); void (*mutex_unlock)(void* ctx, MutexHandle); void (*mutex_destroy)(void* ctx, MutexHandle);
    void (*yield)(void* ctx); void (*sleep_ms)(void* ctx, u32); u32 (*core_count)(void* ctx); u8 (*is_main)(void* ctx);
};

typedef void (*CrashWriterFn)(void* ctx, u32 reason, u32 code, u64 fault_addr, const void* os_context);   // TOOLING §9.3.9

struct CrashApi { void* ctx; ErrCode (*install)(void* ctx, CrashWriterFn, void* wctx); void (*raise_fatal)(void* ctx, const char* msg); };

struct PlatformApi {
    u32 abi_version; u32 _pad0;
    WindowApi window; EventApi events; DrawApi draw; FileApi file; ClockApi clock;
    VMemApi vmem; const EntropyApi* entropy; ThreadApi thread; CrashApi crash;   // crash appended here (the §0 reserved slot rule), abi 1
    u8 is_headless; u8 _pad1[7];
};

// Builds the sdl3 PlatformApi: opens the window/renderer, wires vendor allocator hooks, probes
// entropy (fatal on failure). Returns a table valid until the matching *_shutdown call.
const PlatformApi* platform_sdl3_init(const PlatformConfig*);
// Reverses platform_sdl3_init in exact order (docs/PLATFORM.md §9.5): ENet/Luau are the caller's
// concern; this tears down ImGui, the renderer, the window, then releases every platform arena.
void platform_sdl3_shutdown(const PlatformApi*);
// Builds the headless PlatformApi: no window, validating draw stubs, real file/clock/vmem/
// entropy/thread. Links no SDL (docs/CANON.md "Platform extras settled during the spec pass").
const PlatformApi* platform_headless_init(const PlatformConfig*);
// Reverses platform_headless_init: releases every platform arena. No device to tear down.
void platform_headless_shutdown(const PlatformApi*);

static_assert(sizeof(DrawVertex) == 20, "docs/PLATFORM.md §9.2");
static_assert(sizeof(PlatformConfig) == 88, "docs/PLATFORM.md §9.2");
static_assert(sizeof(RawEvent) == 32, "docs/PLATFORM.md §9.2, §9.6 abi_and_layout");
static_assert(sizeof(FileEntry) == 128, "docs/PLATFORM.md §9.2");
static_assert(PLATFORM_ABI_VERSION == 1, "docs/CANON.md: CrashApi appended, abi 1");
static_assert(sizeof(ERR_PLATFORM_NAMES) / sizeof(ERR_PLATFORM_NAMES[0]) == ERR_PLATFORM_UNSUPPORTED + 1,
              "one name per ErrPlatform value, same order");
