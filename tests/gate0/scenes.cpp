// scenes.cpp - G-01..G-05 scene builders (contract: scene.h). Spec: docs/GATE0-BENCH.md §2.
// Every random placement is keyed by rng_for (docs/DETERMINISM.md §3); everything else is a
// closed-form lattice, so a scene is a pure function of (scenario, seed, particle count).
//
// Units: mass_quanta is in 1/4096 kg (unit mass = 4096 quanta) so the 4096:1 feather fits an
// integer; a 1 m box of unit density is 4096 quanta, a particle (the reference mass, inv_m = 1)
// is 4096 quanta. inv_m = 4096 / quanta, inv_i = 12 / (m (w^2 + h^2)) for a box (docs/ALLOY.md
// §14.4.5 T7: "inv_mass = div(unit, quanta * unit_mass) at creation ... i64").
//
// Bench constants that the spec leaves to the scene (all recorded here, none is a threshold):
//   G01_SETTLE 600 (docs/GATE0-BENCH.md §8.4 "after settle (tick > 600)"), G03_SETTLE 2000
//   (the doc says "after settle" without a tick: 2000 ticks = 33 s of settling for a 5k column,
//   then the 5k "stays settled" window), the feather is a 2.5 m x 0.25 m plank (the smallest
//   4096:1 body whose inverse inertia fits invmass_t's +-8192 range - TODO.md, W2 gate0 notes),
//   particle spacing 2 texels on a square lattice (docs/CANON.md).
#include "gate0/scene.h"
#include "foundation/det_math.h"
#include "foundation/rng.h"
#include "foundation/tl_assert.h"

#include <string.h>

namespace g0scene {

namespace {

pos_t m_of(i64 num, i64 den) { return fx::fx_lit<pos_t>(num, den); }   // metres as a rational
pos_t texels(i32 n) { return fx::fx_raw<pos_t>(fx::TEXEL.v * n); }

}  // namespace

void scene_init(Scene* s, VMemArena* arena, u32 cap_b, u32 cap_p, u32 cap_d) {
    memset(s, 0, sizeof(*s));
    s->bodies = (SceneBody*)arena_push(arena, u64(cap_b) * sizeof(SceneBody) + 16u, 16u);
    s->parts = (SceneParticle*)arena_push(arena, u64(cap_p) * sizeof(SceneParticle) + 16u, 16u);
    s->dists = (SceneDist*)arena_push(arena, u64(cap_d) * sizeof(SceneDist) + 16u, 16u);
    s->cap_b = cap_b; s->cap_p = cap_p; s->cap_d = cap_d;
    s->stack_top_body = 0xFFFFFFFFu;
}

u32 scene_add_body(Scene* s, pos_t x, pos_t y, pos_t hw, pos_t hh, angle_t th, i32 mass_quanta, u8 is_static) {
    TL_CHECK(s->nb < s->cap_b);
    SceneBody* b = &s->bodies[s->nb];
    memset(b, 0, sizeof(*b));
    b->x = x; b->y = y; b->hw = hw; b->hh = hh; b->th = th;
    b->vx = fx::fx_raw<vel_t>(0); b->vy = fx::fx_raw<vel_t>(0); b->w = fx::fx_raw<omega_t>(0);
    if (is_static) {
        b->inv_m = fx::fx_raw<invmass_t>(0); b->inv_i = fx::fx_raw<invmass_t>(0); b->mass_quanta = 0;
    } else {
        TL_CHECK(mass_quanta > 0);
        b->mass_quanta = mass_quanta;
        b->inv_m = fx::fx_lit<invmass_t>(4096, mass_quanta);
        // inv_i = 12 * 4096 / (q * (w^2 + h^2)) with w = 2 hw, h = 2 hh in metres:
        //       = 12 * 4096 * 2^36 / (q * ((2 hw.v)^2 + (2 hh.v)^2))   (num < 2^45, den < 2^62)
        // Two rne_divs, ordered so nothing overflows i64: C = 12 * 2^54 / W2 (W2 = w^2 + h^2 at
        // Q36, so C is 12 / (w^2 + h^2) at Q18, ~2^19 for a 1 m box - 2^-19 relative), then
        // raw = C * 4096 / q. 12 * 2^54 < 2^58; C * 4096 < 2^42 for any box >= 0.1 m.
        const i64 w2 = i64(2 * hw.v) * i64(2 * hw.v) + i64(2 * hh.v) * i64(2 * hh.v);
        const i64 c18 = fx::rne_div(i64(12) * (i64(1) << 54), w2);
        const i64 inv_i_raw = fx::rne_div(c18 * 4096, i64(mass_quanta));
        TL_CHECK(fx::fx_fits<invmass_t>(inv_i_raw));   // a body whose inverse inertia leaves the row is a scene bug (the 4096:1 plank is sized to fit)
        b->inv_i = fx::fx_raw<invmass_t>(i32(inv_i_raw));
    }
    return s->nb++;
}

u32 scene_add_particle(Scene* s, pos_t x, pos_t y, i32 mass_quanta, u8 liquid) {
    TL_CHECK(s->np < s->cap_p);
    SceneParticle* p = &s->parts[s->np];
    memset(p, 0, sizeof(*p));
    p->x = x; p->y = y; p->vx = fx::fx_raw<vel_t>(0); p->vy = fx::fx_raw<vel_t>(0);
    p->mass_quanta = mass_quanta;
    p->inv_m = mass_quanta == 0 ? fx::fx_raw<invmass_t>(0) : fx::fx_lit<invmass_t>(4096, mass_quanta);
    p->liquid = liquid;
    return s->np++;
}

u32 scene_add_dist(Scene* s, u32 a, u32 b, stiff_t alpha_tilde) {
    TL_CHECK(s->nd < s->cap_d && a < s->np && b < s->np);
    SceneDist* d = &s->dists[s->nd];
    d->a = a; d->b = b; d->alpha_tilde = alpha_tilde;
    const fx::vec2<pos_t> dv = { s->parts[a].x - s->parts[b].x, s->parts[a].y - s->parts[b].y };
    d->rest = fx::len(dv);
    return s->nd++;
}

void scene_add_sealed_box(Scene* s, pos_t lo_x, pos_t lo_y, pos_t hi_x, pos_t hi_y, pos_t t) {
    const angle_t z = fx::fx_raw<angle_t>(0);
    const pos_t half_t = fx::fx_raw<pos_t>(t.v / 2);
    const pos_t cx = fx::fx_raw<pos_t>((lo_x.v + hi_x.v) / 2), cy = fx::fx_raw<pos_t>((lo_y.v + hi_y.v) / 2);
    const pos_t hw = fx::fx_raw<pos_t>((hi_x.v - lo_x.v) / 2), hh = fx::fx_raw<pos_t>((hi_y.v - lo_y.v) / 2);
    // Every wall overlaps its neighbours by the full thickness t, so no wall EDGE lies on an
    // interior face: a particle resting on the floor next to the left wall must see the wall's
    // inner face (+x), not its bottom edge - with flush walls the analytic SDF's corner tie-break
    // read the edge and pushed the particle into the floor, out through the corner (measured:
    // G-03's particle 0 left the box at tick 27 whatever the compliance).
    const pos_t hw_t = hw + t, hh_t = hh + t;
    scene_add_body(s, cx, lo_y - half_t, hw_t, half_t, z, 0, 1);   // floor
    scene_add_body(s, cx, hi_y + half_t, hw_t, half_t, z, 0, 1);   // ceiling
    scene_add_body(s, lo_x - half_t, cy, half_t, hh_t, z, 0, 1);   // left
    scene_add_body(s, hi_x + half_t, cy, half_t, hh_t, z, 0, 1);   // right
    s->box_lo_x = lo_x; s->box_lo_y = lo_y; s->box_hi_x = hi_x; s->box_hi_y = hi_y;
}

// A square lattice of liquid particles at 2-texel spacing filling [x0, x0 + cols*2 texels) x
// [y0, ...), `count` particles row-major from the bottom. Returns the top y of the block.
static pos_t add_liquid_block(Scene* s, pos_t x0, pos_t y0, u32 cols, u32 count) {
    const pos_t sp = texels(2);
    const pos_t half = texels(1);
    u32 r = 0, c = 0;
    for (u32 i = 0; i < count; ++i) {
        const pos_t x = x0 + half + fx::fx_raw<pos_t>(sp.v * i32(c));
        const pos_t y = y0 + half + fx::fx_raw<pos_t>(sp.v * i32(r));
        scene_add_particle(s, x, y, 4096, 1);
        c += 1; if (c == cols) { c = 0; r += 1; }
    }
    return y0 + fx::fx_raw<pos_t>(sp.v * i32(r + 1u));
}

static void add_stack(Scene* s, pos_t x, u32 n) {
    const pos_t half = m_of(1, 2);
    const angle_t z = fx::fx_raw<angle_t>(0);
    for (u32 i = 0; i < n; ++i) {
        const u32 b = scene_add_body(s, x, half + fx::fx_int<pos_t>(i32(i)), half, half, z, 4096, 0);
        if (i + 1u == n) s->stack_top_body = b;
    }
}

void scene_g01(Scene* s, u64 seed, u32 stack_n) {
    (void)seed;
    scene_add_sealed_box(s, fx::fx_int<pos_t>(-10), fx::fx_int<pos_t>(0), fx::fx_int<pos_t>(10), fx::fx_int<pos_t>(20), fx::fx_int<pos_t>(1));
    add_stack(s, fx::fx_int<pos_t>(0), stack_n ? stack_n : 10);
    s->settle_tick = 600;
}

void scene_g02a(Scene* s, u64 seed) {
    (void)seed;
    scene_add_sealed_box(s, fx::fx_int<pos_t>(-10), fx::fx_int<pos_t>(0), fx::fx_int<pos_t>(10), fx::fx_int<pos_t>(20), fx::fx_int<pos_t>(1));
    const angle_t z = fx::fx_raw<angle_t>(0);
    scene_add_body(s, fx::fx_int<pos_t>(0), m_of(1, 2), m_of(1, 2), m_of(1, 2), z, 4096, 0);           // boulder, resting
    // the feather: 2.5 m x 0.25 m, 1 quantum (4096:1), 2 m above the boulder's top face
    scene_add_body(s, fx::fx_int<pos_t>(0), m_of(1, 1) + fx::fx_int<pos_t>(2) + m_of(1, 8), m_of(5, 4), m_of(1, 8), z, 1, 0);
    s->settle_tick = 600;
}

void scene_g02b(Scene* s, u64 seed) {
    (void)seed;
    scene_add_sealed_box(s, fx::fx_int<pos_t>(-10), fx::fx_int<pos_t>(0), fx::fx_int<pos_t>(10), fx::fx_int<pos_t>(40), fx::fx_int<pos_t>(1));
    const angle_t z = fx::fx_raw<angle_t>(0);
    scene_add_body(s, fx::fx_int<pos_t>(0), m_of(1, 8), m_of(5, 4), m_of(1, 8), z, 1, 0);                // feather, resting on the floor
    const u32 b = scene_add_body(s, fx::fx_int<pos_t>(0), fx::fx_int<pos_t>(3), m_of(1, 2), m_of(1, 2), z, 4096, 0);   // boulder
    s->bodies[b].vy = -fx::V_MAX_WORLD;                                                                    // at V_MAX, downward
    s->settle_tick = 0;
}

void scene_g03(Scene* s, u64 seed, u32 particles) {
    (void)seed;
    // 2 m wide at 2-texel spacing = 16 columns; the height follows from the count (TODO.md:
    // the doc's "~1.2 m tall" and "5k particles" contradict at the CANON spacing; count wins).
    const u32 cols = 16;
    const u32 rows = (particles + cols - 1u) / cols;
    const pos_t top = texels(2 * i32(rows)) + fx::fx_int<pos_t>(4);
    scene_add_sealed_box(s, fx::fx_int<pos_t>(-1), fx::fx_int<pos_t>(0), fx::fx_int<pos_t>(1), top, fx::fx_int<pos_t>(1));
    add_liquid_block(s, fx::fx_int<pos_t>(-1), fx::fx_int<pos_t>(0), cols, particles);
    s->settle_tick = 2000;
}

void scene_g04(Scene* s, u64 seed) {
    scene_add_sealed_box(s, fx::fx_int<pos_t>(-10), fx::fx_int<pos_t>(0), fx::fx_int<pos_t>(10), fx::fx_int<pos_t>(20), fx::fx_int<pos_t>(1));
    add_stack(s, fx::fx_int<pos_t>(-7), 10);                                                   // the G-01 stack
    add_liquid_block(s, fx::fx_int<pos_t>(2), fx::fx_int<pos_t>(0), 32, 2000);                  // 4 m wide block, ~7.9 m tall
    const angle_t z = fx::fx_raw<angle_t>(0);
    // 20 free 0.5 m boxes (1024 quanta) on a 1 m pitch above everything, y jittered by rng_for
    for (u32 i = 0; i < 20; ++i) {
        const u64 r = rng_for(seed, 0, GATE0_RNG_SYS, i, 0);
        const pos_t jitter = rng_range<pos_t>(r, fx::fx_int<pos_t>(0), fx::fx_int<pos_t>(3));
        const pos_t x = fx::fx_int<pos_t>(-9) + fx::fx_int<pos_t>(i32(i)) - m_of(1, 2);   // -9.5 .. 9.5, inside the +-10 box
        scene_add_body(s, x, fx::fx_int<pos_t>(12) + jitter, m_of(1, 4), m_of(1, 4), z, 1024, 0);
    }
    // 10 ropes of 8 distance constraints: anchor (inv_m 0) at y = 19, eight 0.25 m segments
    // hanging at a 30 degree lean so they swing; rope particles are 512 quanta, not liquid
    const stiff_t at = fx::fx_raw<stiff_t>(0);
    for (u32 k = 0; k < 10; ++k) {
        const pos_t ax = fx::fx_int<pos_t>(-9) + fx::fx_raw<pos_t>(m_of(3, 2).v * i32(k)) + m_of(1, 2);
        u32 prev = scene_add_particle(s, ax, fx::fx_int<pos_t>(19), 0, 0);
        for (u32 j = 1; j <= 8; ++j) {
            const pos_t x = ax + fx::fx_raw<pos_t>(m_of(1, 8).v * i32(j));               // 0.125 m sideways per link
            const pos_t y = fx::fx_int<pos_t>(19) - fx::fx_raw<pos_t>(m_of(1, 4).v * i32(j) * 866 / 1000);   // 0.2165 m down per link
            const u32 cur = scene_add_particle(s, x, y, 512, 0);
            scene_add_dist(s, prev, cur, at);
            prev = cur;
        }
    }
    s->settle_tick = 600;
}

void scene_g05(Scene* s, u64 seed, u32 particles) {
    const u32 cols = 128;                                         // 16 m wide block
    const u32 rows = (particles + cols - 1u) / cols;
    const pos_t liquid_top = texels(2 * i32(rows));
    const pos_t top = liquid_top + fx::fx_int<pos_t>(30);
    scene_add_sealed_box(s, fx::fx_int<pos_t>(-16), fx::fx_int<pos_t>(0), fx::fx_int<pos_t>(16), top, fx::fx_int<pos_t>(1));
    add_liquid_block(s, fx::fx_int<pos_t>(-8), fx::fx_int<pos_t>(0), cols, particles);
    // 2k bodies: 0.25 m boxes (256 quanta) on a 0.5 m pitch, 60 per row, above the liquid,
    // x jittered by rng_for within the pitch so they do not land in one column
    const angle_t z = fx::fx_raw<angle_t>(0);
    for (u32 i = 0; i < 2000; ++i) {
        const u32 row = i / 60u, colx = i % 60u;
        const u64 r = rng_for(seed, 0, GATE0_RNG_SYS, i, 0);
        const pos_t jx = rng_range<pos_t>(r, fx::fx_int<pos_t>(0), m_of(1, 5));
        const pos_t x = fx::fx_int<pos_t>(-15) + fx::fx_raw<pos_t>(m_of(1, 2).v * i32(colx)) + jx;
        const pos_t y = liquid_top + fx::fx_int<pos_t>(2) + fx::fx_raw<pos_t>(m_of(1, 2).v * i32(row));
        scene_add_body(s, x, y, m_of(1, 8), m_of(1, 8), z, 256, 0);
    }
    s->settle_tick = 200;
}

}  // namespace g0scene
