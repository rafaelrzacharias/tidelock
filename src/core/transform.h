#pragma once
// ---------------------------------------------------------------------------------------------
// transform.h - Transform / TransformPrev: the double-buffered, interpolated world transform.
//
// Spec: docs/FRAME-LOOP.md §4 (design - the snap bit, the ping-pong contract), §8.2 step 4 (this
//   pair is registered as "engine components" in app/wiring.cpp, immediately after each other so
//   both columns share one dense order); docs/RENDER2D.md §9.2 (the pinned field shape - extract
//   reads these); docs/CPP-SUBSET.md §8 (the reference TL_FIELDS_Transform shape, verbatim).
// Purpose: `Transform` is `current`; `TransformPrev` is `prev`, ping-ponged at the end-of-tick
//   barrier (docs/FRAME-LOOP.md §3 step 3). render/extract.cpp is the one reader of both columns
//   together (fx -> float, lerp by alpha) - nothing else needs them side by side.
// Invariants: identical field lists by design (FRAME-LOOP.md §4 "double-buffer the resolved
//   world transform"). The ping-pong itself is a per-field copy, not a pointer/column swap
//   (FRAME-LOOP.md §3's recorded deviation, TODO.md RR-28's ruling record: the interp-pair
//   contract is dense-order parity, enforced by `interp_pingpong`'s `TL_CHECK`, not a pointer
//   swap between two physically interchangeable buffers - no lane has built that).
//   `flags` bit 0 = TRANSFORM_SNAP (spawn/teleport: prev = current for one frame - the engine
//   auto-snaps newly realized entities; render/extract.cpp READS the bit, never writes it; the
//   barrier clears it); bits 1..31 are zero. TransformPrev is COMP_HIDDEN (not the generic
//   inspector's business - docs/TOOLING.md §2) but otherwise an ordinary reflected column.
// Determinism: `pos_t`/`angle_t` only - no floats (docs/CANON.md "Types"; docs/CPP-SUBSET.md §1).
//   Both columns are HASHED|SNAPSHOT|GROWS_AT_BARRIER like any component (docs/ECS.md §10.3);
//   this header declares no state of its own, just the two reflected structs.
// Threading: none - POD components, registration is init-only (world_register_component).
// Includes: core/reflect.h (TL_COMPONENT/TL_COMPONENT_FLAGS, the palette row kinds).
//
// LANDING NOTE (render2d lane, 2026-08-26): this file did not exist on `main` and no active
// lane's docs/ROADMAP.md §2 "Builds" column names it explicitly, even though FRAME-LOOP.md's
// interp.cpp (loop+input, a W3 sibling lane launched the same day, branch not yet pushed at the
// time of this commit) and RENDER2D.md's extract.cpp (this lane) both need it to compile. Landed
// here under the docs/foundation/rect.h precedent (that header's own contract block: a
// downstream lane may transcribe a struct VERBATIM from the doc that pins its exact shape, when
// no lane's Builds column currently claims it and the consuming lane needs it now) - the shape
// is pinned identically in two docs (RENDER2D.md §9.2, CPP-SUBSET.md §8), so there is no
// judgment call left to make, only a landing-order race to record. Filed in TODO.md/LESSONS.md
// so the loop+input lane merges main and finds this rather than re-declaring a second Transform.
// ---------------------------------------------------------------------------------------------
#include "core/reflect.h"

enum : u32 { TRANSFORM_SNAP = 1u << 0 };   // bits 1..31 reserved, zero (docs/FRAME-LOOP.md §4)

// TL_FIELDS_Transform - the reference shape (docs/CPP-SUBSET.md §8), transcribed verbatim.
#define TL_FIELDS_Transform(X, XA, XH) \
    X(pos_t, x) X(pos_t, y) X(angle_t, rot) X(u32, flags)   /* bit 0 = snap (FRAME-LOOP.md §4); bits 1..31 zero */
TL_COMPONENT(Transform)

// TransformPrev - identical field list (docs/RENDER2D.md §9.2), COMP_HIDDEN. **Nothing in src/
// registers either component today** (corrected 2026-08-28, W3 sweep area C defect D5: this block
// claimed "registered by core immediately after Transform", which was never true - the only
// registrations in the tree are two render test fixtures, tests/render/{extract,sprite}.test.cpp).
// app/wiring.cpp is the intended registrar and does not exist yet (docs/ARCHITECTURE.md §9, v0,
// W4). A consumer that needs the pair must register it itself until then.
// Registration order alone would not guarantee dense order stays in
// lockstep after add/remove anyway (column_remove is swap-remove) - dense-order parity is the ruled
// interp-pair contract (TODO.md RR-28), enforced at runtime by `interp_pingpong`'s `TL_CHECK`,
// not a structural consequence of "added/removed together".
#define TL_FIELDS_TransformPrev(X, XA, XH) \
    X(pos_t, x) X(pos_t, y) X(angle_t, rot) X(u32, flags)
TL_COMPONENT_FLAGS(TransformPrev, COMP_HIDDEN)

static_assert(sizeof(Transform) == sizeof(TransformPrev), "docs/RENDER2D.md section 9.2: identical field lists");
