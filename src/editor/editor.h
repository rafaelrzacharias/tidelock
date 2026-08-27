#pragma once
// ---------------------------------------------------------------------------------------------
// editor.h - Editor: the panel table, selection, capture mask, dev arena; editor_init/frame/
//   shutdown.
//
// Spec: docs/TOOLING.md §1 (the shell, design), §9.1 (this file's row in the file layout table),
//   §9.3.7 (the capture-mask publish algorithm); docs/ROADMAP.md §2 (editor lane).
// Purpose: one `Editor` per process (dev tiers only - `src/editor/`'s whole directory is excluded
//   from netcode/ship by `src/editor/CMakeLists.txt`'s empty `INTERFACE` branch, so nothing here
//   needs its own `#if TL_DEV`). `editor_register_panel` builds the `{ name, draw_fn,
//   default_open }` table `TOOLING.md` §1 describes ("menus are data"); `editor_frame` is the
//   per-render-frame entry point `app/wiring.cpp` (W4, not built) calls after the platform's
//   `PlatformDevApi.imgui_new_frame` and before `imgui_render`.
// Invariants: `EDITOR_MAX_PANELS` (16, covers every panel §9.4's table names plus headroom)
//   registered panels, duplicate name is TL_FATAL (matching every other registration door in the
//   tree - `cvar_register`/`console_register`/`TL_COMPONENT`'s shared precedent, init-time
//   misconfiguration, not a runtime error). `sel` is single-select through v0 (`TOOLING.md` §10
//   R-1); multi-select is out of scope until that ruling reopens.
// Determinism: none of this is sim state - the dev arena is never registered, never hashed,
//   never snapshotted (`docs/CPP-SUBSET.md` §9 R-4's tooling-plane reasoning, though `Editor` is
//   caller-owned rather than a static, matching `cvar.h`/`console.h`'s shape, not RR-7's
//   namespace-scope exemption). `editor_frame` publishes the capture mask (`TOOLING.md` §9.3.7)
//   but performs no sim mutation itself - every inspector edit is a command (`core/commands.h`),
//   recorded through `world_set_field_cmd`, not written here.
// Threading: none - one `Editor`, the render thread, single-threaded (v0 has no render thread of
//   its own yet either).
// Status: `editor_frame`'s ImGui NewFrame/panel-draw/Render sequence is a **STUB -
//   TL_FATAL("unimplemented")**. It is blocked on `PlatformDevApi` (`docs/PLATFORM.md` §9.2/§9.7
//   step 5: `imgui_init`/`imgui_new_frame`/`imgui_render` hooks), which does not exist yet
//   (`src/render/backend_sdl.cpp`'s own comment: "no PlatformDevApi exists yet") - that is
//   `platform/`'s file, out of this lane's cone (`docs/ROADMAP.md` §0 rule 2). `editor_init`/
//   `editor_register_panel`/`editor_shutdown` need no platform at all and are implemented now;
//   a panel's own `draw_fn` (Log, Console, Inspector, Profiler, Probes, World) is real ImGui
//   widget code that runs headless in tests (`docs/TOOLING.md` §9.5: "panel tests run ImGui
//   headless via a null backend" - `ImGui::NewFrame`/widget calls driven directly against a
//   manually-set `io`, no platform backend) and does not itself need `PlatformDevApi` either;
//   only the real windowed integration inside `editor_frame` does.
// Includes: foundation/tl_types.h, foundation/vmem_arena.h, core/world.h (Entity, World* for
//   panel draw functions), editor/console.h (ConsoleState - the Console panel's registry/history
//   is real, caller-owned state per console.h's own contract, not a hidden static the way
//   foundation/'s RR-7 tooling rings are; `Editor` is that caller, the same role it already plays
//   for `sel`).
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/vmem_arena.h"
#include "core/world.h"
#include "editor/console.h"

struct Editor;

// One panel's draw function: called once per open frame with the panel's own state reached
// through `ed`/`w` (docs/TOOLING.md §9.4's "data source" column). Owns its own
// `ImGui::Begin(name, ...)`/`End()` - `editor_frame` calls every open panel's `draw_fn` and
// nothing more, no per-panel window boilerplate of its own (settled building the first panel,
// `editor/log_panel.cpp`). That is also what makes a panel testable directly, with no `Editor`/
// `editor_frame` involved (`docs/TOOLING.md` §9.5's "panels run ImGui headless via a null
// backend").
typedef void (*PanelDrawFn)(Editor* ed, World* w);

enum { EDITOR_MAX_PANELS = 16, EDITOR_PANEL_NAME_CAP = 32 };

// docs/TOOLING.md §1: "panels registered in a table ({ name, draw_fn, default_open }) - menus
// are data." `open` is the CURRENT visibility (toggled from the shell's menu at runtime);
// `default_open` is only the value `open` starts at.
struct Panel {
    char name[EDITOR_PANEL_NAME_CAP];
    PanelDrawFn draw_fn;
    bool default_open;
    bool open;
    u8 _pad0[6];
};

// core module's editor sub-range (0x038x; see core/cvar.h's contract block for the rest of the
// 0x03xx block's layout).
constexpr ErrCode ERR_EDITOR_TABLE_FULL = (ErrCode)0x0380;   // EDITOR_MAX_PANELS already registered (TL_FATAL site)
constexpr ErrCode ERR_EDITOR_DUPLICATE  = (ErrCode)0x0381;   // panel name already registered (TL_FATAL site)
constexpr ErrCode ERR_EDITOR_LOCKSTEP   = (ErrCode)0x0382;   // an edit refused under a lockstep session (docs/TOOLING.md §9.3.4)

struct Editor {
    Panel panels[EDITOR_MAX_PANELS];
    u32   panel_count;
    Entity sel;          // the single-select inspector target (docs/TOOLING.md §10 R-1)
    u32   capture_mask;  // published every frame (docs/TOOLING.md §9.3.7): bit0 mouse, bit1 kbd
    VMemArena dev_arena; // permanent, non-registered: panel state (never sim state)
    void* imgui_ctx;     // ImGuiContext* - opaque here so this header never includes <imgui.h>
    ConsoleState console;                 // the Console panel's registry + history (console.h)
    char    console_input[CONSOLE_LINE_CAP];       // the live input line, edited in place by ImGui
    char    console_last_reply[256];               // the most recent ConsoleFn's reply text
    ErrCode console_last_err;                       // ERR_OK or the most recent dispatch/exec failure
    u64   prof_view_frame;  // Profiler panel (B-1 fix, 2026-08-27): the ABSOLUTE ProfFrame::frame
                             // pinned while paused (from `tl_prof_ring_at(0)->frame` the moment
                             // `prof_view_frame_valid` goes false->true). Meaningful only when
                             // `prof_view_frame_valid` is set. `prof_view_slot` below is
                             // `slots_back` - relative to the newest completed frame - so it is NOT
                             // stable under an advancing ring; recording a `slots_back` offset on
                             // pause and reusing it every draw is exactly the bug this field
                             // replaces (a paused panel silently showed a different frame each
                             // render, TOOLING.md §9.4). Each draw re-derives the current
                             // `slots_back` for `prof_view_frame` by scanning the ring
                             // (profiler_panel.cpp); once the frame ages out of the ring (evicted
                             // past PROF_RING_FRAMES back), the panel falls back to the oldest
                             // still-live frame and says so.
    u32   prof_view_slot;   // the Profiler panel's CURRENT `tl_prof_ring_at` `slots_back` for
                             // `prof_view_frame` - recomputed every draw while paused, never itself
                             // the source of truth; always 0 (the latest frame) while unpaused.
    u8    prof_paused;      // Profiler panel: true = hold on prof_view_frame; false = always slot 0
    u8    prof_view_frame_valid;  // `prof_view_frame` holds a real pinned frame. False on the first
                                    // draw after pausing (or right after init) - the panel pins
                                    // fresh on that draw. Set false again on unpause so the NEXT
                                    // pause re-pins to the then-current newest frame rather than a
                                    // stale one (a checkbox toggle's true->false->true is the only
                                    // real-usage path here, which headless tests cannot simulate -
                                    // this flag is what makes the pin observable/drivable directly,
                                    // matching `world_arena_hash_valid`'s identical shape below).
    u64*  world_arena_hash_cur;       // World panel: MAX_ARENAS u64s, dev_arena-backed, lazily
                                        // allocated on first "rehash arenas" click (never a
                                        // per-frame cost - registry_hash_all rehashes every
                                        // HASHED arena's full [base,used), which scales with
                                        // world size, so this is explicit-click, not every-frame)
    u64*  world_arena_hash_prev;      // the set from the click before this one, for the panel's
                                        // own "changed since last rehash" diff - null until the
                                        // second click (world_arena_hash_have_prev below)
    u8    world_arena_hash_valid;     // world_arena_hash_cur holds a real computed set
    u8    world_arena_hash_have_prev; // world_arena_hash_prev holds a real previous set to diff
    u8    initialized;
    u8    _pad0[5];   // measured trailing space, B-8 2026-08-27: [4] left one byte of implicit
                       // tail padding unnamed (`sizeof(Editor)` doesn't shrink from naming it -
                       // the byte is already there - but a struct that declares `_pad0` at all
                       // asserts it accounts for every trailing byte, and this one didn't).
};
// Editor is a 55 KB stack local in 30 test bodies (mostly ConsoleState's own CONSOLE_TABLE_CAP *
// sizeof(ConsoleCmd) = 512 * 72). This lane has already paid for three stack-overflow defects
// from an oversized stack local (LESSONS.md) - a size guard is the cheap insurance the day
// CONSOLE_TABLE_CAP/CONSOLE_LINE_CAP grows and all 30 grow silently with it (B-8, 2026-08-27).
static_assert(sizeof(Editor) == 55704, "editor.h B-8: Editor grew/shrank - re-measure and update, "
              "or switch to a <= bound if pinning the exact size stops being worth it");

// Zero-initializes `ed`, reserves `dev_arena_reserve` bytes (0 = a documented default, TODO.md -
// no consumer has sized one yet) for the dev arena. Does NOT create an ImGui context (that is
// `editor_frame`'s first call's job, gated on `PlatformDevApi` per this header's Status note).
// `os` IS CAPTURED, NOT COPIED: forwarded straight into `vmem_arena_init(&ed->dev_arena, ...,
// os)`, whose own contract comment (`foundation/vmem_arena.h`) is the authority on this - `os`
// must outlive `ed`. RR-41 (2026-08-27): a caller that builds `os` as a local and lets it go out
// of scope before `ed` is freed leaves `ed->dev_arena.os` dangling (found via a real segfault,
// six panel test files' shared `make_editor()` helper - fixed by making the local `static`, the
// correct fix ONLY when the helper itself returns while the arena lives on in the caller, not a
// reason to make every `VMemApi` local `static`).
ErrCode editor_init(Editor* ed, const VMemApi* os, u64 dev_arena_reserve);

// Registers one panel (copies `name`, truncated at EDITOR_PANEL_NAME_CAP-1 - TL_CHECK it fits).
// `open` starts at `default_open`. TL_FATAL: table full, duplicate name (matching every other
// registration door in the tree - init-time misconfiguration).
void editor_register_panel(Editor* ed, const char* name, PanelDrawFn draw_fn, bool default_open);

// Toggles `name`'s current `open` state (the shell's menu calls this). TL_CHECK: registered.
void editor_toggle_panel(Editor* ed, const char* name);

// The per-render-frame entry point: ImGui `NewFrame`, the dockspace, every open panel's
// `draw_fn`, the capture-mask publish (docs/TOOLING.md §9.3.7), `Render`. STUB - see this
// header's Status note; TL_FATAL("unimplemented") until `PlatformDevApi` lands.
void editor_frame(Editor* ed, World* w);

// Marks `ed` dead (`initialized = 0`). Does NOT destroy an ImGui context - none exists yet,
// since `editor_frame` (the only thing that would create one) is still `TL_FATAL("unimplemented")`
// - and does NOT release the dev arena's reservation: `vmem_arena.h` has no teardown door at all
// (init/push/reset_to/decommit_above only), so the arena's `VMemApi`-backed reservation is
// released by whatever owns that `VMemApi`'s lifetime (`app/wiring.cpp`, W4, not built), not by
// this call. Corrected 2026-08-27 (B-4) - the previous wording claimed both, which this function
// has never done; likely the right v0 answer (nothing here needs tearing down before the process
// exits), but the header should say what the function does, not what it might do once `app/`
// exists.
void editor_shutdown(Editor* ed);
