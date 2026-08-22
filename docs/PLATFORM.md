# Platform — the porting seam (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §9. Carries foundry's platform seam
> (struct-of-fn-ptrs, SDL3) and names the new seams the pivot needs: virtual memory, OS entropy,
> threads — each a fn-ptr table resolved once at boot, never a per-call virtual.
> **Owns:** `src/platform/platform.h` (the contract), `impl_sdl3/`, `impl_headless/`.
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
    FileApi     file;       // read_all(arena), write_all, exists, enumerate, base_path, pref_path, watch (dev)
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

---

## 6. Threads (DECIDED — primitives only; the job system is `JOBS.md`)

`ThreadApi { create(fn, ctx, name) → handle; join; semaphore_{create,wait,post}; mutex_{lock,
unlock}; yield; core_count }` over SDL threads (one portable path for Windows/Linux). Atomics are
**compiler builtins** wrapped in `foundation/atomic.h` (`__atomic_load_n` etc.; `<atomic>` is STL
and banned). No `thread_local`: a worker's index and scratch are passed explicitly.

---

## 7. Headless impl

No window; `draw` verbs are no-ops that still validate arguments (so render code paths run in
tests); `events.pump` is a no-op (the Script producer bypasses it); file/clock/vmem/entropy/thread
are the real OS implementations (shared TUs). A `--render=software` option for pixel goldens uses
SDL's software renderer in an offscreen surface — still the sdl3 impl, not headless.

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
- Platforms: Windows x86-64 (clang-cl), Linux x86-64 (Steam Deck), Linux aarch64 (Pi 4, cross-
  compiled). Console/mobile: not planned; the seam is where they would go.

---

## 9. Rulings (closed 2026-08-22 — nothing open)

- **R-1 `DrawApi` exposes geometry batches only** (vertex/index spans + texture + clip). One verb;
  the batcher emits quads for sprites, sim-view chunks, text and debug lines alike. A blit fast
  path is rejected: it would be a second verb for a cost no profile has shown.
- **R-2 Gamepad lands with its first consumer**, as bindings only (`INPUT.md` §2); the `RawEvent`
  union already reserves `pad_axis`/`pad_button` so the seam does not change then.

*Rev 1 — 2026-08-22.*
