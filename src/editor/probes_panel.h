#pragma once
// ---------------------------------------------------------------------------------------------
// probes_panel.h - the Probes panel: a summary table over foundation/tl_probe.h's registered keys.
//
// Spec: docs/TOOLING.md §9.3.3 (the throttle algorithm), §9.4 (this panel's row - "enable/disable
//   per key, profile masks; summary table"), §9.6 build order item 5.
// Purpose: v0 reads `tl_probe_key_count`/`tl_probe_key_at` - promoted from test-only to a real
//   production API in this same commit (`tl_probe.h`'s own contract note), matching
//   `tl_log_ring_count`/`_at` and `tl_prof_ring_count`/`_at`'s identical promotions.
// Invariants (v0 scope, narrower than §9.4's row - stated here rather than silently shipped):
//   READ ONLY. No per-key enable/disable checkbox and no "profile mask" control: the real toggle
//   is console/cvar-routed (`TOOLING.md` §3, `tl_probe.h`'s own comment on `tl_probe_test_set_
//   enabled`) and that wiring does not exist yet - the same class of gap already deferred for
//   Console's own cvar UI (`TODO.md`). `ProbeKey` carries no "profile mask" field at all (nothing
//   in this tree defines what one would be); inventing a control for a concept that has no
//   backing state would be UI over nothing. One row per registered key: name, kind, enabled,
//   count, changes, min/max/mean (already f64 in `ProbeKey` - this is display, not a sim path),
//   last value, last tick.
// Determinism: dev UI only (`TOOLING.md` §0). Never touches sim state.
// Threading: none - single-threaded dev UI, matching every other panel this lane has built.
// Includes: editor/editor.h (Editor, World forward decl).
// ---------------------------------------------------------------------------------------------
#include "editor/editor.h"

// Registers the Probes panel on `ed` (`editor_register_panel(ed, "Probes", probes_panel_draw,
// true)`).
void probes_panel_register(Editor* ed);

// The panel's own content. `w` is unused (`ProbeState` is one process-wide `foundation/probe.cpp`
// global, not per-`World` state) - kept in the signature only because `PanelDrawFn` is uniform.
void probes_panel_draw(Editor* ed, World* w);
