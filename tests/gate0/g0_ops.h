// ---------------------------------------------------------------------------------------------
// g0_ops.h - the solver's arithmetic vocabulary, in two bindings selected by GATE0_SHADOW.
//
// Spec: docs/GATE0-BENCH.md §1 (the FLOAT-SHADOW config: "the solver compiled once more over
//   double typedefs"), §8.1 (only this TU family and shadow.cpp may see <math.h>);
//   docs/ALLOY.md §14.4.3 (every fx-side op below is one line of that kernel, spelled with the
//   palette helpers exactly as written there); docs/FX-PALETTE.md §3.1 (the products used),
//   §9 R-6 (the one rne_div on raw bits).
// Purpose: solver.cpp is written ONCE against these names. Compiled without GATE0_SHADOW it is
//   the fixed-point solver (namespace g0) and every op is the production palette arithmetic -
//   the thing Gate 0 measures. Compiled with GATE0_SHADOW it is the same source over `double`
//   (namespace g0s), the never-authoritative diagnostic that names "row X, pass Y" when the fx
//   side drifts (docs/ALLOY.md §10 "FLOAT-SHADOW diagnostic build").
// Invariants: the two bindings have identical names and argument orders; a name added on one
//   side without the other is a compile error in the shadow TU, never a silent divergence.
//   The fx side never rounds anywhere the kernel text does not: the only narrowing points are
//   pos_from_xl (the one round per substep), dlam_of (the one rne_div), and the mul<R> calls.
//   Widened locals are frac 30 (pos_t frac 18 + SOLVER_LOCAL_SHIFT 12), per ALLOY §14.4.3
//   (`xl = x.v << 12`); GATE0-BENCH.md §8.3's "16 extra bits" was that doc's restatement drift,
//   corrected in the same commit as this file (TODO.md, W2 gate0 notes).
// Determinism: fx side - pure integer functions over the palette (bit-exact cross-ISA by
//   construction). Shadow side - libm, x86-64 dev tiers only, never in netcode/ship, never hashed.
// Threading: none - pure functions.
// Includes: fx side: foundation/det_math.h; shadow side: <math.h> additionally.
// This header has NO include guard on purpose: solver_fx.h / solver_dbl.h each include it once
// under their own guard with GATE0_SHADOW set the way they need.
// ---------------------------------------------------------------------------------------------
#include "foundation/det_math.h"
#include "foundation/fx_palette.h"

#undef G0_NS
#ifdef GATE0_SHADOW
#define G0_NS g0s
#include <math.h>
#else
#define G0_NS g0
#endif

// docs/ALLOY.md §14.4.3 S1: xl = fx<i64,30>(i64(x.v) << 12) - the solver-local widened point.
#ifndef G0_SOLVER_LOCAL_SHIFT
#define G0_SOLVER_LOCAL_SHIFT 12
#endif

namespace G0_NS {

#ifndef GATE0_SHADOW
// ---- fixed-point binding: the nine rows and the palette helpers, verbatim ------------------
using pos_t     = ::pos_t;
using vel_t     = ::vel_t;
using invmass_t = ::invmass_t;
using stiff_t   = ::stiff_t;
using q_t       = ::q_t;
using angle_t   = ::angle_t;
using omega_t   = ::omega_t;
using dt_t      = ::dt_t;
using lambda_t  = ::lambda_t;
using scalar_t  = ::scalar_t;
using local_t   = i64;          // frac 30 solver local (fx<i64,30> bits)
template <typename T> using vec2 = fx::vec2<T>;

constexpr i64 LOCAL_ONE_SHIFT = i64(1) << G0_SOLVER_LOCAL_SHIFT;   // 4096
constexpr int LOCAL_FRAC      = pos_t::FRAC_BITS + G0_SOLVER_LOCAL_SHIFT;   // 30
static_assert(LOCAL_FRAC == 30, "docs/ALLOY.md section 14.4.3: solver locals are frac 30");

// Conversions from the scene's fx values (identity on this side).
inline pos_t     cvt_pos(::pos_t x)     { return x; }
inline vel_t     cvt_vel(::vel_t x)     { return x; }
inline invmass_t cvt_w(::invmass_t x)   { return x; }
inline stiff_t   cvt_stiff(::stiff_t x) { return x; }
inline q_t       cvt_q(::q_t x)         { return x; }
inline angle_t   cvt_ang(::angle_t x)   { return x; }
inline omega_t   cvt_omega(::omega_t x) { return x; }
inline dt_t      cvt_dt(::dt_t x)       { return x; }
inline scalar_t  cvt_scalar(::scalar_t x) { return x; }
inline lambda_t  lam_zero()             { return fx::fx_raw<lambda_t>(0); }
inline pos_t     pos_zero()             { return fx::fx_raw<pos_t>(0); }
inline vel_t     vel_zero()             { return fx::fx_raw<vel_t>(0); }
inline q_t       q_zero()               { return fx::fx_raw<q_t>(0); }
inline q_t       q_one()                { return fx::fx_raw<q_t>(q_t::ONE); }
inline angle_t   ang_zero()             { return fx::fx_raw<angle_t>(0); }
inline omega_t   omega_zero()           { return fx::fx_raw<omega_t>(0); }
inline local_t   local_zero()           { return 0; }
inline bool      is_zero_w(invmass_t w) { return w.v == 0; }
inline bool      is_zero_pos(pos_t x)   { return x.v == 0; }
inline bool      lam_is_zero(lambda_t l){ return l.v == 0; }
// The metric side reads raw i64 of a row (docs/GATE0-BENCH.md §8.4: "raw" units).
inline i64 raw_pos(pos_t x)   { return x.v; }
inline i64 raw_vel(vel_t x)   { return x.v; }
inline i64 raw_q(q_t x)       { return x.v; }
inline i64 raw_ang(angle_t x) { return x.v; }
inline i64 raw_lam(lambda_t x){ return x.v; }
inline i64 raw_local(local_t x){ return x; }

// --- widened position locals (ALLOY S1 / S4) ----------------------------------------------
// xl = i64(x.v) * 2^12 (a multiply, never a shift of a signed value - docs/CPP-SUBSET.md §5).
inline local_t xl_from_pos(pos_t x)      { return i64(x.v) * LOCAL_ONE_SHIFT; }
// THE one round per substep: x = pos_t(i32(rne_shr(xl, 12))). Precondition: fits pos_t (asserted).
inline pos_t   pos_from_xl(local_t xl)   { const i64 q = fx::rne_shr(xl, G0_SOLVER_LOCAL_SHIFT); TL_ASSERT(fx::fx_fits<pos_t>(q)); return fx::fx_raw<pos_t>(i32(q)); }
// Rung 3 (docs/FX-PALETTE.md §3.2): the residual the round dropped, xl - x*2^12, in [-2^11, 2^11].
inline i32     residual_of(local_t xl, pos_t x) { return i32(xl - i64(x.v) * LOCAL_ONE_SHIFT); }
inline local_t xl_with_residual(pos_t x, i32 res) { return i64(x.v) * LOCAL_ONE_SHIFT + i64(res); }
inline local_t thl_from_angle(angle_t a) { return i64(a.v); }
// Angle writeback: reduce to (-1/2, 1/2] turn by masking (exact, docs/FX-PALETTE.md §3 angle_t).
inline angle_t angle_from_thl(local_t t) {
    const u64 m = u64(t) & u64(fx::TURN.v - 1);
    const i64 v = (m >= u64(fx::HALF_TURN.v)) ? i64(m) - i64(fx::TURN.v) : i64(m);
    return fx::fx_raw<angle_t>(i32(v));
}
inline i32     residual_of_th(local_t thl, angle_t th) { return i32(u32(u64(thl - i64(th.v)) & u64(fx::TURN.v - 1)) ); }
inline local_t thl_with_residual(angle_t th, i32 res) { (void)res; return i64(th.v); }   // angle residual is always 0 (the mask is exact)

// --- integrate / implicit velocity (op-table rows 1, 2, 4) ---------------------------------
inline pos_t   predict_delta(vel_t v, dt_t h)        { return fx::mul<pos_t>(v, h); }
// v = (x - px) * inv_h, exact 2-bit widening; a delta beyond vel_t's +-2,048 m/s (4.27 m in one
// substep at 480 Hz) is clamped and COUNTED - a row saturation the verdict line reports.
inline vel_t   vel_from_delta(pos_t d, i32 inv_h, u32* sat_hits) {
    const i64 q = i64(d.v) * i64(inv_h) * (i64(1) << (vel_t::FRAC_BITS - pos_t::FRAC_BITS));
    if (!fx::fx_fits<vel_t>(q)) { *sat_hits += 1; return fx::fx_raw<vel_t>(q > 0 ? INT32_MAX : INT32_MIN); }
    return fx::fx_raw<vel_t>(i32(q));
}
inline angle_t ang_delta(omega_t w, dt_t h)          { return fx::mul<angle_t>(w, h); }
// omega = (theta - ptheta) * inv_h with the turn subtraction wrapped to (-1/2, 1/2] by the row's
// masking, so |omega| <= inv_h / 2. It is further clamped to +-inv_h / 4 (a quarter turn per
// substep) and COUNTED: past that the implicit encoding ptheta = theta - omega * h is ambiguous
// mod one turn (G-02b's 4096:1 plank struck at V_MAX reaches it).
inline omega_t omega_from_delta(angle_t d, i32 inv_h, u32* sat_hits) {
    const i64 lim = i64(fx::QUARTER_TURN.v) * i64(inv_h);                                   // frac 30
    i64 p = i64(d.v) * i64(inv_h);
    if (p > lim || p < -lim) { *sat_hits += 1; p = p > 0 ? lim : -lim; }
    const i64 q = fx::rne_shr(p, angle_t::FRAC_BITS - omega_t::FRAC_BITS);
    TL_ASSERT(fx::fx_fits<omega_t>(q));
    return fx::fx_raw<omega_t>(i32(q));
}
// The PBF "scale back once" (docs/FX-PALETTE.md §3 q_t): a correction computed on q = r / h is
// rescaled by h_kernel (pos_t): (a * h.v) >> 18.
inline local_t local_scale_pos(local_t a, pos_t h) { const i64 p = fx::sat_mul(a, i64(h.v)); TL_ASSERT(p != INT64_MAX && p != INT64_MIN); return fx::rne_shr(p, pos_t::FRAC_BITS); }
inline vel_t   vel_add(vel_t a, vel_t b)             { return a + b; }
inline vel_t   vel_sub(vel_t a, vel_t b)             { return fx::sat_sub(a, b); }   // relative velocities saturate (never wrap)
inline pos_t   pos_add(pos_t a, pos_t b)             { return a + b; }
inline pos_t   pos_sub(pos_t a, pos_t b)             { return a - b; }
inline pos_t   pos_neg(pos_t a)                      { return -a; }
inline pos_t   pos_abs(pos_t a)                      { return fx::abs(a); }
inline bool    pos_lt(pos_t a, pos_t b)              { return a.v < b.v; }
inline pos_t   pos_max(pos_t a, pos_t b)             { return fx::max(a, b); }
inline pos_t   pos_min(pos_t a, pos_t b)             { return fx::min(a, b); }
inline angle_t ang_add(angle_t a, angle_t b)         { return a + b; }
inline angle_t ang_sub(angle_t a, angle_t b)         { return a - b; }   // wraps by policy (turns)
inline omega_t omega_add(omega_t a, omega_t b)       { return a + b; }
inline vel_t   vel_abs(vel_t a)                      { return fx::abs(a); }
inline bool    vel_lt(vel_t a, vel_t b)              { return a.v < b.v; }
inline vel_t   vel_neg(vel_t a)                      { return -a; }
inline pos_t   pos_scale_q(pos_t a, q_t q)           { return fx::mul<pos_t>(a, q); }
inline vel_t   vel_scale_q(vel_t a, q_t q)           { return fx::mul<vel_t>(a, q); }
inline q_t     q_mul(q_t a, q_t b)                   { return fx::mul<q_t>(a, b); }
inline q_t     q_sub(q_t a, q_t b)                   { return a - b; }
inline q_t     q_add(q_t a, q_t b)                   { return a + b; }
inline q_t     q_neg(q_t a)                          { return -a; }
inline bool    q_lt(q_t a, q_t b)                    { return a.v < b.v; }
inline q_t     q_div(pos_t a, pos_t b)               { return fx::div<q_t>(a, b); }
inline q_t     q_div_q(q_t a, q_t b)                 { return fx::div<q_t>(a, b); }
inline q_t     q_sat(local_t q30)                    { return fx::fx_raw<q_t>(q30 > i64(INT32_MAX) ? INT32_MAX : q30 < i64(INT32_MIN) ? INT32_MIN : i32(q30)); }

// --- geometry (det_math.h) -----------------------------------------------------------------
inline void        sincos_of(angle_t a, q_t* s, q_t* c)          { fx::sincos(a, s, c); }
inline vec2<pos_t> rot(vec2<pos_t> p, q_t s, q_t c)         { return fx::rotate(p, s, c); }
// Precondition |d| < 11,585 m (len2_wide, docs/FX-PALETTE.md fx_palette.h headroom note) -
// every contact/neighbour pair is inside a kernel radius or a body extent, far inside it.
inline pos_t       length(vec2<pos_t> d)                          { return fx::len(d); }
inline vec2<q_t>   unit(vec2<pos_t> d)                    { return fx::normalize(d); }
inline pos_t       dot_pn(vec2<pos_t> a, vec2<q_t> n)          { return fx::dot<pos_t>(a, n); }
inline pos_t       cross_pn(vec2<pos_t> r, vec2<q_t> n)        { return fx::cross<pos_t>(r, n); }
inline vec2<pos_t> vsub(vec2<pos_t> a, vec2<pos_t> b)          { return a - b; }
inline vec2<pos_t> vadd(vec2<pos_t> a, vec2<pos_t> b)          { return a + b; }
inline vec2<q_t>   perp(vec2<q_t> n)                           { return { -n.y, n.x }; }
inline vec2<q_t>   qneg(vec2<q_t> n)                           { return { -n.x, -n.y }; }
inline vec2<q_t>   rotate_q(vec2<q_t> n, q_t s, q_t c)         { return { fx::mul<q_t>(n.x, c) - fx::mul<q_t>(n.y, s), fx::mul<q_t>(n.x, s) + fx::mul<q_t>(n.y, c) }; }
// v . n for the velocity pass, saturating at vel_t's range (a 4096:1 plank launched at V_MAX
// exceeds it relative to the boulder; the saturated value only feeds restitution).
inline vel_t       dot_vn(vec2<vel_t> v, vec2<q_t> n) {
    const i64 s = fx::sat_add(i64(v.x.v) * i64(n.x.v), i64(v.y.v) * i64(n.y.v));
    const i64 q = fx::rne_shr(s, q_t::FRAC_BITS);
    return fx::fx_raw<vel_t>(q > i64(INT32_MAX) ? INT32_MAX : q < i64(INT32_MIN) ? INT32_MIN : i32(q));
}

// --- the XPBD step (ALLOY §14.4.3 "generic XPBD step", all i64 at frac 30) -----------------
// pair clamp: wa' = max(wa, wb >> 12) unless the raw w is 0 (static stays 0). MASS_RATIO_CLAMP = 2^12.
inline invmass_t w_clamp(invmass_t w, invmass_t other) {
    if (w.v == 0) return w;
    const i32 floor_w = other.v >> fx::MASS_RATIO_SHIFT;   // arithmetic shift, other.v >= 0
    return fx::fx_raw<invmass_t>(w.v < floor_w ? floor_w : w.v);
}
// body angular share of the denominator: (r x n)^2 [frac 18] x inv_inertia [frac 18] = frac 36
// -> frac 30 as an i64 local. NOT narrowed to invmass_t: a 4096:1 plank with a 1.25 m lever has
// inv_I (r x n)^2 ~ 12,000, outside the row (+-8,192) - the den is i64 anyway (docs/ALLOY.md
// section 14.4.3 spells "w +=" into invmass_t; measured to overflow at G-02: TODO.md, W2 gate0).
inline local_t w_ang30(invmass_t inv_i, pos_t rn) {
    const i64 rn2 = fx::rne_shr(i64(rn.v) * i64(rn.v), pos_t::FRAC_BITS);      // frac 18, < 2^31 for |rn| < 90 m
    return fx::rne_shr(rn2 * i64(inv_i.v), invmass_t::FRAC_BITS - G0_SOLVER_LOCAL_SHIFT);   // frac 36 -> 30
}
inline invmass_t w_add(invmass_t a, invmass_t b) { return fx::sat_add(a, b); }
// den = wa'*4096 + wb'*4096 + alpha~ (frac 30; < 2^45; multiplies, never << of a signed value).
inline local_t den_of(invmass_t wa, invmass_t wb, stiff_t at) {
    return i64(wa.v) * LOCAL_ONE_SHIFT + i64(wb.v) * LOCAL_ONE_SHIFT + i64(at.v);
}
inline local_t den_add(local_t a, local_t b) { return a + b; }
inline bool    den_is_zero(local_t d)       { return d == 0; }
// C given as a frac-30 local (the density C, or a contact C computed at frac 30 and rounded to
// pos_t first by the caller when the kernel says so). num = -C - (alpha~ * lambda) >> 16.
inline local_t num_of30(local_t c30, stiff_t at, local_t lam30) {
    return -c30 - fx::rne_shr(fx::sat_mul(i64(at.v), lam30), LOCAL_FRAC);
}
inline local_t c30_from_pos(pos_t c) { return i64(c.v) * LOCAL_ONE_SHIFT; }
// dlambda as a frac-30 i64 local: rne_div(num * 2^30, den) - ONE rne_div on the raw bits, RNE
// (docs/FX-PALETTE.md §9 R-6). Rung 1 (§3.2): lambda stays i64 across the substep's sweep and
// narrows to lambda_t ONCE (lam_narrow, at writeback). `ladder == 0` reproduces docs/ALLOY.md
// §14.4.3's per-constraint narrowing (rne_div(num * 2^16, den) then widened back): measured to
// creep at G-01 because a unit-mass correction is then quantised to 4 pos_t quanta (lambda_t's
// 1.5e-5 kg.m quantum x w = 1) - the CSV evidence for the rev-2 lambda_t decision (TODO.md).
// num * 2^30 must fit i64, i.e. |num| < 2^33 (8 m at frac 30). A V_MAX contact generated with
// an 8.5 m travel margin (G-02b) can exceed it: then num and den are both halved (RNE) until it
// fits - den >= 2^18 (w >= 1/4096 at frac 30) keeps the quotient's precision far above the
// lambda quantum, and the scaling is a pure function of the operands (deterministic).
inline local_t dlam_of(local_t num, local_t den, u32 ladder, u32* sat_hits) {
    TL_ASSERT(den != 0);
    while (num >= (i64(1) << 33) || num <= -(i64(1) << 33)) { num = fx::rne_shr(num, 1); den = fx::rne_shr(den, 1); TL_ASSERT(den != 0); }
    if (ladder == 0) {
        i64 q16 = fx::rne_div(num * (i64(1) << lambda_t::FRAC_BITS), den);
        if (!fx::fx_fits<lambda_t>(q16)) { *sat_hits += 1; q16 = q16 > 0 ? INT32_MAX : INT32_MIN; }
        return q16 * (i64(1) << (LOCAL_FRAC - lambda_t::FRAC_BITS));
    }
    return fx::rne_div(num * (i64(1) << LOCAL_FRAC), den);
}
// The once-per-substep narrowing of a lambda local to the stored lambda_t row (RNE by 14);
// a value outside the row is COUNTED (docs/GATE0-BENCH.md §2 G-02 "any saturation hit") and clamped.
inline lambda_t lam_narrow(local_t lam30, u32* sat_hits) {
    i64 q = fx::rne_shr(lam30, LOCAL_FRAC - lambda_t::FRAC_BITS);
    if (!fx::fx_fits<lambda_t>(q)) { *sat_hits += 1; q = q > 0 ? INT32_MAX : INT32_MIN; }
    return fx::fx_raw<lambda_t>(i32(q));
}
inline local_t lam_widen(lambda_t l) { return i64(l.v) * (i64(1) << (LOCAL_FRAC - lambda_t::FRAC_BITS)); }
inline local_t local_max(local_t a, local_t b) { return a < b ? b : a; }
// mag = (w' * dlambda) >> 18: invmass(18) x lambda local(30) = 48 -> frac 30 (a length).
// Precondition |mag| < 2^33 raw (8 m): asserted - a larger correction means the scene is broken.
inline local_t corr_mag(invmass_t w, local_t dl30, u32* clamps) {
    const i64 p = fx::sat_mul(i64(w.v), dl30);
    i64 m = (p == INT64_MAX || p == INT64_MIN) ? p : fx::rne_shr(p, invmass_t::FRAC_BITS);
    // 8 m per constraint per substep is the bench's sanity bound (G-02b's 4096:1 plank, spun a
    // quarter turn per substep by the V_MAX boulder, reaches it through the linearised lever
    // term): clamped and COUNTED, reported on the verdict line, never a crash.
    if (m >= (i64(1) << 33) || m <= -(i64(1) << 33)) { *clamps += 1; m = m > 0 ? (i64(1) << 33) - 1 : -((i64(1) << 33) - 1); }
    return m;
}
// One component of the correction: (mag * n) >> 30, frac 30.
inline local_t corr_comp(local_t mag, q_t n) { return fx::rne_shr(mag * i64(n.v), q_t::FRAC_BITS); }
// The correction along a NON-unit frac-30 gradient (PBF): (mag * g30) >> 30; |grad C_i| reaches
// several units for a one-sided neighbourhood, so the product is checked, not the operand.
inline local_t corr_comp30(local_t mag, local_t g30) {
    // (mag >> 8) * (g30 >> 8) >> 14: both operands may reach 2^33, so the product is taken on
    // 2^-8-reduced copies (a 2^-22 relative resolution on each) and never overflows.
    const i64 p = fx::sat_mul(fx::rne_shr(mag, 8), fx::rne_shr(g30, 8));
    TL_ASSERT(p != INT64_MAX && p != INT64_MIN);
    return fx::rne_shr(p, LOCAL_FRAC - 16);
}
// Angular correction in TURNS at frac 30: inv_i * dlambda [frac 30, 1/m] x rn [frac 18] -> rad
// frac 30; x inv_two_pi (q_t) -> turns frac 30.
inline local_t ang_corr(invmass_t inv_i, local_t dl30, pos_t rn, q_t inv_two_pi) {
    const i64 p = fx::sat_mul(i64(inv_i.v), dl30);
    TL_ASSERT(p != INT64_MAX && p != INT64_MIN);
    const i64 mag = fx::rne_shr(p, invmass_t::FRAC_BITS);
    const i64 rad = fx::rne_shr(mag * i64(rn.v), pos_t::FRAC_BITS);
    return fx::rne_shr(rad * i64(inv_two_pi.v), q_t::FRAC_BITS);
}
// A turn delta (frac 30) to radians (frac 30): x two_pi (scalar_t frac 16) >> 16.
inline local_t rad30_from_turn30(local_t dth, scalar_t two_pi) { return fx::rne_shr(dth * i64(two_pi.v), scalar_t::FRAC_BITS); }
// (dx, dy at frac 30) . n  -> frac 30.
inline local_t dot30_n(local_t dx, local_t dy, vec2<q_t> n) {
    return fx::rne_shr(dx * i64(n.x.v) + dy * i64(n.y.v), q_t::FRAC_BITS);
}
// rad (frac 30) x rn (pos_t) -> frac 30 metres.
inline local_t rad_times_rn(local_t rad30, pos_t rn) { return fx::rne_shr(rad30 * i64(rn.v), pos_t::FRAC_BITS); }
inline local_t local_add(local_t a, local_t b) { return a + b; }
inline local_t local_sub(local_t a, local_t b) { return a - b; }
inline local_t local_neg(local_t a)            { return -a; }
inline local_t local_abs(local_t a)            { return a < 0 ? -a : a; }
inline bool    local_lt(local_t a, local_t b)  { return a < b; }
inline local_t local_scale_q(local_t a, q_t q) { return fx::rne_shr(a * i64(q.v), q_t::FRAC_BITS); }
// den contribution of a frac-30 gradient with inverse mass w: |g|^2 (frac 60 -> >>30) x w (>>18) -> frac 30.
inline local_t den_grad(invmass_t w, local_t gx, local_t gy) {
    // |g| reaches several units for a one-sided neighbourhood (2^31 raw squared overflows i64):
    // square 2^-15-reduced copies, whose product is already at frac 30.
    const i64 hx = fx::rne_shr(gx, 15), hy = fx::rne_shr(gy, 15);
    const i64 g2 = hx * hx + hy * hy;
    const i64 p = fx::sat_mul(g2, i64(w.v));
    TL_ASSERT(p != INT64_MAX && p != INT64_MIN);
    return fx::rne_shr(p, invmass_t::FRAC_BITS);
}
// rho accumulation: kw (q_t) x W (q_t) -> frac 60, summed in i64; rounded once by rho_round.
inline local_t rho_term(q_t kw, q_t w)   { return fx::mul_widen(kw.v, w.v); }
inline local_t rho_round(local_t acc60)  { return fx::rne_shr(acc60, q_t::FRAC_BITS); }
// gradient accumulation: kw (q_t) x dW (q_t) x n (q_t): (kw*dW) >> 30 then x n >> 30 -> frac 30.
inline local_t grad_term(q_t kw, q_t dw, q_t n) {
    const i64 s = fx::rne_shr(fx::mul_widen(kw.v, dw.v), q_t::FRAC_BITS);
    return fx::rne_shr(s * i64(n.v), q_t::FRAC_BITS);
}
inline local_t local_from_q(q_t q) { return i64(q.v); }
inline local_t local_one()         { return i64(q_t::ONE); }

// --- velocity pass (ALLOY S5) --------------------------------------------------------------
// vacc at frac 36 (= vel_t raw << 16), i64.
inline local_t vacc_from_vel(vel_t v)   { return i64(v.v) * 65536; }
// The velocity pass narrows the frac-36 accumulator once; a value outside vel_t is clamped and
// COUNTED (the same row saturation vel_from_delta reports).
inline vel_t   vel_from_vacc(local_t a, u32* sat_hits) {
    const i64 q = fx::rne_shr(a, 16);
    if (!fx::fx_fits<vel_t>(q)) { *sat_hits += 1; return fx::fx_raw<vel_t>(q > 0 ? INT32_MAX : INT32_MIN); }
    return fx::fx_raw<vel_t>(i32(q));
}
// w * dlambda with NO 8 m clamp: the PBF path multiplies this by a small non-unit gradient, so the
// bound belongs on the applied correction (corr_clamp30), not on the multiplier.
// docs/CANON.md V_MAX_WORLD (512 m/s) is validator-enforced in the sim (T-A-02): the bench
// clamps each velocity component to it at the end of the velocity pass and COUNTS the clamps
// (G-02b's 4096:1 plank launched by the V_MAX boulder is the case that reaches it).
inline vel_t vel_clamp_vmax(vel_t v, u32* clamps) {
    if (v.v > fx::V_MAX_WORLD.v) { *clamps += 1; return fx::V_MAX_WORLD; }
    if (v.v < -fx::V_MAX_WORLD.v) { *clamps += 1; return fx::fx_raw<vel_t>(-fx::V_MAX_WORLD.v); }
    return v;
}
inline local_t corr_mag_unbounded(invmass_t w, local_t dl30) {
    const i64 p = fx::sat_mul(i64(w.v), dl30);
    TL_ASSERT(p != INT64_MAX && p != INT64_MIN);
    return fx::rne_shr(p, invmass_t::FRAC_BITS);
}
// The 8 m per-constraint-per-substep sanity bound on a frac-30 correction, clamped and counted.
inline local_t corr_clamp30(local_t c, u32* clamps) {
    if (c >= (i64(1) << 33) || c <= -(i64(1) << 33)) { *clamps += 1; return c > 0 ? (i64(1) << 33) - 1 : -((i64(1) << 33) - 1); }
    return c;
}
// dvl = dvn / wsum at frac 20 (a velocity per unit inverse mass): rne_div(dvn * 2^18, wsum).
inline local_t dvl_of(vel_t dvn, invmass_t wsum) { TL_ASSERT(wsum.v > 0); return fx::rne_div(i64(dvn.v) * (i64(1) << invmass_t::FRAC_BITS), i64(wsum.v)); }
// carrier share: (w * dvl) >> 18 -> frac 20, then x n (>>30) into frac 36: << 16 first.
inline local_t vacc_term(invmass_t w, local_t dvl, q_t n) {
    const i64 dv = fx::rne_shr(i64(w.v) * dvl, invmass_t::FRAC_BITS);      // frac 20
    return fx::rne_shr(dv * i64(n.v), q_t::FRAC_BITS - 16);                  // frac 36
}
// XSPH: c_visc (q_t) x W (q_t) x (vj - vi) (vel_t): (c*W) >> 30 as q_t, x dv -> frac 50 -> >>14 = frac 36.
inline local_t xsph_term(q_t c, q_t w, vel_t dv) {
    const i64 cw = fx::rne_shr(fx::mul_widen(c.v, w.v), q_t::FRAC_BITS);
    return fx::rne_shr(cw * i64(dv.v), q_t::FRAC_BITS - 16);
}

// --- box SDF (docs/GATE0-BENCH.md §8.3 step 2: sd = max(|lx| - hw, |ly| - hh), analytic) ---
// Returns the signed distance of local point l to the box (hw, hh) and the unit outward normal
// by the dominant axis. Exact in pos_t; the normal is one of four axis vectors (exact q_t).
// `hint` breaks the tie at a corner (|lx| - hw == |ly| - hh, every corner of an aligned stack):
// the axis along which the querying body's centre (in this box's space) is further out, scaled
// by the extents, wins - the face that centre is facing. Ties within a quarter texel count.
inline pos_t box_sdf(vec2<pos_t> l, pos_t hw, pos_t hh, vec2<pos_t> hint, vec2<q_t>* n_out) {
    const pos_t dx = fx::abs(l.x) - hw;
    const pos_t dy = fx::abs(l.y) - hh;
    const i64 tie = i64(dx.v) - i64(dy.v);
    bool use_x = dx.v >= dy.v;
    if (tie <= fx::TEXEL.v / 4 && tie >= -(fx::TEXEL.v / 4)) {
        const i64 hx = (i64(hint.x.v) < 0 ? -i64(hint.x.v) : i64(hint.x.v)) * i64(hh.v);
        const i64 hy = (i64(hint.y.v) < 0 ? -i64(hint.y.v) : i64(hint.y.v)) * i64(hw.v);
        use_x = hx > hy;
    }
    if (use_x) { *n_out = { fx::fx_raw<q_t>(l.x.v < 0 ? -q_t::ONE : q_t::ONE), fx::fx_raw<q_t>(0) }; return dx; }
    *n_out = { fx::fx_raw<q_t>(0), fx::fx_raw<q_t>(l.y.v < 0 ? -q_t::ONE : q_t::ONE) };
    return dy;
}

// --- broadphase keys (docs/ALLOY.md §14.4.B, verbatim) ------------------------------------
// fine cell = 4 texels = raw 1 << 16; coarse cell = 1 m = raw 1 << 18 (the bench's body cell,
// docs/GATE0-BENCH.md §8.2). Arithmetic shift of a signed value is defined (C++20).
inline u32 fine_cx(pos_t x)   { return u32((x.v >> 16) + 32768); }
inline u32 coarse_cx(pos_t x) { return u32((x.v >> 18) + 8192); }
inline pos_t pos_from_raw(i64 raw) { TL_ASSERT(fx::fx_fits<pos_t>(raw)); return fx::fx_raw<pos_t>(i32(raw)); }
// |d|^2 < h^2 on the exact Q36 squares (the broadphase's dist2 test, docs/ALLOY.md §14.4.B).
inline bool  within_radius(vec2<pos_t> d, pos_t h) { return fx::len2_wide(d) < fx::mul_wide(h, h); }
// a * k for a small plain integer (the per-tick travel = per-substep delta x substeps), saturating.
inline pos_t pos_mul_int(pos_t a, i32 k) { return fx::fx_raw<pos_t>(fx::sat_mul(a.v, k)); }
// (a * b) >> 30 on two frac-30 locals, saturating product asserted not to saturate.
inline local_t local_mul30(local_t a, local_t b) { const i64 p = fx::sat_mul(a, b); TL_ASSERT(p != INT64_MAX && p != INT64_MIN); return fx::rne_shr(p, LOCAL_FRAC); }

// --- the normalize-once pair kernel (docs/FX-PALETTE.md §3 q_t row: "normalize once per pair";
// the 91 ns form docs/GATE0-BENCH.md §7 R-3 names). ONE root (the fx::len isqrt64) and ONE
// reciprocal (rne_div) per pair, against the rev-1 spelling's two roots + three divisions:
//   q = r / h_kernel is an EXACT shift (h_kernel raw is a power of two; q.v = r.v << q_shift
//   equals div<q_t>(r, h) bit for bit - consts_make asserts the precondition);
//   n = d * (2^48 / r.v) >> 18: the reciprocal 1/r at frac 30 (1/m) via one rne_div, then a
//   multiply per component. Its ROUNDING DIFFERS from normalize()'s div<q_t>(d.x, r) per
//   component, so the physics is a different evaluation order from the rev-1 bench: the re-run
//   is judged, not reproduced (TODO.md, the post-rulings closeout slice).
// Returns false when r == 0: n is undefined (left zero); q = 0 and W(0) stay valid.
struct PairGeom { pos_t r; q_t q; vec2<q_t> n; };
inline bool pair_geom(vec2<pos_t> d, pos_t hk, u32 q_shift, PairGeom* g) {
    (void)hk;                                                     // the fx side derives q from q_shift (hk is the double binding's operand)
    const pos_t r = fx::len(d);                                   // the one isqrt64
    g->r = r;
    const i64 qv = i64(r.v) * (i64(1) << q_shift);
    TL_ASSERT(fx::fx_fits<q_t>(qv));                              // callers test within_radius(d, h) first
    g->q = fx::fx_raw<q_t>(i32(qv));
    if (r.v == 0) { g->n = { fx::fx_raw<q_t>(0), fx::fx_raw<q_t>(0) }; return false; }
    const i64 inv_r = fx::rne_div(i64(1) << 48, i64(r.v));        // the one reciprocal: 1/r at frac 30 (1/m)
    g->n = { fx::fx_raw<q_t>(i32(fx::rne_shr(i64(d.x.v) * inv_r, pos_t::FRAC_BITS))),
             fx::fx_raw<q_t>(i32(fx::rne_shr(i64(d.y.v) * inv_r, pos_t::FRAC_BITS))) };   // |d.c| <= r keeps the product < 2^49 and |n| <= ONE + rounding
    return true;
}
// q = r / h_kernel by the same exact shift, for the pair walks that need no direction (XSPH).
inline q_t q_of_r(pos_t r, pos_t hk, u32 q_shift) {
    (void)hk;
    const i64 qv = i64(r.v) * (i64(1) << q_shift);
    TL_ASSERT(fx::fx_fits<q_t>(qv));
    return fx::fx_raw<q_t>(i32(qv));
}

#else
// ---- the double mirror: same names, same argument orders, libm arithmetic ------------------
using pos_t     = double;
using vel_t     = double;
using invmass_t = double;
using stiff_t   = double;
using q_t       = double;
using angle_t   = double;   // turns
using omega_t   = double;   // turns/s
using dt_t      = double;
using lambda_t  = double;
using scalar_t  = double;
using local_t   = double;
template <typename T> struct vec2 { T x; T y; };

constexpr double LOCAL_ONE_SHIFT = 1.0;
constexpr int    LOCAL_FRAC      = 30;   // the fx side's; unused here beyond the shared constant name

inline double fxd(i64 v, int frac) { return double(v) / double(i64(1) << frac); }
inline pos_t     cvt_pos(::pos_t x)     { return fxd(x.v, 18); }
inline vel_t     cvt_vel(::vel_t x)     { return fxd(x.v, 20); }
inline invmass_t cvt_w(::invmass_t x)   { return fxd(x.v, 18); }
inline stiff_t   cvt_stiff(::stiff_t x) { return fxd(x.v, 30); }
inline q_t       cvt_q(::q_t x)         { return fxd(x.v, 30); }
inline angle_t   cvt_ang(::angle_t x)   { return fxd(x.v, 30); }
inline omega_t   cvt_omega(::omega_t x) { return fxd(x.v, ::omega_t::FRAC_BITS); }   // 22 at rev 2 - derived, not restated (this literal sat at 20 when the row moved)
inline dt_t      cvt_dt(::dt_t x)       { return fxd(x.v, 30); }
inline scalar_t  cvt_scalar(::scalar_t x) { return fxd(x.v, 16); }
inline lambda_t  lam_zero()             { return 0.0; }
inline pos_t     pos_zero()             { return 0.0; }
inline vel_t     vel_zero()             { return 0.0; }
inline q_t       q_zero()               { return 0.0; }
inline q_t       q_one()                { return 1.0; }
inline angle_t   ang_zero()             { return 0.0; }
inline omega_t   omega_zero()           { return 0.0; }
inline local_t   local_zero()           { return 0.0; }
inline bool      is_zero_w(invmass_t w) { return w == 0.0; }
inline bool      is_zero_pos(pos_t x)   { return x == 0.0; }
inline bool      lam_is_zero(lambda_t l){ return l == 0.0; }
// "raw" on this side = the value scaled to the fx row's quantum, rounded - so the two sides
// compare in the same unit (docs/GATE0-BENCH.md §4 shadow CSV: max |fx - double|).
inline i64 raw_pos(pos_t x)    { return (i64)llround(x * 262144.0); }
inline i64 raw_vel(vel_t x)    { return (i64)llround(x * 1048576.0); }
inline i64 raw_q(q_t x)        { return (i64)llround(x * 1073741824.0); }
inline i64 raw_ang(angle_t x)  { return (i64)llround(x * 1073741824.0); }
inline i64 raw_lam(lambda_t x) { return (i64)llround(x * 65536.0); }
inline i64 raw_local(local_t x){ return (i64)llround(x * 1073741824.0); }

inline local_t xl_from_pos(pos_t x)      { return x; }
inline pos_t   pos_from_xl(local_t xl)   { return xl; }
inline i32     residual_of(local_t, pos_t) { return 0; }
inline local_t xl_with_residual(pos_t x, i32) { return x; }
inline local_t thl_from_angle(angle_t a) { return a; }
inline angle_t angle_from_thl(local_t t) { double r = t - floor(t + 0.5); if (r <= -0.5) r += 1.0; return r; }
inline i32     residual_of_th(local_t, angle_t) { return 0; }
inline local_t thl_with_residual(angle_t th, i32) { return th; }

inline pos_t   predict_delta(vel_t v, dt_t h)        { return v * h; }
inline vel_t   vel_from_delta(pos_t d, i32 inv_h, u32*) { return d * double(inv_h); }
inline angle_t ang_delta(omega_t w, dt_t h)          { return w * h; }
inline omega_t omega_from_delta(angle_t d, i32 inv_h, u32* sat_hits) {
    const double lim = 0.25 * double(inv_h);
    double w = d * double(inv_h);
    if (w > lim || w < -lim) { *sat_hits += 1; w = w > 0 ? lim : -lim; }
    return w;
}
inline local_t local_scale_pos(local_t a, pos_t h) { return a * h; }
inline vel_t   vel_add(vel_t a, vel_t b)             { return a + b; }
inline vel_t   vel_sub(vel_t a, vel_t b)             { return a - b; }
inline pos_t   pos_add(pos_t a, pos_t b)             { return a + b; }
inline pos_t   pos_sub(pos_t a, pos_t b)             { return a - b; }
inline pos_t   pos_neg(pos_t a)                      { return -a; }
inline pos_t   pos_abs(pos_t a)                      { return fabs(a); }
inline bool    pos_lt(pos_t a, pos_t b)              { return a < b; }
inline pos_t   pos_max(pos_t a, pos_t b)             { return a < b ? b : a; }
inline pos_t   pos_min(pos_t a, pos_t b)             { return a < b ? a : b; }
inline angle_t ang_add(angle_t a, angle_t b)         { return a + b; }
inline angle_t ang_sub(angle_t a, angle_t b)         { return a - b; }
inline omega_t omega_add(omega_t a, omega_t b)       { return a + b; }
inline vel_t   vel_abs(vel_t a)                      { return fabs(a); }
inline bool    vel_lt(vel_t a, vel_t b)              { return a < b; }
inline vel_t   vel_neg(vel_t a)                      { return -a; }
inline pos_t   pos_scale_q(pos_t a, q_t q)           { return a * q; }
inline vel_t   vel_scale_q(vel_t a, q_t q)           { return a * q; }
inline q_t     q_mul(q_t a, q_t b)                   { return a * b; }
inline q_t     q_sub(q_t a, q_t b)                   { return a - b; }
inline q_t     q_add(q_t a, q_t b)                   { return a + b; }
inline q_t     q_neg(q_t a)                          { return -a; }
inline bool    q_lt(q_t a, q_t b)                    { return a < b; }
inline q_t     q_div(pos_t a, pos_t b)               { return a / b; }
inline q_t     q_div_q(q_t a, q_t b)                 { return a / b; }
inline q_t     q_sat(local_t q)                      { return q > 2.0 ? 2.0 : q < -2.0 ? -2.0 : q; }

inline void        sincos_of(angle_t a, q_t* s, q_t* c)          { const double r = a * 6.283185307179586476925286766559; *s = sin(r); *c = cos(r); }
inline vec2<pos_t> rot(vec2<pos_t> p, q_t s, q_t c)         { return { p.x * c - p.y * s, p.x * s + p.y * c }; }
inline pos_t       length(vec2<pos_t> d)                          { return sqrt(d.x * d.x + d.y * d.y); }
inline vec2<q_t>   unit(vec2<pos_t> d)                    { const double l = length(d); if (l == 0.0) return { 0.0, 0.0 }; return { d.x / l, d.y / l }; }
inline pos_t       dot_pn(vec2<pos_t> a, vec2<q_t> n)          { return a.x * n.x + a.y * n.y; }
inline pos_t       cross_pn(vec2<pos_t> r, vec2<q_t> n)        { return r.x * n.y - r.y * n.x; }
inline vec2<pos_t> vsub(vec2<pos_t> a, vec2<pos_t> b)          { return { a.x - b.x, a.y - b.y }; }
inline vec2<pos_t> vadd(vec2<pos_t> a, vec2<pos_t> b)          { return { a.x + b.x, a.y + b.y }; }
inline vec2<q_t>   perp(vec2<q_t> n)                           { return { -n.y, n.x }; }
inline vec2<q_t>   qneg(vec2<q_t> n)                           { return { -n.x, -n.y }; }
inline vec2<q_t>   rotate_q(vec2<q_t> n, q_t s, q_t c)         { return { n.x * c - n.y * s, n.x * s + n.y * c }; }
inline vel_t       dot_vn(vec2<vel_t> v, vec2<q_t> n)          { return v.x * n.x + v.y * n.y; }

inline invmass_t w_clamp(invmass_t w, invmass_t other) { if (w == 0.0) return w; const double f = other / 4096.0; return w < f ? f : w; }
inline local_t   w_ang30(invmass_t inv_i, pos_t rn)    { return rn * rn * inv_i; }
inline invmass_t w_add(invmass_t a, invmass_t b)       { return a + b; }
inline local_t   den_of(invmass_t wa, invmass_t wb, stiff_t at) { return wa + wb + at; }
inline local_t   den_add(local_t a, local_t b)         { return a + b; }
inline bool      den_is_zero(local_t d)               { return d == 0.0; }
inline local_t   num_of30(local_t c, stiff_t at, local_t lam) { return -c - at * lam; }
inline local_t   c30_from_pos(pos_t c)                 { return c; }
inline local_t   dlam_of(local_t num, local_t den, u32, u32*) { return num / den; }
inline lambda_t  lam_narrow(local_t l, u32*)           { return l; }
inline local_t   lam_widen(lambda_t l)                 { return l; }
inline local_t   local_max(local_t a, local_t b)       { return a < b ? b : a; }
inline local_t   corr_mag(invmass_t w, local_t dl, u32*) { return w * dl; }
inline local_t   corr_comp(local_t mag, q_t n)         { return mag * n; }
inline local_t   corr_comp30(local_t mag, local_t g)   { return mag * g; }
inline local_t   ang_corr(invmass_t inv_i, local_t dl, pos_t rn, q_t inv_two_pi) { return inv_i * dl * rn * inv_two_pi; }
inline local_t   rad30_from_turn30(local_t dth, scalar_t two_pi) { return dth * two_pi; }
inline local_t   dot30_n(local_t dx, local_t dy, vec2<q_t> n) { return dx * n.x + dy * n.y; }
inline local_t   rad_times_rn(local_t rad, pos_t rn)   { return rad * rn; }
inline local_t   local_add(local_t a, local_t b)       { return a + b; }
inline local_t   local_sub(local_t a, local_t b)       { return a - b; }
inline local_t   local_neg(local_t a)                  { return -a; }
inline local_t   local_abs(local_t a)                  { return fabs(a); }
inline bool      local_lt(local_t a, local_t b)        { return a < b; }
inline local_t   local_scale_q(local_t a, q_t q)       { return a * q; }
inline local_t   den_grad(invmass_t w, local_t gx, local_t gy) { return (gx * gx + gy * gy) * w; }
inline local_t   rho_term(q_t kw, q_t w)               { return kw * w; }
inline local_t   rho_round(local_t acc)                { return acc; }
inline local_t   grad_term(q_t kw, q_t dw, q_t n)      { return kw * dw * n; }
inline local_t   local_from_q(q_t q)                   { return q; }
inline local_t   local_one()                           { return 1.0; }

inline local_t vacc_from_vel(vel_t v)   { return v; }
inline vel_t   vel_from_vacc(local_t a, u32*) { return a; }
inline local_t corr_mag_unbounded(invmass_t w, local_t dl) { return w * dl; }
inline vel_t vel_clamp_vmax(vel_t v, u32* clamps) { if (v > 512.0) { *clamps += 1; return 512.0; } if (v < -512.0) { *clamps += 1; return -512.0; } return v; }
inline local_t corr_clamp30(local_t c, u32*) { return c; }
inline local_t dvl_of(vel_t dvn, invmass_t wsum) { return dvn / wsum; }
inline local_t vacc_term(invmass_t w, local_t dvl, q_t n) { return w * dvl * n; }
inline local_t xsph_term(q_t c, q_t w, vel_t dv) { return c * w * dv; }

inline pos_t box_sdf(vec2<pos_t> l, pos_t hw, pos_t hh, vec2<pos_t> hint, vec2<q_t>* n_out) {
    const double dx = fabs(l.x) - hw;
    const double dy = fabs(l.y) - hh;
    const double tie = dx - dy;
    bool use_x = dx >= dy;
    if (tie <= 1.0 / 64.0 && tie >= -1.0 / 64.0) { use_x = fabs(hint.x) * hh > fabs(hint.y) * hw; }
    if (use_x) { *n_out = { l.x < 0.0 ? -1.0 : 1.0, 0.0 }; return dx; }
    *n_out = { 0.0, l.y < 0.0 ? -1.0 : 1.0 };
    return dy;
}
// The shadow world keys its own grid on the same cells as the fx world would at these positions
// (floor to the fx quantum first, so a particle sitting on a cell boundary lands the same way).
inline u32 fine_cx(pos_t x)   { return u32(((i64)floor(x * 262144.0) >> 16) + 32768); }
inline u32 coarse_cx(pos_t x) { return u32(((i64)floor(x * 262144.0) >> 18) + 8192); }
inline pos_t pos_from_raw(i64 raw) { return fxd(raw, 18); }
inline bool  within_radius(vec2<pos_t> d, pos_t h) { return d.x * d.x + d.y * d.y < h * h; }
inline pos_t pos_mul_int(pos_t a, i32 k) { return a * double(k); }
inline local_t local_mul30(local_t a, local_t b) { return a * b; }

// The double mirror of the normalize-once pair kernel: same structure (one sqrt, one reciprocal,
// multiplies), so the two bindings change evaluation order together.
struct PairGeom { pos_t r; q_t q; vec2<q_t> n; };
inline bool pair_geom(vec2<pos_t> d, pos_t hk, u32, PairGeom* g) {
    const double r = sqrt(d.x * d.x + d.y * d.y);
    g->r = r;
    g->q = r / hk;
    if (r == 0.0) { g->n = { 0.0, 0.0 }; return false; }
    const double ir = 1.0 / r;
    g->n = { d.x * ir, d.y * ir };
    return true;
}
inline q_t q_of_r(pos_t r, pos_t hk, u32) { return r / hk; }
#endif

}  // namespace G0_NS
