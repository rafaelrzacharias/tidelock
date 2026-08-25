#pragma once
// ---------------------------------------------------------------------------------------------
// scene.h - the scene description G-01..G-05 are built from, in fx rows only.
//
// Spec: docs/GATE0-BENCH.md §2 (the scenarios), §8.1 (scenes.cpp); docs/DETERMINISM.md §3
//   (rng_for keys every random placement; the bench's system_id is GATE0_RNG_SYS below).
// Purpose: one fx-typed POD description that BOTH solver bindings load (world_load in
//   solver.h): the fx world copies it, the double world converts it - so the two start from
//   bit-identical scenes and every later difference is arithmetic, not setup.
// Invariants: arrays are fixed-capacity, arena-pushed, counts <= caps; indices in SceneDist
//   refer to particles. A static body has inv_m == inv_i == 0 exactly (docs/ALLOY.md §8.1
//   "Anchor = inv_mass 0"). Every position is inside the sealed box (box_lo/box_hi) which is
//   itself four static bodies in the list.
// Determinism: built from rng_for(seed, 0, GATE0_RNG_SYS, carrier, draw) - the same seed gives
//   the same scene on every machine (docs/DETERMINISM.md §3).
// Threading: none.
// Includes: foundation/fx_palette.h, foundation/vmem_arena.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/fx_palette.h"
#include "foundation/vmem_arena.h"

namespace g0scene {

// The bench's RNG system id (docs/DETERMINISM.md §3: engine systems register from 1; the bench
// is not an engine system and never shares a seed with one, so any nonzero id is correct).
enum : u32 { GATE0_RNG_SYS = 1 };

struct SceneBody {
    pos_t x, y;           // centre
    pos_t hw, hh;         // half extents (box)
    angle_t th;
    vel_t vx, vy;
    omega_t w;
    invmass_t inv_m;      // 0 = static
    invmass_t inv_i;
    i32 mass_quanta;      // energy metric mass (docs/GATE0-BENCH.md §8.4), 0 for statics
    u32 _pad0;
};
struct SceneParticle {
    pos_t x, y;
    vel_t vx, vy;
    invmass_t inv_m;
    i32 mass_quanta;
    u8 liquid;            // 1 = PBF density constraint carrier; 0 = rope/free particle
    u8 _pad0[3];
    u32 _pad1;
};
struct SceneDist {        // distance constraint between particles a and b
    u32 a, b;
    pos_t rest;
    stiff_t alpha_tilde;
};
struct Scene {
    SceneBody* bodies;      u32 nb, cap_b;
    SceneParticle* parts;   u32 np, cap_p;
    SceneDist* dists;       u32 nd, cap_d;
    // The sealed box interior, for the tunneling check and the broadphase extent.
    pos_t box_lo_x, box_lo_y, box_hi_x, box_hi_y;
    // Scenario-specific metric parameters (docs/GATE0-BENCH.md §8.4)
    u32 settle_tick;        // metrics start after this tick (G-01: 600; G-03: GATE0_G03_SETTLE)
    u32 stack_top_body;     // G-01: the body whose y is the sink/pop probe; 0xFFFFFFFF = none
    u32 _pad0;
};

// Allocates the scene's arrays from `arena` at the given capacities; counts start at 0.
void scene_init(Scene* s, VMemArena* arena, u32 cap_b, u32 cap_p, u32 cap_d);
// Appends a box body; returns its index. TL_FATAL over capacity.
u32  scene_add_body(Scene* s, pos_t x, pos_t y, pos_t hw, pos_t hh, angle_t th, i32 mass_quanta, u8 is_static);
// Appends a particle; returns its index. TL_FATAL over capacity.
u32  scene_add_particle(Scene* s, pos_t x, pos_t y, i32 mass_quanta, u8 liquid);
// Appends a distance constraint between particles a and b with rest = their current distance.
u32  scene_add_dist(Scene* s, u32 a, u32 b, stiff_t alpha_tilde);
// Adds the four static walls of a sealed box with interior [lo, hi] and wall thickness `t`,
// records box_lo/box_hi. Walls are inv_mass 0 bodies.
void scene_add_sealed_box(Scene* s, pos_t lo_x, pos_t lo_y, pos_t hi_x, pos_t hi_y, pos_t t);

// The scenario builders (docs/GATE0-BENCH.md §2). `seed` keys every random placement;
// `particles` overrides the particle count where the scenario takes one (G-03, G-05).
void scene_g01(Scene* s, u64 seed, u32 stack_n);   // stack_n 0 = the 10 of the spec (a debugging knob)
void scene_g02a(Scene* s, u64 seed);              // feather dropped on a resting boulder from 2 m
void scene_g02b(Scene* s, u64 seed);              // boulder dropped on the resting feather at V_MAX
void scene_g03(Scene* s, u64 seed, u32 particles);
void scene_g04(Scene* s, u64 seed);
void scene_g05(Scene* s, u64 seed, u32 particles);

}  // namespace g0scene
