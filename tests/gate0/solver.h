// ---------------------------------------------------------------------------------------------
// solver.h - the disposable Gate 0 solver: docs/ALLOY.md §8.1's substep loop reduced to what
//   G-01..G-06 need. One source, two bindings (see g0_ops.h): namespace g0 is the fixed-point
//   solver under test, g0s the double shadow.
//
// Spec: docs/GATE0-BENCH.md §1 (what gets built), §8.2 (solver data, SoA), §8.3 (the substep),
//   §7 R-1 (corner-vs-SDF contacts), R-2 (the broadphase is in the G-05 budget);
//   docs/ALLOY.md §14.4.B (broadphase), §14.4.3 (pass 3: contacts, colouring, S1..S5 - the
//   arithmetic is spelled there and this file follows it line for line); docs/MEMORY.md §8
//   (the real registered arenas hold every authoritative column; scratch holds the rest);
//   docs/DETERMINISM.md §4 (the per-tick hash is registry_hash_all over [base, used)).
// Purpose: written like production code so the kernel can be MOVED into src/sim by the W3
//   alloy-solver lane if the rows come out clean (docs/GATE0-BENCH.md §1) - but promotion is
//   not the goal; the verdict on the rows is.
// Invariants (docs/ALLOY.md §14.5, the parts that apply): every loop runs in its stated order
//   ([index up], [slot up], [sorted key up]); contacts are generated once per tick and reused
//   by every substep with the linearised depth; neighbour lists are ID-sorted; persistent
//   constraints are coloured once, contacts every tick; within a colour no two constraints
//   share a DYNAMIC carrier (a static body, inv_mass == 0, is never written and so is not a
//   carrier for colouring - the bench's reading of "sharing a carrier"; docs/ALLOY.md's 64-colour
//   fatal would otherwise fire on any body immersed in liquid: TODO.md ruling request);
//   sleeping is OFF; single-threaded (the bench measures arithmetic, not threading).
// Determinism: hashed state = the four registered arenas (hdr, body, particle, dist), every
//   column explicitly sized and zero-initialised by the arena; every transient (contacts,
//   neighbours, colour levels, accumulators) lives in the caller's Scratch and is rebuilt per
//   tick from hashed state, so the hash trace is a pure function of the scene, the seed and the
//   substep count. No pointer-keyed order anywhere; the contact list is sort_u64_kv'd by (i, j).
// Threading: none.
// Includes: g0_ops.h (must precede), scene.h, foundation/{arena_registry,scratch,sort}.h.
// No include guard on purpose (see g0_ops.h) - solver_fx.h / solver_dbl.h guard it.
// ---------------------------------------------------------------------------------------------
#include "foundation/arena_registry.h"
#include "foundation/scratch.h"
#include "foundation/sort.h"
#include "gate0/scene.h"

namespace G0_NS {

// --- constants that depend on the substep count (docs/GATE0-BENCH.md §2 sweep 4 / 8 / 16) ----
// h = 1 / (60 * substeps), g_sub = 9.81 * h, inv_h = 60 * substeps: re-derived by fx_lit exactly
// the way fx_palette.h derives the CANON values (at substeps == 8 they ARE the CANON values:
// asserted in consts_make).
struct Consts {
    dt_t     h;
    vel_t    g_sub;
    i32      inv_h;
    u32      substeps;
    u32      ladder;           // 0 (lambda narrowed per constraint, ALLOY text) | 1 | 2 (identical: i64 lambda, RNE) | 3 (+ residual carry)
    u32      _pad0;
    q_t      inv_two_pi;       // turns <- radians
    scalar_t two_pi;           // radians <- turns
    q_t      mu;               // Coulomb friction (position level)
    q_t      restitution;      // 0: resting stability is the question
    vel_t    rest_vel_min;     // restitution threshold: 2 * g_sub
    pos_t    contact_margin;   // contacts generated within this signed distance (+ travel)
    pos_t    h_kernel;         // PBF kernel radius: 4 texels (docs/CANON.md)
    q_t      kw;               // kernel normalisation: rho == ONE on the rest lattice
    q_t      c_visc;           // XSPH viscosity (docs/ALLOY.md §14.4.3 S5)
    stiff_t  at_density;       // PBF compliance (0)
    stiff_t  at_contact;       // contact compliance (0)
    u32      density_iters;    // Jacobi passes of the density solve per substep (1 = docs/ALLOY.md; a solver-design knob)
    u32      q_shift;          // 30 - log2(h_kernel raw): q = r / h_kernel as an EXACT shift (h_kernel is a power-of-two number of quanta; the normalize-once pair kernel, docs/GATE0-BENCH.md §7 R-3)
};

// --- the registered header (arena 0) --------------------------------------------------------
struct Header {
    u64 tick;
    u32 nb, np, nd;
    u32 substeps;
    u32 ladder;
    u32 _pad0;
};
static_assert(sizeof(Header) == 32, "explicit padding (docs/CPP-SUBSET.md section 5)");

enum : u8 { BF_STATIC = 1 };
enum : u8 { PF_LIQUID = 1 };
enum : u8 { CK_BB = 0, CK_PB = 1 };                         // contact kinds: body-body, particle-body
enum : u32 { LV_DIST = 0, LV_CONTACT = 1, LV_DENSITY = 2 };  // level-list item kinds (2 bits)
enum : u32 { MAX_COLORS = 4096, COLOR_WORDS = MAX_COLORS / 64, MAX_NEIGHBOURS = 128, DEEPEST_K = 4 };   // 128, not docs/ALLOY.md section 14.4.B's 64: the per-tick list carries the section 1.2 support margin (radius up to 2h), which holds ~50 at rest; an overflowing list drops neighbours, under-estimates rho, and the fluid collapses (measured, TODO.md)   // 4096: a 0.5 m box hitting the G-04 liquid at 14 m/s collects ~1,100 particle contacts (docs/ALLOY.md 64-colour fatal: TODO.md)
enum : u32 { NO_BODY = 0xFFFFFFFFu };

// A contact row (transient, scratch; docs/GATE0-BENCH.md §8.2). n points from j to i: the
// projection pushes i along +n. For CK_PB, i is a particle index and j a body index.
struct Contact {
    u32 i, j;
    u8  kind;
    u8  _pad0[3];
    u32 color;
    vec2<pos_t> point;      // world, at generation
    vec2<q_t>   n;          // unit, world
    pos_t depth;            // penetration at generation (> 0 = penetrating)
    pos_t rn_i, rn_j;       // cross(r, n) lever terms (0 for a particle / static)
    local_t lam_n, lam_t;   // frac-30 lambda locals (rung 1: i64 across the sweep)
    invmass_t wi, wj;       // pair-clamped effective inverse masses (translational)
    local_t wi_ang, wj_ang;     // angular den shares inv_I (r x n)^2, frac 30 (i64: outside invmass_t for a light plank)
};

// The world: four registered arenas of SoA columns + scratch transients. Caps are fixed at
// world_init; counts come from the scene.
struct World {
    // registered, hashed (docs/MEMORY.md §8.3)
    VMemArena a_hdr, a_body, a_part, a_dist;
    ArenaRegistry reg;
    Header* hdr;
    u32 cap_b, cap_p, cap_d;
    u32 nb, np, nd;
    // bodies [slot up]
    pos_t *bx, *by, *bpx, *bpy;     // position, previous (implicit velocity encoding, ALLOY S5)
    angle_t *bth, *bpth;
    vel_t *bvx, *bvy;               // last writeback velocity (metrics, restitution pre-solve)
    omega_t *bw;
    invmass_t *binv_m, *binv_i;
    pos_t *bhw, *bhh;
    i32 *bmass;
    u8  *bflags;
    i32 *bres_x, *bres_y;           // rung 3 residuals (state: hashed)
    // particles [index up]
    pos_t *px, *py, *ppx, *ppy;
    vel_t *pvx, *pvy;
    invmass_t *pinv_m;
    i32 *pmass;
    u8  *pflags;
    i32 *pres_x, *pres_y;
    // distance constraints [slot up]
    u32 *da, *db;
    pos_t *drest;
    stiff_t *dat;
    lambda_t *dlam;
    u32 *dcolor;
    // transients (scratch, rebuilt per tick)
    Scratch* scratch;
    u64 scratch_mark;               // tick-begin mark; reset at tick end
    local_t *xl_bx, *xl_by, *thl;   // solver-local widened positions (bodies)
    local_t *xl_px, *xl_py;         // (particles)
    pos_t *xs_bx, *xs_by;           // start-of-substep positions
    angle_t *xs_bth;
    pos_t *xs_px, *xs_py;
    pos_t *xg_bx, *xg_by;           // generation (tick-start) positions, for the linearised depth
    angle_t *xg_bth;
    pos_t *xg_px, *xg_py;
    vel_t *vpre_bx, *vpre_by, *vpre_px, *vpre_py;   // pre-solve velocities (restitution)
    Contact* contacts; u32 nc, cap_c;
    u32 *nbr_begin, *nbr; u32 nbr_total;
    local_t* dens_lam;              // density lambda locals (frac 30)
    local_t* dlam30;                // distance lambda locals for the sweep; narrowed into dlam at writeback
    local_t *gcx, *gcy;             // grad C_i (density), frac 30
    local_t *pair_gx, *pair_gy;     // per neighbour-slot gradient terms kw |W'| n_ij (pass 1 -> pass 2)
    q_t* rho;                       // last density evaluation per particle (metrics)
    u8* dens_active;                // 1 if the particle's density constraint ran this substep
    u32 *level_begin, *level_items; u32 n_colors, n_items;
    u32 max_colors_seen;            // over the run (reported in the CSV header)
    u32 sat_hits;                   // lambda_t saturations (G-02 FAIL signal)
    u32 sat_vel;                    // vel_t clamps in the implicit velocity
    u32 sat_omega;                  // omega_t clamps (quarter turn per substep)
    u32 corr_clamps;                // corrections clamped at 8 m per constraint per substep
    u32 vmax_clamps;                // velocity components clamped at V_MAX_WORLD
    u32 max_degree_seen;            // most constraints on one carrier over the run
    u64 pair_evals;                 // density + XSPH neighbour evaluations (the G-05 cost unit)
    u64 contact_evals;              // contact projections
    u8  debug_density;              // dev aid: print density corrections larger than a texel to stderr (first 60)
    u8  _pad2[3];
    u32 debug_prints;
    u32 max_neighbours_seen;
    u32 nbr_overflow;               // neighbour lists truncated at MAX_NEIGHBOURS (a count, reported)
    Consts k;
};

// Builds the constants for `substeps` and `ladder` (docs/GATE0-BENCH.md §2 sweep, §3.2 rungs).
// At substeps == 8, h/g_sub/inv_h equal the CANON constants (asserted).
Consts consts_make(u32 substeps, u32 ladder, u32 mu_percent, u32 alpha_nano, u32 density_iters);   // alpha_nano: the density compliance alpha x 1e9 (physical; alpha~ = alpha / h^2 is re-derived per substep count)   // alpha_milli: the density compliance alpha~ x 1000 (scene data, docs/ALLOY.md section 8.1)

// Reserves and registers the four arenas at the given caps (docs/MEMORY.md §8.3, sealed),
// binds the scratch, zeroes counts. Never fails (TL_FATAL on a refused reservation).
void world_init(World* w, const VMemApi* os, Scratch* scratch, u32 cap_b, u32 cap_p, u32 cap_d, const Consts* k);
// Releases the four reservations.
void world_release(World* w, const VMemApi* os);
// Loads `s` into the columns (fx copy, or double conversion in the shadow binding). Counts must
// fit the caps (TL_CHECK). Resets tick to 0.
void world_load(World* w, const g0scene::Scene* s);

// --- the tick, exposed piecewise so shadow.cpp can lockstep two worlds pass by pass ---------
// Tick begin: broadphase (fine grid + coarse grid), contact generation (corner-vs-SDF, R-1),
// neighbour lists, colouring, level lists. Opens the tick's scratch scope. Returns the number
// of contacts. `broadphase_us_out` receives the broadphase-only cost when clock is non-null.
u32  tick_begin(World* w);
// S1 predict for substep s: v = (x - px) * inv_h + g_sub; xl = x + v*h widened; xs = x; px = xs.
void substep_predict(World* w, u32 s);
// The PBF density solve for this substep: two Jacobi passes over the neighbour lists (lambda
// gather, then the lambda_i + lambda_j correction), owner-only writes. Runs after predict and
// before the colour sweep (see solver.cpp).
void substep_density(World* w);
// S2 + S3 for ONE colour: resets nothing (lambdas are reset in substep_predict); projects every
// distance constraint and contact of colour c in stable order. Precondition c < n_colors.
void substep_project_color(World* w, u32 c);
// S4 writeback: x = round(xl) (the one round per substep), v = (x - px) * inv_h; rung 3 keeps the
// residual.
void substep_writeback(World* w);
// S5 velocity pass: restitution on contacts (Jacobi over pre-pass v), XSPH on liquid particles;
// px = x - v_final * h re-encodes the velocity implicitly.
void substep_velocity(World* w);
// Tick end: closes the scratch scope, bumps the tick. After this the transients are invalid.
void tick_end(World* w);
// The whole tick = tick_begin, then for each substep predict/colours/writeback/velocity, then
// tick_end. Writes the broadphase+contact cost (ticks of the caller's clock) if `clock` is set.
void world_tick(World* w);

// The per-tick state hash: registry_hash_all over the four arenas (docs/DETERMINISM.md §4).
u64  world_hash(const World* w, u64 per_arena_out[MAX_ARENAS]);

// --- metric probes over the columns (docs/GATE0-BENCH.md §8.4), fx-side semantics ----------
// Max penetration (in pos_t) of any dynamic body corner or particle into any static body,
// re-evaluated from the CURRENT positions; sets *tunnel if a dynamic body's centre or a
// particle lies strictly inside a static box.
pos_t world_max_penetration(const World* w, u8* tunnel);
// The §7 R-5 graded-body probe: max penetration of `body`'s corners into EVERY other body's box
// (static or dynamic - "the boulder tunnels through neither the feather nor the floor"); sets
// *tunnel if `body`'s centre lies more than a texel inside any other body's interior.
// Precondition: body < nb and dynamic.
pos_t world_body_penetration(const World* w, u32 body, u8* tunnel);
// Energy at the documented shifts: KE = sum m*(vx^2+vy^2)*50 >> 40, PE = sum m*981*y >> 18,
// both in centijoules (mass quanta = unit mass = 1 kg, g = 9.81), summed in i64 (saturating).
i64  world_energy_cj(const World* w, i64* ke_out, i64* pe_out);

}  // namespace G0_NS
