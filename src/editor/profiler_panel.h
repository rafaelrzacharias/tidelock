#pragma once
// ---------------------------------------------------------------------------------------------
// profiler_panel.h - the Profiler panel: the latest (or, paused, a chosen) foundation/tl_prof.h
//   ring frame's node tree, plus the counter table.
//
// Spec: docs/TOOLING.md §9.3.1 (the ring algorithm), §9.4 (this panel's row - "flame graph of
//   ring[head-1], pause button freezes head"), §9.6 build order item 5.
// Purpose: v0 reads `tl_prof_ring_count`/`tl_prof_ring_at`/`tl_prof_counter_count`/
//   `tl_prof_counter_at` - promoted from test-only to a real production API in this same commit
//   (`tl_prof.h`'s own contract note), matching `tl_log_ring_count`/`_at`'s identical promotion
//   the day `log_panel.cpp` needed it.
// Invariants (v0 scope, narrower than §9.4's row - stated here rather than silently shipped):
//   the node tree draws as a depth-indented TEXT list, not a rendered flame graph (no rectangle/
//   timeline widget exists in this tree yet, and inventing one is a real feature on its own, not
//   a byproduct of the first panel that reads `ProfState`). "Pause" freezes the PANEL'S OWN view
//   (`Editor::prof_paused`/`prof_view_slot`), never `ProfState.head` itself - `prof.cpp` has no
//   pause concept and this file does not add one there: freezing the shared ring would stop every
//   OTHER reader (a future trace export, a second Editor) from seeing live frames too, for one
//   panel's convenience. `dump` (`TOOLING.md` §9.3.2, `trace_export.cpp`) is not wired here -
//   `trace_export.cpp` itself is still blocked on `fmt_buf`'s disk-flush half (`PlatformApi.
//   file.append`, `TOOLING.md` §9.6 build order item 3) - there is nothing to call yet.
// Determinism: dev UI only (`TOOLING.md` §0). Never touches sim state; `ProfState` itself is
//   never hashed or snapshotted (`tl_prof.h`'s own Determinism note).
// Threading: none - single-threaded dev UI, matching every other panel this lane has built.
// Includes: editor/editor.h (Editor, World forward decl).
// ---------------------------------------------------------------------------------------------
#include "editor/editor.h"

// Registers the Profiler panel on `ed` (`editor_register_panel(ed, "Profiler",
// profiler_panel_draw, true)`).
void profiler_panel_register(Editor* ed);

// The panel's own content. `w` is unused (`ProfState` is one process-wide `foundation/prof.cpp`
// global, not per-`World` state) - kept in the signature only because `PanelDrawFn` is uniform.
void profiler_panel_draw(Editor* ed, World* w);
