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
    u32   prof_view_slot;   // the Profiler panel's own view index (docs/TOOLING.md §9.3.1's ring,
                             // `tl_prof_ring_at`'s `slots_back`) - meaningful only when paused;
                             // freezing the VIEW, never the ring itself (nothing stops `prof.cpp`
                             // from advancing underneath a paused panel - every other reader, a
                             // future trace export included, keeps seeing live frames)
    u8    prof_paused;      // Profiler panel: true = hold on prof_view_slot; false = always slot 0
    u8    initialized;
    u8    _pad0[6];
};

// Zero-initializes `ed`, reserves `dev_arena_reserve` bytes (0 = a documented default, TODO.md -
// no consumer has sized one yet) for the dev arena. Does NOT create an ImGui context (that is
// `editor_frame`'s first call's job, gated on `PlatformDevApi` per this header's Status note).
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

// Destroys the ImGui context (if `editor_frame` ever created one) and releases the dev arena.
void editor_shutdown(Editor* ed);
