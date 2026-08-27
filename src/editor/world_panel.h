#pragma once
// ---------------------------------------------------------------------------------------------
// world_panel.h - the World panel: the entity slot walk, the singleton component list, and the
//   registered arena set with an on-demand per-arena rehash/diff.
//
// Spec: docs/TOOLING.md §9.4 (this panel's row - "World.entities (slot walk), singleton list,
//   ArenaRegistry + last per-arena hashes; entity list virtualized (ImGuiListClipper)"), §9.6
//   build order item 5. Sixth and final v0 panel this lane builds.
// Purpose: `World.entities` is a `SlotMap<EntityRecord, Entity>` (`core/world.h`) - walked the
//   way `docs/CONTAINERS.md` §2 requires (`0..slotmap_slot_cap()`, skipping dead slots, never
//   `0..live_count`), reconstructing each live slot's `Entity` via `handle_make<Entity>(idx,
//   gen[idx])`. The singleton list reuses `editor/inspector.cpp`'s own `w->comps[]`/
//   `COMP_SINGLETON` walk (names only - Inspector already shows a singleton's fields). The arena
//   section reads `World::registry` (`foundation/arena_registry.h`).
// Invariants (v0 scope, corrected against §9.4's row in this same commit - stated here rather
//   than silently shipped): **"last per-arena hashes" is not a stored field anywhere in this
//   tree** - `ArenaEntry` carries only `{id, arena*, flags}`, no cached hash
//   (`arena_registry.h`'s own struct). `registry_hash_all` is real and genuinely computes one,
//   but it rehashes every `ARENA_HASHED` entry's full `[base, used)` range on every call - a cost
//   that scales with world size, not a fixed per-frame cost like every other panel's own reads.
//   Calling it unconditionally "every frame" (§9.4's original phrasing) would impose a hidden,
//   world-size-dependent cost on a dev tool nobody asked to pay every frame it happens to be
//   open - the "hidden cost" `CLAUDE.md`'s skepticism principle asks to flag, not build past
//   silently. v0 instead computes on an explicit "rehash arenas" button click, keeps the
//   previous click's set (`Editor::world_arena_hash_prev`) to diff against, and flags which
//   arenas' hashes changed since the last click - this is the actual dev workflow "last hash"
//   implies (spot a desync candidate), just paid for on click, not on frame. No readable arena
//   NAME exists anywhere either (`ArenaEntry::id`/`VMemArena::id` are opaque `NameHash` - no
//   reverse lookup from hash to the literal string is built, unlike component names, which
//   `ComponentInfo::name` keeps as a real string) - arenas display by hex id, not a human name.
//   The entity list virtualizes over `slotmap_slot_cap()` via `ImGuiListClipper` exactly as
//   specified; a run of dead slots inside the clipped physical window renders fewer live rows
//   than fit on screen rather than back-filling from further down the list (true "N live rows
//   always visible" virtualization would need a separate live-index array, an extra full pass
//   this v0 does not add for what is, in every world built so far, a sparse-dead-slot list).
//   **`registry_hash_all` is `TL_CHECK`-fatal on an unsealed registry** (`arena_registry.cpp`) -
//   `registry_seal` is the registry OWNER's call, once, at the end of init (`app/`'s job, `W4`,
//   not built - the exact "blocked on app/" shape `editor.h`'s own Status note already carries
//   for `editor_frame`). The arena section checks `w->registry->sealed` before ever offering the
//   button, and `world_panel_rehash_arenas` checks it again and no-ops rather than TL_FATALing a
//   whole dev session over one click - a test (or a future sealed `app/`) is the only caller that
//   sees real hashes at v0; every world built by this lane's own tests seals its fixture registry
//   explicitly (`registry_seal(&f.reg)`) to exercise the real path.
// Determinism: dev UI only (`TOOLING.md` §0). Read-only over `World`/`ArenaRegistry` state -
//   never mutates either; the arena hash buffers live in `Editor::dev_arena` (never registered,
//   never hashed, never snapshotted - `editor.h`'s own contract note on that arena).
// Threading: none - single-threaded dev UI, matching every other panel this lane has built.
// Includes: editor/editor.h (Editor, World forward decl).
// ---------------------------------------------------------------------------------------------
#include "editor/editor.h"

// Registers the World panel on `ed` (`editor_register_panel(ed, "World", world_panel_draw,
// true)`).
void world_panel_register(Editor* ed);

// The panel's own content. Selecting an entity row sets `ed->sel` (the same selection Inspector
// reads - `editor.h`'s own `sel` contract note), the same "go" convention `inspector.cpp` already
// uses for an `Entity`-kind field.
void world_panel_draw(Editor* ed, World* w);

// What the "rehash arenas" button calls - exposed and testable directly, the same way
// `inspector_set_scalar_field`/`console_exec` are: nothing in this tree can simulate a real
// button click against the null ImGui backend. Lazily allocates `ed->world_arena_hash_cur`/
// `_prev` from `ed->dev_arena` on first call; on every later call, copies the previous result
// into `_prev` (so the panel can flag "changed since last rehash") before recomputing `_cur` via
// `registry_hash_all`.
void world_panel_rehash_arenas(Editor* ed, World* w);

// The live entity slot indices `draw_entities` (this file's own row-producing loop) walks and
// draws, in slot order (`0..slotmap_slot_cap()`, skipping dead slots per this header's Purpose
// note - never `0..live_count`). Writes up to `cap` indices into `out`; returns the TOTAL live
// count (`w->entities.live_count`) regardless of `cap`, so a caller can detect truncation by
// comparing the return value against `cap`. Factored out so a test can assert the panel's single
// non-trivial invariant - a destroyed slot is excluded - directly (B-3, 2026-08-27), rather than
// only that the panel drew something. `draw_entities` calls the same per-slot predicate this
// function does, so the two cannot drift out of sync with each other.
u32 world_panel_visible_slots(const World* w, u32* out, u32 cap);
