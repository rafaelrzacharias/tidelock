#pragma once
// ---------------------------------------------------------------------------------------------
// simview.h - the sim view: material LUT, chunk/body/particle/basin writers over sim/views.h,
//   simview_texel_to_world.
//
// Spec: docs/RENDER2D.md §5 (design), §9.2 (view struct shapes expected from sim/views.h, the
//   MaterialLut/ChunkSlot shapes), §9.3.7 (the chunk writer algorithm - Milestone 2).
// Purpose: Alloy's read-only views -> pixels (docs/RENDER2D.md §5). v0 ships this header plus an
//   EMPTY simview_update (docs/RENDER2D.md §9.1 file layout: "v0 ships the header + an empty
//   update"); the real writers land at Milestone 2.
// CRITICAL DEPENDENCY (this lane's brief, and TODO.md): `sim/views.h` is an alloy-substrate
//   deliverable not yet on `main` (expected after 2026-09-01). The five view structs below are
//   forward-declared OPAQUE here rather than included from sim/views.h, so this header and
//   simview.cpp's stub compile without it; simview_update takes no view parameter yet (v0's
//   empty update needs none). Milestone 2 replaces the forward declarations with
//   `#include "sim/views.h"` once alloy-substrate lands on main (merge main into this branch's
//   Milestone 2 successor - never invent the header here, per the brief).
// Invariants: no coverage AA - a sign test + one-texel edge tint per material (docs/RENDER2D.md
//   §10 R-2). Chunk `(cx, cy)` covers world `x ∈ [-4096 + 8cx, -4096 + 8cx + 8)`, same for y with
//   cy; raster row 0 is the chunk's TOP (highest y); texel (tx, ty) centre is world
//   `(-4096 + 8cx + (tx + 0.5)*TEXEL, -4096 + 8cy + 8 - (ty + 0.5)*TEXEL)` (docs/RENDER2D.md
//   §9.2, the half-texel rule of §0).
// Determinism: the fx->float conversions here (body/particle/basin positions) are two of the
//   only two call sites in the binary, the other being extract.cpp (docs/RENDER2D.md §9.5).
// Threading: v0's stub runs on the main thread; the real Milestone-2 writer is chunk-parallel via
//   JOBS.md, output keyed by chunk (free - docs/RENDER2D.md §5).
// Includes: render/render.h. sim/views.h is Milestone 2's, deliberately not included (see above).
// ---------------------------------------------------------------------------------------------
#include "render/render.h"

// Opaque forward declarations standing in for sim/views.h (the CRITICAL DEPENDENCY note above).
struct ChunkView;
struct BodyView;
struct ParticleView;
struct BurnView;
struct BasinView;

// docs/RENDER2D.md §9.3.7: compiled at init from the Luau palette (UI VM data; 0xAABBGGRR).
struct MaterialLut { u32 rgba[256]; u32 edge_rgba[256]; u8 edge_width[256]; u32 species_rgba[256]; u32 ember_rgba; };

enum : u32 { MAX_RESIDENT_CHUNKS = 256 };

// docs/RENDER2D.md §9.3.7.
struct ChunkSlot { u16 cx, cy; TexHandle tex; u32 seen_serial; u32 last_frame; };

// The half-texel rule (docs/RENDER2D.md §0, §9.2), pure and self-contained - no sim/views.h
// dependency, so it lands now rather than waiting for Milestone 2.
void simview_texel_to_world(u16 cx, u16 cy, u16 tx, u16 ty, f32* wx, f32* wy);

// Milestone 2's chunk/body/particle/basin writer, first system of RENDER (before
// sys_sprite_render - docs/RENDER2D.md §9.3.7). v0: an empty stub (no view parameter yet - see
// the CRITICAL DEPENDENCY note); it is still registered so the schedule shape is stable across
// the Milestone-2 landing.
void simview_update(World* w);
