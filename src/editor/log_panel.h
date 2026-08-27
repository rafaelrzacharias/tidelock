#pragma once
// ---------------------------------------------------------------------------------------------
// log_panel.h - the Log panel: lists foundation/tl_log.h's ring, newest first.
//
// Spec: docs/TOOLING.md §1 (panel table), §9.1 (this file's row), §9.6 build order item 5.
// Purpose: the first panel this lane builds, so it also settles the convention every later panel
//   follows: a `PanelDrawFn` owns its own `ImGui::Begin`/`End` (docs/editor.h's `PanelDrawFn`
//   comment states this) - `editor_frame` (still a stub, `editor.h`'s Status note) will do no more
//   than call every open panel's `draw_fn`, no per-panel window boilerplate of its own. That is
//   also what makes `log_panel_draw` callable directly from a test with no `Editor`/`editor_frame`
//   involved at all - exactly `TOOLING.md` §9.5's "panels run ImGui headless via a null backend."
// Invariants: reads `tl_log_ring_count`/`tl_log_ring_at` only - no state of its own, no
//   allocation (ImGui's own per-frame arena, `pool_vendor`, is the one exception the v0 done
//   criterion names, `TOOLING.md` §9.6 item 5).
// Determinism: dev UI only (`TOOLING.md` §0 "ImGui is dev UI only") - never touches sim state,
//   never on a sim path.
// Threading: none - single-threaded dev UI, matching `editor.h`.
// Includes: editor/editor.h (PanelDrawFn, Editor, World forward decl).
// ---------------------------------------------------------------------------------------------
#include "editor/editor.h"

// Registers the Log panel on `ed` (`editor_register_panel(ed, "Log", log_panel_draw, true)` -
// open by default, matching a console/log panel's usual role as the first thing a dev looks at).
void log_panel_register(Editor* ed);

// The panel's own content, callable directly (see this header's Purpose note) - `ed`/`w` are
// unused today (the log ring is one process-wide `foundation/log.cpp` global, not per-`Editor` or
// per-`World` state); kept in the signature only because `PanelDrawFn` is uniform across every
// panel (`editor.h`).
void log_panel_draw(Editor* ed, World* w);
