#pragma once
// ---------------------------------------------------------------------------------------------
// inspector.h - the generic reflection walker: every component's FieldInfo table, one widget
//   per FieldKind, for the single selected entity (Editor::sel) plus every singleton.
//
// Spec: docs/TOOLING.md §2 (design), §9.3.4 (the walker algorithm - corrected in the same commit
//   as this file: its pseudocode predates several ECS.md reconciliations - FK_* is really K_*,
//   ECS.md §10.5 not §4 has the real CMD_SET_FIELD shape, `world_get`/`world_singleton_ptr`
//   under those names do not exist - see §9.3.4's own updated text for what actually exists and
//   what this file uses instead).
// Purpose: read every component; edit only the two kinds a command can carry losslessly today
//   (see Invariants). Everything else the spec's algorithm describes is display-only at v0.
// Invariants: an edit is offered ONLY for a non-array field (`FieldInfo::count == 1`) of an
//   integer or bool kind - `core/commands.h`'s real `CMD_SET_FIELD` payload is `{u32 field_index;
//   bytes[field.size]}`, no element index, so a single array-element write is not representable
//   (the same gap `editor/dotpath.cpp`'s `dotpath_set_raw` already documents and guards with
//   `TL_CHECK(f->count == 1u)` - this file guards the same way, by simply not drawing an editable
//   widget for `count > 1`, rather than drawing one that would silently fail or write the wrong
//   bytes). The nine fx palette kinds (`K_pos`..`K_scalar`) and every handle/`K_StrId` kind are
//   DISPLAY ONLY: an fx edit needs a decimal-to-raw RNE quantizer that does not exist anywhere in
//   this tree yet (`core/cvar.cpp`'s own `CVAR_FX_RAW` case already documents the identical gap -
//   "raw:<i32> only... needs FX-PALETTE.md's RNE quantizer, not available to this pure module");
//   inventing one here, untested (nothing in this tree can simulate the keystrokes that would
//   exercise it), is exactly the kind of unreviewable correctness risk `CLAUDE.md` asks to avoid
//   for anything that can write into registered/hashed state through a real command. `ComponentInfo`
//   has no `custom_draw` member and no per-system `debug_draw` registry exists (both named in
//   TOOLING.md §9.3.4's pseudocode) - neither hook is built; the generic per-field walker is the
//   only path.  Lockstep is hardcoded `false` in the edit path (`inspector_set_scalar_field`),
//   matching `console_panel_draw`'s own note - no netcode/Hovel session exists yet to ask.
// Determinism: dev UI only (`TOOLING.md` §0). Every edit reaches sim state through
//   `world_set_field_cmd` (`core/commands.h`'s `CMD_SET_FIELD`), never a direct write - this file
//   never touches a component's memory except through that call and the read-only display loads.
// Threading: none - single-threaded dev UI, matching every other panel this lane has built.
// Includes: editor/editor.h (Editor, World forward decl).
// ---------------------------------------------------------------------------------------------
#include "editor/editor.h"

// Registers the Inspector panel on `ed` (`editor_register_panel(ed, "Inspector",
// inspector_panel_draw, true)`).
void inspector_panel_register(Editor* ed);

// The panel's own content: every non-hidden component's fields for `ed->sel` (skipped if no
// selection - `handle_is_null(ed->sel)`), then every singleton (shown regardless of selection).
void inspector_panel_draw(Editor* ed, World* w);

// What an integer/bool field's edit widget calls on "deactivated after edit" (this header's
// Invariants note: only ever reached for a non-array field). Exposed and testable directly,
// the same way `console_panel_draw`'s submit branch is tested via `console_exec` directly rather
// than a simulated keystroke - nothing in this tree can simulate one. `bytes`/`len` are the raw
// replacement value (`len` must equal the field's own `FieldInfo::size`, matching
// `world_set_field_cmd`'s own contract). Refuses under `lockstep` with `ERR_EDITOR_LOCKSTEP`
// before recording anything (`world_set_field_cmd` itself has no lockstep concept - the caller's
// job, matching `dotpath_set_raw`'s identical shape).
ErrCode inspector_set_scalar_field(World* w, bool lockstep, Entity e, ComponentId comp,
                                    u32 field_index, const void* bytes, u32 len);
