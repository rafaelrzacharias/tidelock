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
// Purpose: read every component; edit the kinds a command can carry losslessly (see Invariants).
//   Handle and `K_StrId` kinds stay display-only at v0 (a handle/StrId edit needs its own
//   resolution UI - "type an entity name" or "pick from the interner" - a different, unscoped
//   feature, not a data-representability gap the way the array-element case is).
// Invariants: an edit is offered ONLY for a non-array field (`FieldInfo::count == 1`) - `core/
//   commands.h`'s real `CMD_SET_FIELD` payload is `{u32 field_index; bytes[field.size]}`, no
//   element index, so a single array-element write is not representable (the same gap `editor/
//   dotpath.cpp`'s `dotpath_set_raw` already documents and guards with `TL_CHECK(f->count == 1u)`
//   - this file guards the same way, by simply not drawing an editable widget for `count > 1`,
//   rather than drawing one that would silently fail or write the wrong bytes). Integer and bool
//   kinds edit via `ImGui::InputScalar`/`Checkbox`, read back on `IsItemDeactivatedAfterEdit`.
//   The nine fx palette kinds (`K_pos`..`K_scalar`) edit via a second, separate "type a new
//   value" text box (RR-38/RR-39, 2026-08-27): parsed through `fx::fx_parse_decimal_raw`
//   (`foundation/fx.h`) - integer-only, no float/double anywhere in the actual quantization; the
//   read-only `%.9g` display next to it was already an f64 before RR-38 and stays one (dev UI,
//   `TOOLING.md` §0's exemption - unrelated to the parse-back path RR-38 needed float-free). A
//   parse failure (empty box, malformed text, out of range) is a silent no-op, matching the
//   console's "a rejected command doesn't mutate state" shape - the error text itself is not
//   surfaced to the user yet (no toast/status-line mechanism exists in this panel), a known,
//   filed (`TODO.md`) post-v0 UX gap, not a silently accepted one. `ComponentInfo` has no
//   `custom_draw` member and no per-system `debug_draw` registry exists (both named in
//   `TOOLING.md` §9.3.4's pseudocode) - neither hook is built; the generic per-field walker is the
//   only path. Lockstep is hardcoded `false` in the edit path (`inspector_set_scalar_field`),
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

// What an integer/bool/fx field's edit widget calls on "deactivated after edit" (this header's
// Invariants note: only ever reached for a non-array field, and only after a successful parse on
// the fx path). Exposed and testable directly,
// the same way `console_panel_draw`'s submit branch is tested via `console_exec` directly rather
// than a simulated keystroke - nothing in this tree can simulate one. `bytes`/`len` are the raw
// replacement value (`len` must equal the field's own `FieldInfo::size`, matching
// `world_set_field_cmd`'s own contract). Refuses under `lockstep` with `ERR_EDITOR_LOCKSTEP`
// before recording anything (`world_set_field_cmd` itself has no lockstep concept - the caller's
// job, matching `dotpath_set_raw`'s identical shape).
ErrCode inspector_set_scalar_field(World* w, bool lockstep, Entity e, ComponentId comp,
                                    u32 field_index, const void* bytes, u32 len);
