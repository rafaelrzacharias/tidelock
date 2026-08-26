# Platform — the porting seam (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §10. Carries foundry's platform seam
> (struct-of-fn-ptrs, SDL3) and names the new seams the pivot needs: virtual memory, OS entropy,
> threads — each a fn-ptr table resolved once at boot, never a per-call virtual.
> **Owns:** `src/platform/platform.h` (the contract), `impl_sdl3/`, `impl_headless/`.
> Implementation spec: §9.
> **The only module that includes OS or SDL headers.** CI greps for `SDL_`/`windows.h`/`unistd.h`
> anywhere else.

---

## 0. Shape (DECIDED)

```cpp
struct PlatformApi {
    u32 abi_version;
    WindowApi   window;     // create/destroy, size, fullscreen, vsync, present hook
    EventApi    events;     // pump → RawEvent ring (input + window events)
    DrawApi     draw;       // device verbs for render2d: texture create/lock/unlock/destroy, draw_geometry, set_clip, clear, present
    FileApi     file;       // read_all(arena), write_all, write_atomic, append, exists, enumerate, base_path, pref_path, watch (dev) — full list §9.2
    ClockApi    clock;      // hires ticks + frequency
    VMemApi     vmem;       // reserve / commit / decommit / release
    EntropyApi  entropy;    // fill(buf, n) — sim-unreachable header (§5)
    ThreadApi   thread;     // create/join, semaphore, mutex, yield, core_count
    // RESERVED (append + bump): audio device, gamepad, clipboard, IME
};
const PlatformApi* platform_sdl3_init(const PlatformConfig*);
const PlatformApi* platform_headless_init(const PlatformConfig*);
```

Engine code above the seam speaks engine types: `TexHandle`, `RawEvent`, `Rect<i32>`, arena
pointers — never `SDL_Texture*`. Fixed per build → direct calls through the table, zero per-call
cost worth discussing. Two impls from v0: **sdl3** (the game) and **headless** (tests, Hovel,
CI) — the headless impl *is* the second implementation that proves the seam.

---

## 1. Window and presentation

SDL3 window + renderer (`SDL_Render` v0). `present()` is the one place the frame leaves the
engine. High-DPI: the window reports drawable size; `resolve_layout` (`RENDER2D.md` §2) does the
rest. Multiple windows reserved (ImGui viewports will want them in the editor tier).

---

## 2. Events (DECIDED)

`pump_events()` drains SDL into a `RingBuffer<RawEvent>` on main scratch: a tagged union
`{ kind: u8; timestamp_ticks: u64; union { key, mouse_move, mouse_button, wheel, text, pad_axis,
pad_button, window_resize, window_focus, quit } }`. Consumers: the Live input producer
(`INPUT.md` §4), ImGui (dev), engine window callbacks (resize → `resolve_layout`, quit → shutdown).
Direct callbacks exist only here — platform → engine — never for gameplay.

---

## 3. Files and clock

- Files: `read_all(path, arena) → Result<Span<u8>>` (whole-file into an arena; no streaming
  reads at v0), `write_all`, `write_atomic` (temp → fsync → rename; the durable checkpoint tier
  needs it), `enumerate(dir)`, `base_path`/`pref_path` (SDL), and a dev-only `watch(dir, callback)`
  for asset/script hot-reload. Paths are `StrView`; content roots are configured in `app/`.
- Clock: `ticks()`/`frequency()` over `SDL_GetPerformanceCounter`; `FRAME-LOOP.md` §1 is the only
  caller on the game path. Wall-clock date/time exists for log stamps and save headers only.

---

## 4. Virtual memory (DECIDED)

`VMemApi { reserve(bytes) → base; commit(base, bytes); decommit(base, bytes); release(base);
page_size }` over `VirtualAlloc(MEM_RESERVE/MEM_COMMIT)` / `mmap(PROT_NONE)` + `mprotect`/
`madvise(MADV_DONTNEED)`. ~150 lines per OS. Committed pages are zero by OS contract — the
foundation relies on that (`MEMORY.md` §1.1). Foundation receives this table at init; it never
includes an OS header.

---

## 5. Entropy (DECIDED — resolves NETCODE R12)

`entropy.fill(buf, n)` over `BCryptGenRandom` / `getrandom(2)`; failure is `TL_FATAL` (there is
no fallback to a weak source). Declared in `platform/entropy.h`, which only `src/net/` and
`app/` may include; it is absent from every sim lib's include path and the symbol audit fails on
the symbol (`CPP-SUBSET.md` §4). Feeds Monocypher keygen, session nonces, commit/reveal.
`platform.h` itself holds only an opaque `const EntropyApi*` (forward-declared), so including the
contract never exposes the verb (§9.2).

---

## 6. Threads (DECIDED — primitives only; the job system is `JOBS.md`)

`ThreadApi { create(fn, ctx, name) → handle; join; semaphore_{create,wait,post}; mutex_{lock,
unlock}; yield; core_count }` over SDL threads (one portable path for Windows/Linux). Atomics are
**compiler builtins** wrapped in `foundation/atomic.h` (`__atomic_load_n` etc.; `<atomic>` is STL
and banned). No `thread_local`: a worker's index and scratch are passed explicitly.

---

## 7. Headless impl

No window; `draw` verbs are no-ops that still validate arguments (so render code paths run in
tests); `events.pump` is a no-op (the Script producer bypasses it); vmem and entropy are the real
OS implementations in TUs shared with the sdl3 impl; file/clock/thread are real, OS-direct
implementations in `impl_headless/` so the headless exe links no SDL at all (§9.1, §9.4). A
`--render=software` option for pixel goldens uses SDL's software renderer in an offscreen
surface — still the sdl3 impl, not headless.

---

## 8. Vendoring and platforms (DECIDED)

- Vendored pure-C / single-file deps, compiled once in their own TUs, exempt from the subset
  internally, never leaking includes: SDL3, SDL_ttf, stb_image, stb_sprintf, ENet, Monocypher,
  rapidhash. C++ exceptions to the pure-C rule, confined to their own TUs with their own flags:
  **Luau** (built with `LUA_USE_LONGJMP=1` so it never throws; `-fno-exceptions` holds for the
  rest of the binary) and **Dear ImGui** (+ ImGuiColorTextEdit; dev tiers only).
- Every vendored lib's allocator hook points at a `mem_pool` (`MEMORY.md` §1.5):
  `SDL_SetMemoryFunctions`, `lua_newstate(alloc)`, `ImGui::SetAllocatorFunctions`,
  `enet_initialize_with_callbacks`, `STBI_MALLOC`.
- Platforms: the `CANON.md` target matrix (ruled 2026-08-25) — {Windows, Linux} × {x86-64, arm64},
  clang-cl on Windows, clang on Linux; machines (the PC, later the Deck) are physical instances,
  not the definition. Console/mobile: not planned; the seam is where they would go.

---

## 9. Implementation specification

Scope: the contract header, two implementations, vendor wiring, tests. `f32`/`f64`, `<math.h>`,
OS and SDL headers are legal inside `impl_*`/`os_*` TUs only (`CPP-SUBSET.md` §1); `platform.h`
includes nothing but `foundation/{tl_types,handle,strview,ring,rect,span,vmem_api,thread_api}.h`
(ruled 2026-08-26: `thread_api.h` joined the list — the `VMemApi` case verbatim, `JOBS.md` §6.2's
`Jobs` holds `ThreadHandle`s and cannot include `platform.h`). `VMemApi`
is defined once, in `foundation/vmem_api.h` — foundation is a leaf (`ARCHITECTURE.md` §1 rule 1)
and `vmem_arena.cpp` calls through the table without including `platform.h`, so that is the
struct's one foundation-visible home (`MEMORY.md` §8.2); `platform.h` includes it rather than
redefining it. Every fn-ptr takes its table's `void* ctx` first — no static mutable state
anywhere in the impls (`CPP-SUBSET.md` §1); the impl state is one struct allocated from the
platform's own `VMemArena` at init.

### 9.1 File layout — `src/platform/`

| File | Contents | Lib |
|---|---|---|
| `platform.h` | `PlatformApi` + sub-tables, `RawEvent`, `PlatformConfig`, `PixelFmt`, `TexUsage`, `DrawVertex`, handle typedefs, `ERR_PLATFORM_*`, `platform_sdl3_init/shutdown`, `platform_headless_init/shutdown` | both |
| `entropy.h` | `struct EntropyApi` definition — include path granted to `net/`, `app/` only (§5) | both |
| `platform_dev.h` | `PlatformDevApi` (ImGui backend hooks), `TL_DEV` only | sdl3 |
| `impl_sdl3/init.cpp` | `platform_sdl3_init`: §9.5 order; builds the table | `tl_platform_sdl3` |
| `impl_sdl3/window.cpp` `events.cpp` `draw.cpp` `file.cpp` `clock.cpp` `thread.cpp` | one table each, over SDL3 | sdl3 |
| `impl_sdl3/vmem.cpp` `entropy.cpp` | thin: point the table at the shared `os_*` functions | sdl3 |
| `impl_sdl3/imgui_backend.cpp` | `imgui_impl_sdl3` + `imgui_impl_sdlrenderer3` host (the only TU that sees both ImGui and SDL), `TL_DEV` only | sdl3 |
| `impl_headless/init.cpp` | `platform_headless_init` | `tl_platform_headless` |
| `impl_headless/window.cpp` `events.cpp` `draw.cpp` | validating stubs (§9.4) | headless |
| `impl_headless/file.cpp` `clock.cpp` `thread.cpp` | real, OS-direct (`#ifdef _WIN32` Win32 / POSIX branches in one TU each) — no SDL | headless |
| `impl_headless/vmem.cpp` `entropy.cpp` | thin over the shared `os_*` | headless |
| `os_win_vmem.cpp` / `os_posix_vmem.cpp` | `VirtualAlloc`/`mmap` implementation (one compiled per OS) | both |
| `os_entropy.cpp` | `BCryptGenRandom` / `getrandom(2)` | both |
| `os_file_atomic.cpp` | `write_atomic` (needs `fsync`/`FlushFileBuffers`, which SDL_IOStream lacks) — both impls point `file.write_atomic` here | both |
| `os_crash_win.cpp` / `os_crash_posix.cpp` | `CrashApi` (§9.2; consumer `TOOLING.md` §9.3.9) | both |

Exactly one platform lib links into an exe: `tidelock`, `tl_hovel` → sdl3; `tl_tests`,
`tl_driver`, `tl_gate0` → headless (`tl_tests` may be built a second time against sdl3 for
`--render=software` goldens). CI grep: `SDL_`, `windows.h`, `unistd.h`, `sys/mman.h`, `bcrypt.h`,
`imgui_impl_` appear under `src/` only in `src/platform/`.

**`#define NOMINMAX` precedes every `<windows.h>` in the tree** — `src/platform/` and `tests/`
alike (ruled 2026-08-24). `windows.h`'s raw `min`/`max` macros mangle any same-named declaration
below them, and `fx.h` declares free functions `min`/`max` (`FX-PALETTE.md`); a TU reaching both
fails with "too many arguments to function-like macro invocation" pointing at an `fx` template,
naming neither `windows.h` nor `min`/`max`. Renaming `fx`'s `min`/`max` was rejected — those
spellings are pinned across `FX-PALETTE.md` and `ALLOY.md`, and churning a doc-visible vocabulary
to dodge a Windows macro is the wrong trade. The grep above is what makes this enforceable: the
sites are enumerable. `tools/audit/includes.py` gate 7 fails any file that includes `<windows.h>`
with no `#define NOMINMAX` on an earlier line, and it is the one gate that walks `tests/` as well
as `src/` (`TESTING.md` §5).

### 9.2 Contract structs

```cpp
enum : u32 { PLATFORM_ABI_VERSION = 1 };
typedef Handle<TexTag, 12, 4>    TexHandle;     // minted by the platform (the device owns textures); the asset registry maps name→TexHandle+refcount, no second handle space
typedef Handle<ThreadTag, 12, 4> ThreadHandle;  typedef Handle<SemTag, 12, 4> SemHandle;
typedef Handle<MutexTag, 12, 4>  MutexHandle;   typedef Handle<WatchTag, 12, 4> WatchHandle;
enum PixelFmt : u8 { PIXFMT_RGBA8 = 0 };       // bytes R,G,B,A in memory = u32 0xAABBGGRR little-endian (SDL_PIXELFORMAT_RGBA32)
enum TexUsage : u8 { TEX_STATIC = 0, TEX_STREAMING = 1, TEX_TARGET = 2 };
struct DrawVertex { f32 x, y; f32 u, v; u32 rgba; };   // 20 B; x,y in current-target pixels; u,v normalized [0,1]
enum ErrPlatform : u16 { ERR_PLATFORM_OK = 0, ERR_PLATFORM_INIT, ERR_PLATFORM_WINDOW, ERR_PLATFORM_RENDERER,
    ERR_PLATFORM_TEX_LIMIT, ERR_PLATFORM_TEX_BAD_ARG, ERR_PLATFORM_TEX_USAGE, ERR_PLATFORM_TEX_STALE, ERR_PLATFORM_TEX_LOCK,
    ERR_PLATFORM_FILE_NOT_FOUND, ERR_PLATFORM_FILE_IO, ERR_PLATFORM_FILE_TOO_LARGE, ERR_PLATFORM_PATH_TOO_LONG,
    ERR_PLATFORM_VMEM, ERR_PLATFORM_THREAD_LIMIT, ERR_PLATFORM_THREAD_CREATE, ERR_PLATFORM_WATCH_LIMIT, ERR_PLATFORM_UNSUPPORTED };
    // module range assigned in tl_types.h's ErrCode table; constexpr name table ERR_PLATFORM_NAMES[]

typedef void (*ResizeFn)(void* ctx, i32 w, i32 h, i32 draw_w, i32 draw_h);   typedef void (*QuitFn)(void* ctx);
struct PlatformConfig {                       // 88 B: 3×StrView(16) @0 · i32×2 @48 · 3 ptrs @56 · u8×6 @80 · u16 @86; static_assert(sizeof == 88)
    StrView title, org, app;                  // pref_path = <os pref root>/org/app/
    i32 window_w, window_h;                   // logical px; 0,0 → 1280×720
    ResizeFn on_resize; QuitFn on_quit; void* callback_ctx;   // the only platform→engine callbacks (§2)
    u8 fullscreen, vsync, resizable, high_dpi, software_renderer, _pad0;
    u16 event_ring_cap_log2; };               // default 10 → 1024 RawEvents

struct WindowApi { void* ctx;
    void    (*size)(void* ctx, i32* w, i32* h);            // logical
    void    (*drawable_size)(void* ctx, i32* w, i32* h);   // pixels (high-DPI)
    ErrCode (*set_fullscreen)(void* ctx, u8 on);           // 1 = borderless desktop
    ErrCode (*set_vsync)(void* ctx, u8 on);
    void    (*set_title)(void* ctx, StrView);
    u8      (*has_focus)(void* ctx); };
enum RawEventKind : u8 { EV_NONE = 0, EV_KEY, EV_MOUSE_MOVE, EV_MOUSE_BUTTON, EV_WHEEL, EV_TEXT, EV_PAD_AXIS, EV_PAD_BUTTON,
                         EV_PAD_CONNECT, EV_WINDOW_RESIZE, EV_WINDOW_FOCUS, EV_QUIT };
struct RawEvent {                 // 32 B; static_assert(sizeof == 32)
    u64 timestamp_ticks;          // ClockApi.ticks domain; all events of one pump share the pump's stamp
    u8 kind; u8 _pad0; u16 _pad1; u32 _pad2;
    union {
        struct { u32 scancode; u16 mods; u8 down; u8 repeat; } key;             // scancode = SDL_Scancode value verbatim (USB HID usage); mods bit0 shift · bit1 ctrl · bit2 alt · bit3 gui
        struct { i32 x, y, dx, dy; } mouse_move;                                // logical window px
        struct { i32 x, y; u8 button, down, clicks, _pad; } mouse_button;       // button 1 left · 2 middle · 3 right · 4 x1 · 5 x2
        struct { f32 dx, dy; i32 x, y; } wheel;                                 // f32 is legal here; the Live producer quantizes
        struct { char utf8[16]; } text;                                         // NUL-terminated, ≤ 15 bytes
        struct { u8 pad, axis; i16 value; } pad_axis;                           // −32768..32767
        struct { u8 pad, button, down, _pad; } pad_button;
        struct { u8 pad, connected; } pad_connect;
        struct { i32 w, h, draw_w, draw_h; } window_resize;
        struct { u8 gained; } window_focus; } u; };
struct EventApi { void* ctx;
    u32 (*pump)(void* ctx, RingBuffer<RawEvent>* out);      // returns events appended this call
    u32 (*dropped_total)(void* ctx); };
struct DrawApi { void* ctx;
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
    u32     (*live_textures)(void* ctx); };
struct FileEntry { char name[120]; u32 size; u8 is_dir; u8 _pad0; u16 _pad1; };  // 128 B
typedef void (*WatchFn)(void* ctx, StrView path);
struct FileApi { void* ctx;
    Result<Span<u8>> (*read_all)(void* ctx, StrView path, VMemArena* arena);   // pushes len+1 bytes (trailing NUL); count = len; ≤ 1 GB
    ErrCode (*write_all)(void* ctx, StrView path, Span<const u8>);
    ErrCode (*write_atomic)(void* ctx, StrView path, Span<const u8>);           // §9.3
    ErrCode (*append)(void* ctx, StrView path, Span<const u8>);                 // log/TSV sinks
    u8      (*exists)(void* ctx, StrView path);
    Result<u32> (*enumerate)(void* ctx, StrView dir, FileEntry* out, u32 cap); // non-recursive; sorted bytewise by name; count > cap → ERR_PLATFORM_FILE_TOO_LARGE; a dir that does not exist → ERR_PLATFORM_FILE_NOT_FOUND, never an empty listing
    StrView (*base_path)(void* ctx);  StrView (*pref_path)(void* ctx);          // with trailing '/'
    Result<WatchHandle> (*watch)(void* ctx, StrView dir, WatchFn, void* wctx); // dev; netcode/ship: ERR_PLATFORM_UNSUPPORTED
    void    (*unwatch)(void* ctx, WatchHandle); };
struct ClockApi { void* ctx; u64 (*ticks)(void* ctx); u64 (*frequency)(void* ctx); u64 (*wall_unix_ms)(void* ctx); };
struct VMemApi  { void* ctx; void* (*reserve)(void* ctx, u64 bytes); ErrCode (*commit)(void* ctx, void* base, u64 bytes);
                  ErrCode (*decommit)(void* ctx, void* base, u64 bytes); void (*release)(void* ctx, void* base, u64 bytes); u32 page_size; u32 _pad0; };
                  // ^ defined in foundation/vmem_api.h (this struct, verbatim); platform.h includes it, does not redefine it (§9)
struct EntropyApi { void* ctx; void (*fill)(void* ctx, void* buf, u32 n); };   // defined in entropy.h; platform.h: `struct EntropyApi;`
typedef void (*ThreadFn)(void* ctx);
// ThreadHandle/SemHandle/MutexHandle/ThreadFn/ThreadApi are defined in foundation/thread_api.h
// (this struct, verbatim); platform.h includes it, does not redefine it — the VMemApi pattern
// (ruled 2026-08-26). CONTRACT the jobs wake path depends on: a sem_post observed by sem_wait on
// the same semaphore establishes happens-before (release on post, acquire on wait) — every OS
// primitive provides it; stated here because a woken worker must see the epoch published before
// the post (the silence-is-not-permission class, filed by the W1 mem review).
struct ThreadApi { void* ctx;
    Result<ThreadHandle> (*create)(void* ctx, ThreadFn, void* tctx, StrView name, u32 stack_bytes /*0 → 1 MB*/);
    void (*join)(void* ctx, ThreadHandle);
    Result<SemHandle> (*sem_create)(void* ctx, u32 initial); void (*sem_wait)(void* ctx, SemHandle); u8 (*sem_try_wait)(void* ctx, SemHandle);
    void (*sem_post)(void* ctx, SemHandle); void (*sem_destroy)(void* ctx, SemHandle);
    Result<MutexHandle> (*mutex_create)(void* ctx); void (*mutex_lock)(void* ctx, MutexHandle); void (*mutex_unlock)(void* ctx, MutexHandle); void (*mutex_destroy)(void* ctx, MutexHandle);
    void (*yield)(void* ctx); void (*sleep_ms)(void* ctx, u32); u32 (*core_count)(void* ctx); u8 (*is_main)(void* ctx); };
typedef void (*CrashWriterFn)(void* ctx, u32 reason, u32 code, u64 fault_addr, const void* os_context);   // TOOLING §9.3.9
struct CrashApi { void* ctx; ErrCode (*install)(void* ctx, CrashWriterFn, void* wctx); void (*raise_fatal)(void* ctx, const char* msg); };
struct PlatformApi { u32 abi_version; u32 _pad0;
    WindowApi window; EventApi events; DrawApi draw; FileApi file; ClockApi clock;
    VMemApi vmem; const EntropyApi* entropy; ThreadApi thread; CrashApi crash;   // crash appended here (the §0 reserved slot rule), abi 1
    u8 is_headless; u8 _pad1[7]; };
// platform_dev.h (TL_DEV)
struct PlatformDevApi { void* ctx; void (*imgui_init)(void* ctx, void* imgui_context); void (*imgui_new_frame)(void* ctx);
                        void (*imgui_render)(void* ctx); void (*imgui_shutdown)(void* ctx); };
const PlatformDevApi* platform_sdl3_dev_api(const PlatformApi*);   // null on headless
```
`foundation/atomic.h` is owned by `JOBS.md` §6.1, which carries the full API (verbs, widths,
orders) — this doc names no verbs (ruled 2026-08-26: one header had grown two incompatible
spellings across the two docs). No `<atomic>`, no `volatile`.

### 9.3 Per-API behaviour

| API | sdl3 behaviour |
|---|---|
| window | `SDL_CreateWindow(flags: RESIZABLE·HIGH_PIXEL_DENSITY per config)` + `SDL_CreateRenderer(software ? "software" : null)`; `set_vsync` → `SDL_SetRenderVSync`; `set_fullscreen` → `SDL_SetWindowFullscreen` borderless; `size` → `SDL_GetWindowSize`, `drawable_size` → `SDL_GetWindowSizeInPixels`; a resize fires `on_resize` (render re-runs `resolve_layout`) *and* an `EV_WINDOW_RESIZE` ring event |
| events | `pump`: (1) drain the watcher queue → `WatchFn` on this thread; (2) `stamp = clock.ticks()`; (3) `while SDL_PollEvent(&e)`: dev → `ImGui_ImplSDL3_ProcessEvent(&e)` first; translate → `RawEvent`; if ring full: pop oldest, `dropped++`; push. Quit → `on_quit` + `EV_QUIT`. Unmapped SDL events are skipped. Ring capacity `1 << event_ring_cap_log2`, lives on main scratch (the frame consumes it) |
| draw | main-thread-only (`TL_ASSERT(thread.is_main)` in debug). Texture table: `SlotMap<TexRec, TexHandle>` cap 4096 (`TexRec { SDL_Texture* t; u16 w, h; u8 fmt, usage; }`) — exhausted → `ERR_PLATFORM_TEX_LIMIT`; `w == 0 || h == 0 || w > 8192` → `TEX_BAD_ARG`; stale handle → `TEX_STALE`; `lock` on non-streaming / `set_target` on non-target / `upload` on non-static → `TEX_USAGE`. `texture_create`: `SDL_CreateTexture(RGBA32, access per usage)`, `SDL_SetTextureScaleMode(NEAREST)`, blend `SDL_BLENDMODE_BLEND`. `lock/unlock` → `SDL_LockTexture(null rect)`; `draw_geometry` → `SDL_RenderGeometryRaw` with stride 20, colour as `SDL_FColor` via the u8→float path the backend provides (`SDL_RenderGeometryRaw` takes `SDL_FColor*`: the impl converts into a scratch `SDL_FColor[n]` — the one per-call allocation, from the platform arena mark/reset); `set_clip` → `SDL_SetRenderClipRect`; `clear` → `SDL_SetRenderDrawColor` + `SDL_RenderClear`; `present` → `SDL_RenderPresent` |
| file | paths UTF-8, `/` separators, ≤ 1024 B (else `PATH_TOO_LONG`), copied to a stack buffer with a NUL. `read_all`: `SDL_IOFromFile` "rb" → size → `arena_push(len+1, 16)` → read loop → `[len] = 0`; `write_all`: "wb"; `append`: "ab"; `enumerate`: `SDL_EnumerateDirectory` into `out`, then `sort_u32_kv` over name hashes? — no: an insertion sort on the ≤ cap entries by `memcmp` (tools-grade, bounded); `base_path`/`pref_path`: `SDL_GetBasePath`/`SDL_GetPrefPath(org, app)` copied into the platform arena at init; `watch`: a thread polling `mtime` of each file in `dir` every 250 ms (SDL has no watcher; inotify/ReadDirectoryChangesW are the named upgrade), pushing changed paths into a mutex-guarded 64-entry queue drained by `pump` |
| `write_atomic` | `tmp = path + ".tmp." + pid`; write all; `FlushFileBuffers`/`fsync(fd)`; close; `MoveFileExW(tmp, path, REPLACE_EXISTING \| WRITE_THROUGH)` / `rename(2)` then `fsync(dirfd(parent))` on POSIX; any failure → delete `tmp`, return `FILE_IO`, the old file is untouched. Uses Win32/POSIX calls directly in `os_file_atomic.cpp` (SDL_IOStream has no fsync) |
| clock | `ticks` → `SDL_GetPerformanceCounter`; `frequency` → `SDL_GetPerformanceFrequency`; `wall_unix_ms` → `SDL_GetCurrentTime / 1e6` (ns → ms). Monotonic; the loop is the only game-path caller (§3) |
| vmem | `reserve` → `VirtualAlloc(null, n, MEM_RESERVE, PAGE_NOACCESS)` / `mmap(null, n, PROT_NONE, MAP_PRIVATE\|MAP_ANONYMOUS\|MAP_NORESERVE)`; `commit` → `VirtualAlloc(base, n, MEM_COMMIT, PAGE_READWRITE)` / `mprotect(RW)`; `decommit` → `VirtualFree(base, n, MEM_DECOMMIT)` / `madvise(MADV_DONTNEED)` + `mprotect(PROT_NONE)`; `release` → `VirtualFree(base, 0, MEM_RELEASE)` / `munmap(base, n)`. `base`/`n` must be page multiples (`TL_CHECK`). **Zero-fill guarantee:** a page first touched after `commit` — including a re-commit after `decommit` — reads as zeros on both OSes (Windows demand-zero pages; Linux `DONTNEED` discards private anonymous pages). Linux overcommit means `commit` cannot fail for lack of RAM — an OOM is a kill, accepted and stated. `page_size` = `GetSystemInfo().dwPageSize` / `sysconf(_SC_PAGESIZE)` (4096 on x86-64 and mainstream aarch64 Linux; 16 K pages are tolerated by reading it) |
| entropy | `BCryptGenRandom(null, buf, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)` / `getrandom(buf, n, 0)` looped until `n` bytes; any failure → `TL_FATAL("entropy unavailable")`. Probed once at init with 32 bytes |
| thread | `SDL_CreateThreadWithProperties` (name, stack) → table `ThreadRec[64]`, `SemRec[256]` (`SDL_Semaphore*`), `MutexRec[256]` (`SDL_Mutex*`), exhausted → `THREAD_LIMIT`; `join` → `SDL_WaitThread` + slot free; `yield` → `SDL_Delay(0)`; `sleep_ms` → `SDL_Delay`; `core_count` → `SDL_GetNumLogicalCPUCores`; `is_main` compares `SDL_GetCurrentThreadID` with the id captured at init |
| crash | `install` registers the writer; `os_crash_win.cpp`: `SetUnhandledExceptionFilter`; `os_crash_posix.cpp`: `sigaction` on SEGV/BUS/ILL/FPE/ABRT with `SA_SIGINFO\|SA_ONSTACK` on a 64 KB `sigaltstack`; `raise_fatal` → Windows `RaiseException(0xE0544C46)` / POSIX `abort()`. Mechanism in `TOOLING.md` §9.3.9; the platform owns the OS half only |

### 9.4 Headless impl

| API | Behaviour |
|---|---|
| window | `size`/`drawable_size` return `config.window_w/h` (default 1280×720); `set_*` return OK and record; `has_focus` = 1 |
| events | `pump` returns 0 and does nothing; `dropped_total` = 0. Tests inject events by pushing into the ring directly |
| draw | **validating stub:** the same `SlotMap<TexRec>` and every argument check as sdl3 (limits, usage, stale, `m % 3`, `n ≤ 65536`, null vertex with `n > 0`), no device. Streaming textures own a CPU buffer (`w·h·4` from the platform arena) so `lock` returns real writable memory and tests can read it back. Every verb appends `DrawCall { u8 verb; u8 _pad0; u16 tex; u32 n, m; Rect_i32 clip; u32 rgba; }` (32 B) to a 65536-entry record ring readable by `tests/render/present_descriptor` via `headless_draw_log(const PlatformApi*) → Span<const DrawCall>`; cleared by `present` |
| file, clock, thread | real, OS-direct (`CreateFileW`/`open`, `QueryPerformanceCounter`/`clock_gettime(MONOTONIC)`, `CreateThread`/`pthread_create`, `CreateSemaphoreW`/`sem_init`, `SRWLOCK`/`pthread_mutex`); identical contracts and error codes; `watch` → `ERR_PLATFORM_UNSUPPORTED`. **Every Win32 file call is the `W` entry point**, never `*A`: the `A` family decodes its argument in the process ANSI code page, not UTF-8, so a §9.3 UTF-8 path with any byte ≥ 0x80 names a different file — and `FindFirstFileA` hands names BACK through that code page, which would make §9.2's "sorted bytewise by name" a function of the machine's locale. `os_path_win.cpp` is the one UTF-8 ⇄ UTF-16 boundary (`MB_ERR_INVALID_CHARS`/`WC_ERR_INVALID_CHARS`: malformed input is refused, never substituted); an `enumerate` entry whose name does not fit `FileEntry::name` is skipped, not truncated, because truncated UTF-8 no longer names the file |
| vmem, entropy, crash | the shared `os_*` TUs — bit-identical behaviour to sdl3 |
| `base_path` / `pref_path` | Both are the process's **current working directory** with a trailing `/`. Headless has no SDL `org`/`app` pref-root scheme to derive the §9.2 `<os pref root>/org/app/` layout from, and inventing one would put test output somewhere a CI job cannot find. A documented simplification of this impl, not a contract change: the sdl3 impl answers §9.2 |
| `read_all` arena cost | Pushes **`align16(len+1)`**, not `len+1`: `arena_push` aligns a push's START, not its SIZE (`MEMORY.md` §8.2), so a file whose length is not already a multiple of 16 would otherwise leave `used` unaligned and break §9.6's `read_all_contract` growth clause. `Span.count` is still `len` |
| stale handles | The verbs that RETURN a code report one (`TEX_STALE`, `TEX_USAGE`). The void-returning verbs cannot, so they split: **use** verbs `TL_FATAL` on a handle that does not resolve — `sem_wait`/`sem_post`/`sem_try_wait`, `mutex_lock`/`mutex_unlock`, `texture_unlock` (also on a non-streaming texture, the case `lock` refuses with `TEX_USAGE`) — because a quiet return leaves the caller believing it holds a lock, or posted a signal, that it did not, and the damage surfaces far from the dangling handle. **Release** verbs (`join`, `sem_destroy`, `mutex_destroy`, `texture_destroy`) stay no-ops on a null or already-released handle: double-release on a shutdown path is ordinary. `texture_size` is the ASK verb and answers 0×0 on a stale handle — an answer, not a swallowed error |
| slot tables | `tex`/`thread`/`sem`/`mutex` are hand-rolled arrays with `Handle`'s generation math until `SlotMap<T>` lands (`CONTAINERS.md`). Every handle here is `Handle<_,12,4>` - four generation bits, `GEN_MAX` 15 - so the release path's **generation-wrap policy is wrap-to-1, never to 0** (`MEMORY.md` §3 leaves the policy per domain; Entity's quarantine is the ECS's, not this seam's). A bare `++gen` overflows on a slot's 16th reuse and issues a handle that reads back generation 0 - stale on arrival, and the null handle on slot 0. ABA after 15 reuses of one slot is accepted and stated |
| dev | `platform_sdl3_dev_api` is absent; `is_headless = 1` |

### 9.5 Vendor hook wiring and init order

Pools (`MEMORY.md` §1.5): `pool_vendor` (SDL + ImGui + stb + FreeType, 64 MB reserve), `pool_luau_sim`,
`pool_luau_ui` (64 MB each, owned by `LUAU-LAYER`), `pool_enet` (16 MB, owned by `net/`). All
`pool_*` calls live in `src/vendor_glue/` (the one folder allowed a static pool pointer, for the
`STBI_MALLOC` compile-time macro).

| Lib | Hook | Pool |
|---|---|---|
| SDL3 | `SDL_SetMemoryFunctions(tl_sdl_malloc, tl_sdl_calloc, tl_sdl_realloc, tl_sdl_free)` — **before** `SDL_Init` | `pool_vendor` |
| Dear ImGui | `ImGui::SetAllocatorFunctions(tl_imgui_alloc, tl_imgui_free)` — before `CreateContext` (no `user_data`; the adaptor closes over `pool_vendor()` directly) | `pool_vendor` |
| stb_image / stb_sprintf | `#define STBI_MALLOC/REALLOC/FREE` → `tl_stbi_*` (stb_sprintf allocates nothing) | `pool_vendor` |
| Luau | `lua_newstate(glue_luau_alloc, &pool_luau_x)` per VM | per VM |
| ENet | `enet_initialize_with_callbacks(ENET_VERSION, &{tl_enet_malloc, tl_enet_free, tl_enet_no_memory → TL_FATAL})` | `pool_enet` |
| SDL_ttf | its own `SDL_malloc`/`SDL_free` call sites inherit SDL3's hook; has no adaptor `.cpp` of its own | `pool_vendor` |
| FreeType | no runtime allocator-registration API SDL_ttf's `TTF_Init()` exposes — hooked at FreeType's own platform-customization seam instead: `builds/<platform>/ftsystem.c`'s `ft_alloc`/`ft_realloc`/`ft_free`/`FT_New_Memory` call `tl_freetype_alloc`/`realloc`/`free` (`src/vendor_glue/freetype_glue.cpp`), a declared verbatim deviation (`vendor/VERSIONS`' freetype row) | `pool_vendor` |
| Monocypher, rapidhash | allocate nothing | — |

`platform_sdl3_init(config)` order: (1) build `vmem` (no state needed) → allocate the platform
arena (16 MB reserve) and the impl state; (2) `mem_pool_init(&pool_vendor, &vmem, 64 MB)`;
(3) `SDL_SetMemoryFunctions`; (4) `SDL_Init(VIDEO | EVENTS | GAMEPAD)` → `ERR_PLATFORM_INIT`;
(5) window + renderer → `ERR_PLATFORM_WINDOW` / `_RENDERER`; (6) clock/thread/file tables (capture
main thread id, `base_path`, `pref_path`); (7) entropy probe (fatal); (8) crash install (writer
null until `app/` registers one); (9) dev: `ImGui::SetAllocatorFunctions`, `CreateContext`,
`ImGui_ImplSDL3_InitForSDLRenderer`, `ImGui_ImplSDLRenderer3_Init`; (10) return the table.
Luau VMs and ENet initialize later from their own modules, in `app/`'s wiring order, after this
call. Shutdown is the exact reverse: ENet deinit → Luau `lua_close` → ImGui backend shutdown →
`DestroyContext` → `TL_LOG_WARN` if `live_textures() != 0` then destroy all → renderer → window →
`SDL_Quit` → `mem_pool` budget report → `vmem.release` of every pool and the platform arena.
Headless: steps 1, 2 (pools still exist for Luau/stb), 6, 7, 8 only.

### 9.6 Tests — `tests/platform/` (in `tl_tests`, both impls where applicable)

| Test | Asserts |
|---|---|
| `vmem_reserve_commit` | reserve 1 GB succeeds; commit first/last page; every byte of a committed page reads 0; write, decommit, re-commit → reads 0 again; `release` then reserve again at any address; non-page-multiple → `TL_CHECK` fatal (child process) |
| `vmem_page_size` | `page_size` is a power of two ≥ 4096 and equals the OS report |
| `write_atomic_crash_safety` | a child process writes `A` atomically, then is killed (`TerminateProcess`/`SIGKILL`) by the parent at each of three instrumented points (after temp write, after fsync, before rename — via an env-var hook in the impl, dev only); after each kill the target file is intact (`A` or the previous content, never torn); no stray `.tmp.*` after a successful call |
| `read_all_contract` | missing → `FILE_NOT_FOUND`; 0-byte file → `count 0` and a NUL at `[0]`; 10 MB round-trip equals; path > 1024 → `PATH_TOO_LONG`; arena `used` grows by exactly `align16(len+1)` |
| `enumerate_sorted` | 50 files in random creation order come back bytewise-sorted; `cap` smaller than count → `FILE_TOO_LARGE` |
| `event_ring_overflow` | headless: push `cap + 37` events; ring holds the last `cap`, `dropped_total == 37`, oldest surviving event is #37 |
| `headless_draw_validates` | each bad argument class returns its named code; a valid lock/write/unlock round-trips pixels; the call log records verbs in order; 4097th texture → `TEX_LIMIT`; destroyed handle → `TEX_STALE` |
| `entropy_nonrepeat` | 1000 × 32-byte fills: no two equal, no all-zero, byte histogram within 4σ of uniform |
| `thread_primitives` | 8 threads × 100k `atomic_add32` == 800k; semaphore ping-pong 10k rounds; mutex-guarded counter exact; `join` returns after the fn; 65th thread → `THREAD_LIMIT`; `core_count ≥ 1`; `is_main` true on main, false in a worker |
| `clock_monotonic` | 10k consecutive `ticks()` never decrease; `frequency ≥ 1e6` |
| `abi_and_layout` | `sizeof(RawEvent) == 32`, `sizeof(DrawVertex) == 20`, `abi_version == 1`, every fn-ptr non-null on both impls (except dev-only ones in `netcode`) |

### 9.7 Build order and done criteria

1. `platform.h` (every struct, every `static_assert`), `os_*_vmem.cpp`, `os_entropy.cpp`, headless `init/file/clock/thread` → `vmem_*`, `entropy_nonrepeat`, `thread_primitives`, `read_all_contract`, `clock_monotonic`. **Foundation can now allocate** (`MEMORY.md` §1.1 depends on this step).
2. headless `window/events/draw` stubs + call log → `event_ring_overflow`, `headless_draw_validates`, `abi_and_layout`. `tl_tests` links.
3. `write_atomic` + `enumerate` on both impls → `write_atomic_crash_safety`, `enumerate_sorted`.
4. sdl3 `init/window/events/draw/clock/thread/file` + vendor hooks → `tidelock` opens a window, clears, presents at vsync; the allocator shim reports zero CRT mallocs per frame with SDL routed through `pool_vendor`.
5. dev: `imgui_backend.cpp` + `PlatformDevApi`; `watch` thread. `crash` tables (OS half) — the writer arrives with `TOOLING.md` §9.3.9.
**Done (v0):** steps 1–4 green on Windows and Linux x86-64; the CI grep for OS/SDL headers outside `src/platform/` passes; `tl_tests` runs the whole suite against headless in < 5 s; the aarch64 legs build and run natively in CI (`TESTING.md` §4; was "Pi cross-build links" before the 2026-08-25 ruling).

---

## 10. Rulings (closed 2026-08-22 — nothing open)

- **R-1 `DrawApi` exposes geometry batches only** (vertex/index spans + texture + clip). One verb;
  the batcher emits quads for sprites, sim-view chunks, text and debug lines alike. A blit fast
  path is rejected: it would be a second verb for a cost no profile has shown.
- **R-2 Gamepad lands with its first consumer**, as bindings only (`INPUT.md` §2); the `RawEvent`
  union already reserves `pad_axis`/`pad_button` so the seam does not change then.

*Rev 1 — 2026-08-22.*
