#pragma once
// ---------------------------------------------------------------------------------------------
// det_math.h - deterministic kernels over the palette: sqrt, sincos, atan2, vec2.
//
// Spec: docs/FX-PALETTE.md §4 (det math: what is ours, what is ported), §4.2 (the API shape),
//   §4.3 (vec2), §4.4 (the three-layer oracle), §10.3 (this header).
// Purpose: every transcendental the sim needs, bit-exact by construction. sqrt is an exact
//   integer sqrt (correctly rounded - no polynomial); sin/cos/atan2 are FixPointCS ports
//   (github.com/XMunkki/FixPointCS, MIT, (c) Jere Sanisalo, Petri Kero - attribution and the
//   verbatim coefficients are in det_math.cpp). One deliberate deviation from the reference:
//   angles are TURNS (angle_t), so range reduction is an exact mask, never a mod-2pi.
// Invariants: the error bound of each kernel is recorded next to it (measured by tools/fxcheck;
//   "max |err|" is over the whole input domain, in ulps of the result format). sin(0) == 0,
//   sin(QUARTER_TURN) == q_t::ONE exactly; sqrt of a perfect square is exact.
// Determinism: pure functions, no state, integer-only; the coefficient tables are const data.
//   A call with an out-of-domain argument (sqrt of a negative, normalize of a zero vector,
//   atan2(0,0)) asserts in debug/dev and returns the documented value in every tier.
// Threading: none - safe from any worker.
// Includes: foundation/fx_palette.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/fx_palette.h"

namespace fx {

// --- integer square roots (det_math.cpp) ----------------------------------------------------

// floor(sqrt(x)), exact for every u32 (bit-by-bit restoring; 16 iterations, no data-dependent
// branch on the value). Never fails.
u32 isqrt32(u32 x);
// floor(sqrt(x)), exact for every u64 (32 iterations). Never fails; the quanta-path sqrt
// (geometric-mean conductivity) is this function on a plain integer.
u64 isqrt64(u64 x);

// --- sqrt / rsqrt ---------------------------------------------------------------------------

// sqrt of x in format R, correctly rounded (nearest; a tie cannot occur). S = 2*R::FRAC -
// A::FRAC must be in [0, 30] (static). Precondition x >= 0 (asserted; returns 0 otherwise) and
// the result fits R (asserted). sqrt<pos_t>(pos2_wide_t) is the squared-distance path (S = 0).
template <typename R, typename A>
inline R sqrt(A x) {
    constexpr int S = 2 * R::FRAC_BITS - A::FRAC_BITS;
    static_assert(S >= 0 && S <= 30, "sqrt: 2*R::FRAC - A::FRAC must be in [0, 30] (docs/FX-PALETTE.md section 10.3)");
    static_assert(sizeof(typename R::rep) == 4, "rev 1: 32-bit result rows only");
    TL_ASSERT(x.v >= 0);
    if (x.v < 0) return fx_raw<R>(0);
    const u64 n = u64(x.v) << S;                           // x.v >= 0: no sign bit to lose
    u64 y = isqrt64(n);                                    // floor
    if (n - y * y > y) y += 1;                             // nearest: frac >= 1/2 <=> n - y^2 > y
    TL_ASSERT(y <= u64(INT32_MAX));
    return fx_raw<R>(i32(y));
}

// 1 / sqrt(x) in format R as div<R>(1, sqrt<R>(x)) - never an estimate instruction. Requires
// the same-row quotient (R, R, R) in the op table. Precondition x > 0 and the result in R's
// range (both asserted; x == 0 yields the saturated quotient of div<R>).
template <typename R, typename A>
inline R rsqrt(A x) {
    TL_ASSERT(x.v > 0);
    return div<R>(fx_int<R>(1), sqrt<R>(x));
}

// --- trigonometry in turns (det_math.cpp) ---------------------------------------------------

// Measured error bounds, in ulps of the result format (q_t / angle_t, 2^-30), over the WHOLE
// input domain: tools/fxcheck sweeps all 2^30 turn fractions for sin/cos and 2^24 seeded pairs
// plus every octant boundary for atan2, against a long double reference, and
// tools/fxcheck/oracle.py re-evaluates the argmax at 60 digits (2026-08-23, W1 fx lane). The
// tests assert against these; re-measure and re-pin them whenever a coefficient changes.
//   sin/cos      : max |err| 9.0584 ulp at raw 759040234 (the reference polynomial is "27.13
//                  bits" = 7.3 ulp of Q30, plus the RNE steps). docs/FX-PALETTE.md section
//                  10.5's 2-ulp figure was unattainable with SinPoly4; a tighter kernel is a
//                  ruling (TODO.md).
//   sin^2 + cos^2: within 18 ulp of 1 (two ~9-ulp kernels, doubled by the squaring).
//   atan2        : max |err| 4.3359 ulp (2^24 samples); atan2(sin a, cos a) returns a within 4.
constexpr i32 FX_SIN_MAX_ERR_ULP             = 10;
constexpr i32 FX_SIN2COS2_MAX_ERR_ULP        = 19;
constexpr i32 FX_ATAN2_MAX_ERR_ULP           = 5;
constexpr i32 FX_ATAN2_ROUNDTRIP_MAX_ERR_ULP = 5;

// sin and cos of a in one reduction. a is reduced mod 1 turn by masking (exact), so any
// angle_t value is valid. Max |err| vs the true value: FX_SIN_MAX_ERR_ULP (measured over all
// 2^30 turn fractions). sin(0) == 0 and sin(QUARTER_TURN) == q_t::ONE exactly; |s|, |c| <= ONE
// always; sin(-a) == -sin(a), cos(-a) == cos(a), sin(a + 1/4) == cos(a) bit-exactly.
void sincos(angle_t a, q_t* s, q_t* c);
// sin(a) alone; same contract as sincos.
q_t sin(angle_t a);
// cos(a) alone; same contract as sincos.
q_t cos(angle_t a);

// Angle of the vector (x, y) in turns, in [-HALF_TURN, HALF_TURN] - CLOSED at both ends: y < 0
// with |y| / |x| below ~2^-31 rounds to -HALF_TURN, the same angle as +HALF_TURN mod one turn
// (mask with TURN.v - 1 before comparing angles; never test == HALF_TURN). Octant reduction on
// (|y|, |x|), one exact RNE ratio, the FixPointCS polynomial (coefficients pre-scaled to turns),
// unfold. Max |err|: FX_ATAN2_MAX_ERR_ULP. Axes and diagonals are exact; atan2(-y, x) ==
// -atan2(y, x) and atan2(x, y) == 1/4 - atan2(y, x) (mod 1) bit-exactly. atan2(0, 0) asserts
// and returns 0.
angle_t atan2(pos_t y, pos_t x);
// The q_t overload of atan2 (unit vectors, normals); same contract.
angle_t atan2q(q_t y, q_t x);

// --- interpolation --------------------------------------------------------------------------

// a + (b - a) * t, one RNE; t in q_t (any value - extrapolation is allowed). Precondition:
// b - a does not wrap (asserted indirectly by mul<R>'s range check in debug/dev).
template <typename R>
inline R lerp(R a, R b, q_t t) {
    return a + mul<R>(b - a, t);
}

// --- vec2 (docs/FX-PALETTE.md §4.3) ---------------------------------------------------------

template <typename T>
struct vec2 {
    T x;
    T y;
};

// Component-wise a + b, wraps (same format only).
template <typename T>
constexpr vec2<T> operator+(vec2<T> a, vec2<T> b) { return { a.x + b.x, a.y + b.y }; }
// Component-wise a - b, wraps.
template <typename T>
constexpr vec2<T> operator-(vec2<T> a, vec2<T> b) { return { a.x - b.x, a.y - b.y }; }
// Component-wise negation, wraps.
template <typename T>
constexpr vec2<T> operator-(vec2<T> a) { return { -a.x, -a.y }; }
// Bitwise equality of both components.
template <typename T>
constexpr bool operator==(vec2<T> a, vec2<T> b) { return a.x == b.x && a.y == b.y; }
// Bitwise inequality.
template <typename T>
constexpr bool operator!=(vec2<T> a, vec2<T> b) { return !(a == b); }

// a.x*b.x + a.y*b.y in format R: two exact i64 products, a saturating i64 sum (asserted not to
// saturate), one RNE shift by A::FRAC + B::FRAC - R::FRAC. Listed in the op table as (R, A, B).
// dot<pos2_wide_t>(vec2<pos_t>, vec2<pos_t>) is the squared-distance path (shift 0).
template <typename R, typename A, typename B>
inline R dot(vec2<A> a, vec2<B> b) {
    static_assert(fx_op_allowed<R, A, B>::value, "dot: product not in the mixed-op table (docs/FX-PALETTE.md section 3.1)");
    static_assert(sizeof(typename A::rep) == 4 && sizeof(typename B::rep) == 4, "rev 1: 32-bit operands only");
    constexpr int S = A::FRAC_BITS + B::FRAC_BITS - R::FRAC_BITS;
    static_assert(S >= 0 && S <= 62, "");
    const i64 sum = sat_add(i64(a.x.v) * i64(b.x.v), i64(a.y.v) * i64(b.y.v));
    TL_ASSERT(sum != INT64_MAX && sum != INT64_MIN);
    const i64 q = rne_shr(sum, S);
    TL_ASSERT(fx_fits<R>(q));
    return fx_raw<R>(typename R::rep(q));
}

// a.x*b.y - a.y*b.x in format R (the 2D cross product / perp-dot); same arithmetic as dot.
// cross<pos_t>(r_world, n) is the body angular term of docs/ALLOY.md §14.4.3.
template <typename R, typename A, typename B>
inline R cross(vec2<A> a, vec2<B> b) {
    static_assert(fx_op_allowed<R, A, B>::value, "cross: product not in the mixed-op table (docs/FX-PALETTE.md section 3.1)");
    static_assert(sizeof(typename A::rep) == 4 && sizeof(typename B::rep) == 4, "rev 1: 32-bit operands only");
    constexpr int S = A::FRAC_BITS + B::FRAC_BITS - R::FRAC_BITS;
    static_assert(S >= 0 && S <= 62, "");
    const i64 d = sat_sub(i64(a.x.v) * i64(b.y.v), i64(a.y.v) * i64(b.x.v));
    TL_ASSERT(d != INT64_MAX && d != INT64_MIN);
    const i64 q = rne_shr(d, S);
    TL_ASSERT(fx_fits<R>(q));
    return fx_raw<R>(typename R::rep(q));
}

// |d|^2 as the raw Q36 local (pos2_wide_t bits), exact. Precondition |d|^2 < 2^27 m^2
// (asserted; the broadphase guarantees it, docs/FX-PALETTE.md fx_palette.h headroom note).
inline i64 len2_wide(vec2<pos_t> d) {
    const i64 s = sat_add(mul_wide(d.x, d.x), mul_wide(d.y, d.y));
    TL_ASSERT(s != INT64_MAX);
    return s;
}

// |d| in pos_t, correctly rounded (sqrt<pos_t> of the exact Q36 square). Precondition as
// len2_wide, plus |d| < 8,192 m so the result fits (asserted).
inline pos_t len(vec2<pos_t> d) {
    return sqrt<pos_t>(fx_raw<pos2_wide_t>(len2_wide(d)));
}

// d / |d| as a q_t unit vector: len, then two div<q_t>. A zero vector asserts and returns
// (0, 0) - callers guard first; the contact and PBF code never normalize a zero vector.
inline vec2<q_t> normalize(vec2<pos_t> d) {
    const pos_t l = len(d);
    TL_ASSERT(l.v != 0);
    if (l.v == 0) return { fx_raw<q_t>(0), fx_raw<q_t>(0) };
    return { div<q_t>(d.x, l), div<q_t>(d.y, l) };
}

// p rotated by the angle whose (sin, cos) is (s, c): four mul<pos_t>, two adds. Quarter turns
// with exact (0, +-1) pairs are bit-exact.
inline vec2<pos_t> rotate(vec2<pos_t> p, q_t s, q_t c) {
    return { mul<pos_t>(p.x, c) - mul<pos_t>(p.y, s), mul<pos_t>(p.x, s) + mul<pos_t>(p.y, c) };
}

}  // namespace fx

static_assert(__is_trivially_copyable(fx::vec2<pos_t>) && sizeof(fx::vec2<pos_t>) == 8, "vec2 is two rows, nothing else");
