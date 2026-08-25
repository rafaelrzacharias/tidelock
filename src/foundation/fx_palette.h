#pragma once
// ---------------------------------------------------------------------------------------------
// fx_palette.h - the nine rows, the world constants, the closed mixed-op table.
//
// Spec: docs/FX-PALETTE.md §2 (world constants), §3 (rows, derivation rule), §3.1 (the mixed-op
//   table), §9 R-5 (scalar_t), §10.2 (this header). Values are docs/CANON.md "World constants"
//   and "The fx palette"; this header is written FROM those tables and static_asserts every
//   derivation, so a row that drifts from CANON fails to compile.
// Purpose: the palette is policy - a closed set of formats and a closed set of products. Adding
//   a row or a product is a design decision recorded in docs/FX-PALETTE.md first, then here.
// Invariants: every row is fx<i32, F> (32-bit so the solver columns vectorise, §1); integer bits
//   >= ceil(log2(range * margin)) per row, asserted. FX_PALETTE_REV is parsed by
//   tools/fingerprint.py into build_id: two peers on different palette revs cannot handshake.
// Determinism: constants are constexpr RNE of exact rationals (fx_lit) - one shared rounding,
//   by construction; no runtime computes them.
// Threading: none - header-only constants and traits.
// Namespace: everything is in `fx`; the nine row aliases are ALSO exported at global scope
//   because every X-macro field table (docs/ECS.md §6, docs/ALLOY.md §14) spells them bare.
//   The helpers stay namespaced (fx::mul, fx::div, fx::sqrt) so they never collide with libc.
// Includes: foundation/fx.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/fx.h"

namespace fx {

// --- the rows (docs/CANON.md "The fx palette", rev 2) --------------------------------------
using pos_t     = fx<i32, 18>;   // +-8,192 m, 3.8 um (1/16384 texel)      - the risk row (G-01)
using vel_t     = fx<i32, 20>;   // +-2,048 m/s, ~1 um/s
using invmass_t = fx<i32, 18>;   // +-8,192, statics exactly 0
using stiff_t   = fx<i32, 30>;   // +-2, alpha~ = alpha / h^2, precomputed at init
using q_t       = fx<i32, 30>;   // +-2, unitless: kernels on q = r/h, normals, weights
using angle_t   = fx<i32, 30>;   // +-2 TURNS (not radians); wraps by masking
using omega_t   = fx<i32, 22>;   // +-512 turn/s: retuned at rev 2 to 2x the structural cap
                                  //   |omega| <= inv_h/2 = 240 turn/s (docs/FX-PALETTE.md §9 R-8c);
                                  //   no longer vel_t's format - the omega op triples are explicit below
using dt_t      = fx<i32, 30>;   // +-2 s; only H
using scalar_t  = fx<i32, 16>;   // +-32,768 unitless scalars outside the solver (§9 R-5)
using lambda_t  = scalar_t;      // XPBD multiplier: the same format, the solver-facing name

// Locals named in docs/FX-PALETTE.md §3.1 - never stored, never hashed.
using pos2_wide_t = fx<i64, 36>;   // pos_t x pos_t (squared distances); sqrt<pos_t> of it is exact
using den_wide_t  = fx<i64, 30>;   // stiff_t + invmass_t (+ invmass_t): the XPBD denominator

constexpr u32 FX_PALETTE_REV = 2;

// --- world constants (docs/CANON.md "World constants") -------------------------------------
constexpr pos_t   TEXEL            = fx_raw<pos_t>(1 << 14);                // 1/16 m
constexpr i32     CHUNK_TEXELS     = 128;                                   // chunk = 8 m
constexpr i32     CHUNK_GRID       = 1024;                                  // 1024 x 1024 chunks
constexpr i32     TICK_HZ          = 60;
constexpr i32     SUBSTEPS         = 8;
constexpr i32     INV_H            = TICK_HZ * SUBSTEPS;                    // 480, a plain integer
constexpr dt_t    H                = fx_lit<dt_t>(1, INV_H);                // raw 2236962 (RNE of 2^30/480)
constexpr vel_t   G_SUBSTEP        = fx_lit<vel_t>(981, 100 * INV_H);       // 9.81 m/s^2 * h: raw 21430
constexpr pos_t   WORLD_HALF       = fx_int<pos_t>(4096);                   // extent +-4,096 m
constexpr vel_t   V_MAX_WORLD      = fx_int<vel_t>(512);                    // 512 m/s (T-A-02)
constexpr i32     MASS_RATIO_CLAMP = 4096;                                  // 2^12, per-pair effective
constexpr i32     MASS_RATIO_SHIFT = 12;                                    // log2(MASS_RATIO_CLAMP)
constexpr i32     MAX_STEPS        = 5;                                     // spiral-of-death cap
constexpr angle_t TURN             = fx_raw<angle_t>(1 << 30);              // 1.0 turn; & (TURN.v - 1) wraps
constexpr angle_t QUARTER_TURN     = fx_raw<angle_t>(1 << 28);
constexpr angle_t HALF_TURN        = fx_raw<angle_t>(1 << 29);

// --- the derivation rule, asserted next to each row (docs/FX-PALETTE.md §3) ----------------
// integer bits >= ceil(log2(range * margin)); the row's INT_BITS is 31 - FRAC.
static_assert((i64(1) << pos_t::INT_BITS)     >= 2 * 4096,  "pos_t: +-4,096 m extent x 2 margin");
static_assert((i64(1) << vel_t::INT_BITS)     >= 4 * 512,   "vel_t: V_MAX_WORLD x 4 margin");
static_assert((i64(1) << invmass_t::INT_BITS) >= 2 * 4096,  "invmass_t: MASS_RATIO_CLAMP x 2 margin");
static_assert((i64(1) << stiff_t::INT_BITS)   >= 2,         "stiff_t: +-2");
static_assert((i64(1) << q_t::INT_BITS)       >= 2,         "q_t: +-2, so 1 - q and 1 + q stay in range");
static_assert((i64(1) << angle_t::INT_BITS)   >= 2,         "angle_t: +-2 turns");
static_assert((i64(1) << omega_t::INT_BITS)   >= 512,       "omega_t: +-512 turn/s = 2x the inv_h/2 structural cap (section 9 R-8)");
static_assert((i64(1) << omega_t::INT_BITS)   >= 2 * 240,   "omega_t: 2x margin over the 240 turn/s implicit-encoding cap at 480 Hz");
static_assert((i64(1) << dt_t::INT_BITS)      >= 2,         "dt_t: +-2 s");
static_assert((i64(1) << scalar_t::INT_BITS)  >= 32768,     "scalar_t: +-32,768");
static_assert(TEXEL.v == (1 << 14) && TEXEL.v * 16 == pos_t::ONE, "TEXEL = 1/16 m");
static_assert(H.v == 2236962, "H = RNE(2^30 / 480)");
static_assert(G_SUBSTEP.v == 21430, "G_SUBSTEP = RNE(9.81 / 480 * 2^20 = 21430.27)");
static_assert(WORLD_HALF.v == 4096 << 18 && V_MAX_WORLD.v == 512 << 20, "");
static_assert(i64(CHUNK_TEXELS) * CHUNK_GRID * TEXEL.v == 2 * i64(WORLD_HALF.v),
              "1024 chunks x 128 texels x 1/16 m = 8,192 m = the +-4,096 m span");
static_assert(TURN.v == angle_t::ONE && QUARTER_TURN.v * 4 == TURN.v && HALF_TURN.v * 2 == TURN.v, "");
static_assert((i64(1) << MASS_RATIO_SHIFT) == MASS_RATIO_CLAMP, "");
// The XPBD denominator: (w1 + w2 + alpha~) widened to fx<i64,30> cannot overflow by construction.
static_assert(2 * (i64(8192) << 30) + (i64(2) << 30) < (i64(1) << 62), "den_wide_t headroom");
// The squared-distance local: one component of a world-spanning difference (8,192 m)^2 at Q36
// is 2^62 and fits; the SUM of two such squares is 2^63 and does not, so len2_wide/dot carry the
// precondition |d|^2 < 2^27 m^2 (|d| < 11,585 m minus one quantum) - every pair the broadphase
// hands the solver is within a kernel radius, far inside it (docs/ALLOY.md §14.3).
static_assert((i64(2 * 4096) * (2 * 4096)) < (i64(1) << (63 - 36)), "pos2_wide_t headroom per component");
// The INV_H product pos_t x 480 -> vel_t is a 2-bit widening: exact by construction; the
// angle_t x 480 -> omega_t narrowing is 8 bits with RNE (rev 2; it was 10 at frac 20).
static_assert(vel_t::FRAC_BITS - pos_t::FRAC_BITS == 2 && omega_t::FRAC_BITS - angle_t::FRAC_BITS == -8, "");

// --- the mixed-op table (docs/FX-PALETTE.md §3.1 + §9 R-5), one line per sanctioned product ---
// mul<R>(A, B): the shift is A::FRAC + B::FRAC - R::FRAC. div<R>(A, B): R::FRAC + B::FRAC - A::FRAC.
// mul_int<R>(A, i32): the plain-integer factor is spelled B = i32.
//
// The key is the FORMAT, not the row name: rows that share a format are one C++ type
// (pos_t == invmass_t, q_t == stiff_t == angle_t == dt_t, scalar_t == lambda_t), so the
// compiler checks scale, never units, and a line below serves every row of its formats.
// Each line names the §3.1 product it exists for; the rows it ALSO admits are the same format
// and numerically identical. Order matters (A then B); commutative uses list both orders.
// Rev 2: vel_t and omega_t are DISTINCT formats (§9 R-8c) - every product that used to ride
// vel_t's line "also omega_t" is an explicit omega triple now.
#define FX_OP(R, A, B) template <> struct fx_op_allowed<R, A, B> { static constexpr bool value = true; };

// integrate / predict:   x += mul<pos_t>(v, H)                                   (shift 32)
FX_OP(pos_t,       vel_t,     dt_t)
// implicit velocity:     v = mul_int<vel_t>(x - x_prev, INV_H)                   (widen 2, exact)
FX_OP(vel_t,       pos_t,     i32)
// angular velocity:      omega = mul_int<omega_t>(wrap_sub(theta, ptheta), INV_H)   (narrow 8)
FX_OP(omega_t,     angle_t,   i32)
// rotate:                theta += mul<angle_t>(omega, H)                          (shift 22)
FX_OP(angle_t,     omega_t,   dt_t)
// projection dx = mul<pos_t>(w, lambda); also scalar_t x pos_t/invmass_t (§9 R-5)  (shift 16)
FX_OP(pos_t,       invmass_t, lambda_t)
FX_OP(pos_t,       lambda_t,  invmass_t)
// q_t x row -> row: kernel weights, friction, damping, normals, lerp, rng_range  (shift 30)
FX_OP(pos_t,       q_t,       pos_t)        // also invmass_t
FX_OP(pos_t,       pos_t,     q_t)
FX_OP(vel_t,       q_t,       vel_t)
FX_OP(vel_t,       vel_t,     q_t)
FX_OP(omega_t,     q_t,       omega_t)      // the omega twin of the vel_t line above (shift 30)
FX_OP(omega_t,     omega_t,   q_t)
FX_OP(q_t,         q_t,       q_t)          // also angle_t/stiff_t/dt_t, and div<q_t>(q_t, q_t)
FX_OP(scalar_t,    q_t,       scalar_t)     // also lambda_t
FX_OP(scalar_t,    scalar_t,  q_t)
// scalar_t x row -> row (§9 R-5): quanta-path coefficients, modifiers           (shift 16)
FX_OP(vel_t,       scalar_t,  vel_t)        // pos_t/invmass_t are the lambda lines
FX_OP(vel_t,       vel_t,     scalar_t)
FX_OP(omega_t,     scalar_t,  omega_t)      // the omega twin (shift 16)
FX_OP(omega_t,     omega_t,   scalar_t)
FX_OP(q_t,         scalar_t,  q_t)          // also angle_t/stiff_t/dt_t
FX_OP(q_t,         q_t,       scalar_t)
FX_OP(scalar_t,    scalar_t,  scalar_t)     // also div<scalar_t>(scalar_t, scalar_t) for rsqrt
// pos_t x pos_t -> fx<i64,36>: dot<pos2_wide_t>/len2_wide before sqrt<pos_t>    (shift 0)
FX_OP(pos2_wide_t, pos_t,     pos_t)
// same-row quotient -> q_t: normalize, q = r/h_kernel, density ratio, fx.div_q  (shift 30)
FX_OP(q_t,         pos_t,     pos_t)        // also invmass_t
FX_OP(q_t,         vel_t,     vel_t)
FX_OP(q_t,         omega_t,   omega_t)      // the omega twin (shift 30)
FX_OP(q_t,         scalar_t,  scalar_t)     // also lambda_t
// same-row quotient -> the same row: rsqrt<R> = div<R>(one, sqrt<R>(x))         (shift FRAC)
FX_OP(pos_t,       pos_t,     pos_t)        // also invmass_t
FX_OP(vel_t,       vel_t,     vel_t)
FX_OP(omega_t,     omega_t,   omega_t)      // the omega twin (shift 22)

#undef FX_OP

static_assert(fx_op_allowed<pos_t, vel_t, dt_t>::value && fx_op_allowed<q_t, q_t, q_t>::value, "");
static_assert(!fx_op_allowed<pos_t, vel_t, vel_t>::value && !fx_op_allowed<scalar_t, pos_t, pos_t>::value,
              "an unlisted product is rejected");
static_assert(!fx_op_allowed<pos_t, dt_t, vel_t>::value, "order is part of the key");
static_assert(!fx_op_allowed<pos_t, pos_t, i32>::value, "mul_int<pos_t>(pos_t, k) is not a listed product");

}  // namespace fx

// The row aliases at global scope (see the contract block): X-macro field tables spell them bare.
using fx::pos_t;
using fx::vel_t;
using fx::invmass_t;
using fx::stiff_t;
using fx::q_t;
using fx::angle_t;
using fx::omega_t;
using fx::dt_t;
using fx::scalar_t;
using fx::lambda_t;

static_assert(sizeof(pos_t) == 4 && sizeof(q_t) == 4 && sizeof(scalar_t) == 4, "every row is 32-bit (docs/FX-PALETTE.md section 1)");
static_assert(__is_same(lambda_t, scalar_t) && __is_same(q_t, stiff_t), "aliases of one format are one type");
static_assert(!__is_same(vel_t, omega_t), "vel_t and omega_t are distinct formats from rev 2 (docs/FX-PALETTE.md section 9 R-8)");
