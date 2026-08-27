#pragma once
// ---------------------------------------------------------------------------------------------
// fx.h - the typed fixed-point mechanism: fx<Rep, FRAC>, the named arithmetic, the quanta helpers.
//
// Spec: docs/FX-PALETTE.md §1 (mechanism), §10.1 (this header); docs/CPP-SUBSET.md §5 (the
//   arithmetic helpers that are the only arithmetic on quanta paths).
// Purpose: determinism by construction. Integer add/sub/mul/shift are bit-exact on every ISA
//   and compiler, so a sim written entirely in these helpers is bit-exact cross-ISA for free.
//   The cost is range/resolution bookkeeping, which is why this is a TYPED palette with a CLOSED
//   op table and no implicit conversions, not a "fixed-point number" type.
// Invariants: `+`/`-`/comparisons compile only between identical formats (one template on one
//   format). `*` and `/` are never operators: mul<R>/div<R> name the RESULT format explicitly,
//   widen to i64 inside, round to nearest even at the one visible narrowing point, and compile
//   only for products listed in fx_op_allowed (the closed table fx_palette.h fills). Plain
//   operators wrap (two's complement, explicit policy); quanta paths use the sat_* tier.
//   fx_raw<R> is the ONLY bit-level constructor (greppable); to<R> is the ONLY conversion.
// Determinism: header-only, no state, no floats, no libm, no platform branch. Rounding is RNE
//   everywhere a value narrows (rne_shr / rne_div are the two primitives; nothing else rounds).
//   Left shifts of signed values are written as multiplies by a power of two (a negative left
//   shift is UB, docs/CPP-SUBSET.md §5); right shifts of negatives are arithmetic (C++20).
// Threading: none - pure functions over values; safe from any worker.
// Includes: foundation/tl_types.h (widths, ErrCode/Result<T>), foundation/tl_assert.h (the range
//   asserts), foundation/strview.h (StrView - `fx_parse_decimal_raw`'s input, RR-38, §9 R-10, §10.1).
// Rev 1: the palette instantiates Rep = i32 only; fx<i64, F> exists as a LOCAL type (widened
//   accumulators: fx<i64,36> squared distances, fx<i64,30> solver locals) with +/-/compare, and
//   every narrowing helper static_asserts 32-bit operands until a rung-4 row exists (§3.2).
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/strview.h"

namespace fx {

// The tag that makes raw construction greppable: only fx_raw<R> spells it.
struct FxRawTag {};

template <typename Rep, int FRAC>
struct fx {
    static_assert(__is_same(Rep, i32) || __is_same(Rep, i64), "fx rep is i32 (rows) or i64 (locals)");
    static_assert(FRAC >= 0 && FRAC <= int(sizeof(Rep)) * 8 - 2, "FRAC leaves at least one integer bit + sign");
    using rep = Rep;
    static constexpr int FRAC_BITS = FRAC;
    static constexpr int INT_BITS  = int(sizeof(Rep)) * 8 - 1 - FRAC;   // magnitude bits above the point
    static constexpr Rep ONE       = Rep(1) << FRAC;

    Rep v;

    // Trivial default: `pos_t p;` is as uninitialised as an i32; `pos_t p{}` is zero.
    constexpr fx() = default;
    // The bit-level constructor; reachable only through fx_raw<R> (the tag is the grep handle).
    explicit constexpr fx(FxRawTag, Rep raw) : v(raw) {}
};

// The closed mixed-op table (docs/FX-PALETTE.md §3.1). Primary = false: an unlisted product is a
// compile error naming this line. fx_palette.h supplies one specialisation per sanctioned row;
// B = i32 marks a plain-integer factor (mul_int). Nothing else in the codebase specialises it.
template <typename R, typename A, typename B>
struct fx_op_allowed { static constexpr bool value = false; };

// --- bit-level and integer construction -----------------------------------------------------

// The only bit-level constructor. `bits` is the raw representation; no scaling, no check.
template <typename R>
constexpr R fx_raw(typename R::rep bits) { return R(FxRawTag{}, bits); }

// Integer i as R (i << FRAC, exact). Precondition |i| < 2^INT_BITS - asserted; a constant
// argument out of range is a compile error under constant evaluation.
template <typename R>
constexpr R fx_int(i32 i) {
    TL_ASSERT(i64(i) > -(i64(1) << R::INT_BITS) && i64(i) < (i64(1) << R::INT_BITS));
    return fx_raw<R>(typename R::rep(i64(i) * (i64(1) << R::FRAC_BITS)));
}

// Floor of x as an integer (arithmetic shift; -0.5 -> -1). Exact, never fails.
template <typename A>
constexpr typename A::rep fx_to_int_floor(A x) { return typename A::rep(x.v >> A::FRAC_BITS); }

// --- the two rounding primitives (nothing else in the sim rounds) ---------------------------

// Round-to-nearest-even arithmetic shift right by s in [0, 62]. Ties (remainder exactly half) go
// to the even quotient. Never overflows: |x >> s| + 1 fits for s >= 1.
constexpr i64 rne_shr(i64 x, int s) {
    if (s == 0) return x;
    TL_ASSERT(s >= 1 && s <= 62);
    const i64 q    = x >> s;                              // arithmetic (C++20)
    const i64 r    = x & ((i64(1) << s) - 1);             // remainder in [0, 2^s), also for x < 0
    const i64 half = i64(1) << (s - 1);
    return (r > half || (r == half && (q & 1))) ? q + 1 : q;
}

// RNE of the exact rational n/d. Preconditions d != 0, |d| < 2^62, and n != INT64_MIN when d == -1
// (the one overflowing quotient) - asserted. Ties go to the even quotient.
constexpr i64 rne_div(i64 n, i64 d) {
    TL_ASSERT(d != 0 && d > -(i64(1) << 62) && d < (i64(1) << 62));
    TL_ASSERT(!(d == -1 && n == -i64(0x7fffffffffffffff) - 1));
    i64 q = n / d;                                        // truncates toward zero
    const i64 r  = n % d;                                 // same sign as n, |r| < |d|
    const i64 r2 = (r < 0 ? -r : r) * 2;
    const i64 ad = d < 0 ? -d : d;
    if (r2 > ad || (r2 == ad && (q & 1))) q += ((n < 0) != (d < 0)) ? -1 : 1;
    return q;
}

// RNE of the rational num/den at R's quantum - for H, G_SUBSTEP, kernel coefficients. Constexpr;
// preconditions den != 0, |num| < 2^(63 - FRAC), |den| < 2^62, result in range - all asserted.
template <typename R>
constexpr R fx_lit(i64 num, i64 den) {
    TL_ASSERT(den != 0 && num > -(i64(1) << (63 - R::FRAC_BITS)) && num < (i64(1) << (63 - R::FRAC_BITS)));
    const i64 q = rne_div(num * (i64(1) << R::FRAC_BITS), den);
    TL_ASSERT(q >= -(i64(1) << (int(sizeof(typename R::rep)) * 8 - 1)) &&
              q <=  (i64(1) << (int(sizeof(typename R::rep)) * 8 - 1)) - 1);
    return fx_raw<R>(typename R::rep(q));
}

// --- integer helpers for quanta paths (docs/CPP-SUBSET.md §5) -------------------------------
// wrap_*: two's-complement wrap, explicit (the unsigned cast is the whole definition).
// sat_*: saturate at the type's range. mul_widen: exact 32x32 -> 64. mulhi64: high half of the
// signed 64x64 product, written as four 32-bit partial products so no compiler-rt routine or
// 128-bit type is involved.

// a + b modulo 2^32, two's complement; never traps, never saturates.
constexpr i32 wrap_add(i32 a, i32 b) { return i32(u32(a) + u32(b)); }
// a + b modulo 2^64, two's complement; never traps, never saturates.
constexpr i64 wrap_add(i64 a, i64 b) { return i64(u64(a) + u64(b)); }
// a - b modulo 2^32, two's complement; never traps, never saturates.
constexpr i32 wrap_sub(i32 a, i32 b) { return i32(u32(a) - u32(b)); }
// a - b modulo 2^64, two's complement; never traps, never saturates.
constexpr i64 wrap_sub(i64 a, i64 b) { return i64(u64(a) - u64(b)); }
// a * b mod 2^32 (also the sanctioned spelling of a left shift of a signed value).
constexpr i32 wrap_mul(i32 a, i32 b) { return i32(u32(a) * u32(b)); }
// a * b modulo 2^64, two's complement; never traps, never saturates.
constexpr i64 wrap_mul(i64 a, i64 b) { return i64(u64(a) * u64(b)); }
// -a mod 2^32 (wrap_neg(INT32_MIN) == INT32_MIN).
constexpr i32 wrap_neg(i32 a) { return i32(u32(0) - u32(a)); }
// -a modulo 2^64, two's complement (wrap_neg(INT64_MIN) == INT64_MIN).
constexpr i64 wrap_neg(i64 a) { return i64(u64(0) - u64(a)); }

// a + b clamped to [INT32_MIN, INT32_MAX].
constexpr i32 sat_add(i32 a, i32 b) {
    const i64 s = i64(a) + i64(b);
    return s > i64(INT32_MAX) ? INT32_MAX : s < i64(INT32_MIN) ? INT32_MIN : i32(s);
}
// a + b clamped to [INT64_MIN, INT64_MAX]; overflow iff a and b share a sign the sum lacks.
constexpr i64 sat_add(i64 a, i64 b) {
    const i64 s = wrap_add(a, b);
    if (((a ^ s) & (b ^ s)) < 0) return a < 0 ? INT64_MIN : INT64_MAX;
    return s;
}
// a - b clamped to [INT32_MIN, INT32_MAX].
constexpr i32 sat_sub(i32 a, i32 b) {
    const i64 s = i64(a) - i64(b);
    return s > i64(INT32_MAX) ? INT32_MAX : s < i64(INT32_MIN) ? INT32_MIN : i32(s);
}
// a - b clamped to [INT64_MIN, INT64_MAX]; overflow iff a and b differ in sign and s differs from a.
constexpr i64 sat_sub(i64 a, i64 b) {
    const i64 s = wrap_sub(a, b);
    if (((a ^ b) & (a ^ s)) < 0) return a < 0 ? INT64_MIN : INT64_MAX;
    return s;
}
// -a clamped (sat_neg(INT32_MIN) == INT32_MAX).
constexpr i32 sat_neg(i32 a) { return a == INT32_MIN ? INT32_MAX : -a; }
// -a clamped (sat_neg(INT64_MIN) == INT64_MAX).
constexpr i64 sat_neg(i64 a) { return a == INT64_MIN ? INT64_MAX : -a; }
// Exact product of two i32 as i64 (never overflows: |p| <= 2^62).
constexpr i64 mul_widen(i32 a, i32 b) { return i64(a) * i64(b); }
// a * b clamped to [INT32_MIN, INT32_MAX].
constexpr i32 sat_mul(i32 a, i32 b) {
    const i64 p = mul_widen(a, b);
    return p > i64(INT32_MAX) ? INT32_MAX : p < i64(INT32_MIN) ? INT32_MIN : i32(p);
}
// High 64 bits of the unsigned 128-bit product a * b.
constexpr u64 mulhi64u(u64 a, u64 b) {
    const u64 a_lo = a & 0xffffffffu, a_hi = a >> 32, b_lo = b & 0xffffffffu, b_hi = b >> 32;
    const u64 p0 = a_lo * b_lo, p1 = a_lo * b_hi, p2 = a_hi * b_lo, p3 = a_hi * b_hi;
    const u64 mid = (p0 >> 32) + (p1 & 0xffffffffu) + (p2 & 0xffffffffu);
    return p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}
// High 64 bits of the signed 128-bit product a * b (two's complement).
constexpr i64 mulhi64(i64 a, i64 b) {
    u64 hi = mulhi64u(u64(a), u64(b));
    if (a < 0) hi -= u64(b);
    if (b < 0) hi -= u64(a);
    return i64(hi);
}
// a * b clamped to [INT64_MIN, INT64_MAX]: overflow iff the high half is not the low half's sign.
constexpr i64 sat_mul(i64 a, i64 b) {
    const i64 lo = wrap_mul(a, b);
    const i64 hi = mulhi64(a, b);
    if (hi != (lo >> 63)) return ((a < 0) != (b < 0)) ? INT64_MIN : INT64_MAX;
    return lo;
}

// --- the palette arithmetic -----------------------------------------------------------------

// Range check of an i64 quotient against R's rep, shared by every narrowing helper.
template <typename R>
constexpr bool fx_fits(i64 q) {
    return sizeof(typename R::rep) == 4 ? (q >= i64(INT32_MIN) && q <= i64(INT32_MAX)) : true;
}

// Same format only: a + b, two's-complement wrap (explicit policy; quanta paths use sat_add).
template <typename Rep, int F>
constexpr fx<Rep, F> operator+(fx<Rep, F> a, fx<Rep, F> b) { return fx_raw<fx<Rep, F>>(wrap_add(a.v, b.v)); }
// Same format only: a - b, wraps.
template <typename Rep, int F>
constexpr fx<Rep, F> operator-(fx<Rep, F> a, fx<Rep, F> b) { return fx_raw<fx<Rep, F>>(wrap_sub(a.v, b.v)); }
// Unary minus, wraps (-MIN == MIN); sat_neg is the clamping form.
template <typename Rep, int F>
constexpr fx<Rep, F> operator-(fx<Rep, F> a) { return fx_raw<fx<Rep, F>>(wrap_neg(a.v)); }
// Same format only: in-place add, wraps.
template <typename Rep, int F>
constexpr fx<Rep, F>& operator+=(fx<Rep, F>& a, fx<Rep, F> b) { a.v = wrap_add(a.v, b.v); return a; }
// Same format only: in-place subtract, wraps.
template <typename Rep, int F>
constexpr fx<Rep, F>& operator-=(fx<Rep, F>& a, fx<Rep, F> b) { a.v = wrap_sub(a.v, b.v); return a; }
// Same format only: bitwise equality of the representation.
template <typename Rep, int F>
constexpr bool operator==(fx<Rep, F> a, fx<Rep, F> b) { return a.v == b.v; }
// Same format only.
template <typename Rep, int F>
constexpr bool operator!=(fx<Rep, F> a, fx<Rep, F> b) { return a.v != b.v; }
// Same format only: signed comparison of the representation.
template <typename Rep, int F>
constexpr bool operator<(fx<Rep, F> a, fx<Rep, F> b) { return a.v < b.v; }
// Same format only.
template <typename Rep, int F>
constexpr bool operator<=(fx<Rep, F> a, fx<Rep, F> b) { return a.v <= b.v; }
// Same format only.
template <typename Rep, int F>
constexpr bool operator>(fx<Rep, F> a, fx<Rep, F> b) { return a.v > b.v; }
// Same format only.
template <typename Rep, int F>
constexpr bool operator>=(fx<Rep, F> a, fx<Rep, F> b) { return a.v >= b.v; }

// Saturating a + b at the format's range (quanta-style safety for a row).
template <typename Rep, int F>
constexpr fx<Rep, F> sat_add(fx<Rep, F> a, fx<Rep, F> b) { return fx_raw<fx<Rep, F>>(sat_add(a.v, b.v)); }
// Saturating a - b.
template <typename Rep, int F>
constexpr fx<Rep, F> sat_sub(fx<Rep, F> a, fx<Rep, F> b) { return fx_raw<fx<Rep, F>>(sat_sub(a.v, b.v)); }
// Saturating negation (sat_neg(MIN) == MAX).
template <typename Rep, int F>
constexpr fx<Rep, F> sat_neg(fx<Rep, F> a) { return fx_raw<fx<Rep, F>>(sat_neg(a.v)); }
// |a|, saturating: abs(MIN) == MAX (a wrapping abs would return MIN, a negative number).
template <typename Rep, int F>
constexpr fx<Rep, F> abs(fx<Rep, F> a) { return a.v < 0 ? sat_neg(a) : a; }
// The smaller of two values of one format.
template <typename Rep, int F>
constexpr fx<Rep, F> min(fx<Rep, F> a, fx<Rep, F> b) { return a.v < b.v ? a : b; }
// The larger of two values of one format.
template <typename Rep, int F>
constexpr fx<Rep, F> max(fx<Rep, F> a, fx<Rep, F> b) { return a.v < b.v ? b : a; }
// x clamped to [lo, hi]; precondition lo <= hi (asserted).
template <typename Rep, int F>
constexpr fx<Rep, F> clamp(fx<Rep, F> x, fx<Rep, F> lo, fx<Rep, F> hi) {
    TL_ASSERT(lo.v <= hi.v);
    return x.v < lo.v ? lo : (hi.v < x.v ? hi : x);
}
// -1, 0 or +1 as a plain integer.
template <typename Rep, int F>
constexpr i32 sign(fx<Rep, F> a) { return a.v < 0 ? -1 : (a.v > 0 ? 1 : 0); }
// True iff the representation is exactly zero.
template <typename Rep, int F>
constexpr bool is_zero(fx<Rep, F> a) { return a.v == 0; }

// The ONLY conversion between formats (greppable). Widening is exact (shift left); narrowing is
// RNE. Precondition: the value fits R - asserted (a wider row narrowed out of range is a bug at
// the call site, not a saturation).
//
// Out of range with the assert compiled out (netcode/ship), documented rather than left silent
// (W1 runner review 1, TODO.md): the intermediate i64 is converted to R::rep, which since C++20
// is a WELL-DEFINED MODULAR conversion - it WRAPS, it does not saturate and it does not clamp.
// to<q_t>(fx_raw<pos_t>(1 << 19)) is the smallest case: the intermediate is 2^31 and comes back
// as INT32_MIN, a positive value arriving as the most negative one. That is a returned value,
// not a contract: callers on a slim tier must range-check, and the value is pinned by
// fx_review_release_error_values so a change to it cannot pass unnoticed. The negative side
// mirrors it - to<q_t>(fx_raw<pos_t>(-(1 << 19) - 1)) wraps to INT32_MAX - 4095, a negative
// value arriving as a large positive one - and is pinned too. Wrap-not-saturate RATIFIED as the
// contract by RR-16 (2026-08-25): an out-of-range conversion is a bug, and wrap's violently
// wrong value surfaces in the hash trace on the tick it happens, where a saturated value would
// drift plausibly and silently. The other assert on
// the widening path (|x.v| < 2^(63 - D)) has no defined release value at all - past it the
// multiply is signed overflow, which is UB, so a call site that can reach it is a bug on every
// tier, not a wrap.
template <typename R, typename A>
constexpr R to(A x) {
    constexpr int D = R::FRAC_BITS - A::FRAC_BITS;
    if constexpr (D == 0) {
        return fx_raw<R>(typename R::rep(x.v));                    // same point: the identity, any rep
    } else if constexpr (D > 0) {
        static_assert(D <= 62, "");
        // |x.v| * 2^D must fit i64, i.e. |x.v| < 2^(63 - D). (62 - D rejected the top bit of the
        // i64 locals: the identity on a fx<i64,F> holding INT64_MIN asserted - W1 fx review 1.)
        TL_ASSERT(i64(x.v) > -(i64(1) << (63 - D)) && i64(x.v) < (i64(1) << (63 - D)));
        const i64 w = i64(x.v) * (i64(1) << D);
        TL_ASSERT(fx_fits<R>(w));
        return fx_raw<R>(typename R::rep(w));
    } else {
        const i64 w = rne_shr(i64(x.v), -D);
        TL_ASSERT(fx_fits<R>(w));
        return fx_raw<R>(typename R::rep(w));
    }
}

// Product a * b in the result format R (named at every call site; never deduced). Exact i64
// product, one RNE shift by (A::FRAC + B::FRAC - R::FRAC). Compiles only for a listed product.
// Precondition: the result fits R (asserted in debug/dev; sat_mul is the clamping form).
template <typename R, typename A, typename B>
constexpr R mul(A a, B b) {
    static_assert(fx_op_allowed<R, A, B>::value, "product not in the mixed-op table (docs/FX-PALETTE.md section 3.1)");
    static_assert(sizeof(typename R::rep) == 4 && sizeof(typename A::rep) == 4 && sizeof(typename B::rep) == 4,
                  "rev 1 narrows to 32-bit rows only (docs/FX-PALETTE.md section 10.1)");
    constexpr int S = A::FRAC_BITS + B::FRAC_BITS - R::FRAC_BITS;
    static_assert(S >= 0 && S <= 62, "the product's point must be at or right of R's");
    const i64 q = rne_shr(i64(a.v) * i64(b.v), S);
    TL_ASSERT(fx_fits<R>(q));
    return fx_raw<R>(i32(q));
}

// mul<R> that clamps to R's range instead of asserting - quanta paths and validator-bounded data.
template <typename R, typename A, typename B>
constexpr R sat_mul(A a, B b) {
    static_assert(fx_op_allowed<R, A, B>::value, "product not in the mixed-op table (docs/FX-PALETTE.md section 3.1)");
    static_assert(sizeof(typename R::rep) == 4 && sizeof(typename A::rep) == 4 && sizeof(typename B::rep) == 4,
                  "rev 1 narrows to 32-bit rows only (docs/FX-PALETTE.md section 10.1)");
    constexpr int S = A::FRAC_BITS + B::FRAC_BITS - R::FRAC_BITS;
    static_assert(S >= 0 && S <= 62, "the product's point must be at or right of R's");
    const i64 q = rne_shr(i64(a.v) * i64(b.v), S);
    return fx_raw<R>(q > i64(INT32_MAX) ? INT32_MAX : q < i64(INT32_MIN) ? INT32_MIN : i32(q));
}

// Quotient a / b in the result format R. The exact rational is rounded once (RNE); compiles
// only for a listed quotient. Precondition b != 0 (asserted); with asserts compiled out a zero
// divisor returns sign(a) * INT32_MAX (0 for a == 0). Precondition: the result fits R (asserted).
template <typename R, typename A, typename B>
constexpr R div(A a, B b) {
    static_assert(fx_op_allowed<R, A, B>::value, "quotient not in the mixed-op table (docs/FX-PALETTE.md section 3.1)");
    static_assert(sizeof(typename R::rep) == 4 && sizeof(typename A::rep) == 4 && sizeof(typename B::rep) == 4,
                  "rev 1 narrows to 32-bit rows only (docs/FX-PALETTE.md section 10.1)");
    constexpr int S = R::FRAC_BITS + B::FRAC_BITS - A::FRAC_BITS;
    static_assert(S >= 0 && S <= 31, "the numerator must be scalable to R's point in i64");
    TL_ASSERT(b.v != 0);
    if (b.v == 0) return fx_raw<R>(a.v < 0 ? -INT32_MAX : (a.v > 0 ? INT32_MAX : 0));
    const i64 q = rne_div(i64(a.v) * (i64(1) << S), i64(b.v));
    TL_ASSERT(fx_fits<R>(q));
    return fx_raw<R>(i32(q));
}

// a * k for a plain integer k (INV_H = 480 is the use), exact product then rescaled from A's
// point to R's (exact when widening, RNE when narrowing). Listed in the op table as
// fx_op_allowed<R, A, i32>. Precondition: the result fits R (asserted).
template <typename R, typename A>
constexpr R mul_int(A a, i32 k) {
    static_assert(fx_op_allowed<R, A, i32>::value, "integer product not in the mixed-op table (docs/FX-PALETTE.md section 3.1)");
    static_assert(sizeof(typename R::rep) == 4 && sizeof(typename A::rep) == 4,
                  "rev 1 narrows to 32-bit rows only (docs/FX-PALETTE.md section 10.1)");
    constexpr int D = R::FRAC_BITS - A::FRAC_BITS;
    const i64 p = i64(a.v) * i64(k);                      // |p| <= 2^62: exact
    i64 q = 0;
    if constexpr (D == 0) {
        q = p;
    } else if constexpr (D > 0) {
        static_assert(D <= 31, "");
        TL_ASSERT(p > -(i64(1) << (63 - D)) && p < (i64(1) << (63 - D)));   // p * 2^D fits i64
        q = p * (i64(1) << D);
    } else {
        q = rne_shr(p, -D);
    }
    TL_ASSERT(fx_fits<R>(q));
    return fx_raw<R>(i32(q));
}

// Raw i64 product of two values of one 32-bit format, no rounding: point at 2 * FRAC (pos_t ->
// the fx<i64,36> local of docs/FX-PALETTE.md §3.1). Never overflows.
template <typename A>
constexpr i64 mul_wide(A a, A b) {
    static_assert(sizeof(typename A::rep) == 4, "mul_wide widens 32-bit rows only");
    return i64(a.v) * i64(b.v);
}

// --- decimal literal parsing (RR-38, 2026-08-27; docs/FX-PALETTE.md §9 R-10, §10.1) -------------------
// Integer-only decimal-to-raw quantizer: turns a base-10 literal a user or a save file typed
// ("[+-]?[0-9]*(\.[0-9]*)?", at least one digit) into a row's raw representation, RNE-rounded,
// with NO float/double token anywhere in the parse or the rounding - the exact rational the
// literal denotes is tracked as an integer numerator/denominator pair and handed to `rne_div`,
// the same primitive every other narrowing path in this header already uses. Serves BOTH of RR-38's
// named callers with this one implementation: `editor/inspector.cpp`'s fx-field edit widget
// (which only knows a runtime `FieldKind`/FRAC pair, never a compile-time row type) and
// `core/cvar.cpp`'s `CVAR_FX_RAW` decimal-literal branch (same shape - a runtime `frac_bits`).
// `fx_parse_decimal<R>` is a three-line convenience wrapper over the raw primitive for callers
// that DO have a compile-time row (this file's own tests, `fx_review`-style call sites).

namespace detail {
// Accumulates up to `count` ASCII digit bytes at `s` into `*out` (u64), left to right, MSD
// first; overflow-checked (returns false rather than wrapping). `count == 0` is a no-op success
// (a caller may legally have zero fractional digits, e.g. "5." - not itself an error).
inline bool fx_accum_digits(const char* s, u32 count, u64* out) {
    u64 acc = *out;
    for (u32 i = 0; i < count; ++i) {
        const u8 c = (u8)s[i];
        if (c < (u8)'0' || c > (u8)'9') { return false; }
        const u32 d = (u32)(c - (u8)'0');
        if (acc > (UINT64_MAX - (u64)d) / 10u) { return false; }   // would overflow u64
        acc = acc * 10u + (u64)d;
    }
    *out = acc;
    return true;
}
}  // namespace detail

constexpr ErrCode ERR_FX_PARSE = (ErrCode)0x0801;  // malformed decimal literal - empty, no digit
                                                     // anywhere, a stray byte, or trailing garbage
constexpr ErrCode ERR_FX_RANGE = (ErrCode)0x0802;   // the literal's magnitude (or its rounded raw
                                                     // result) does not fit a 32-bit row at `frac`
// The fx module's ErrCode range is 0x08xx (mem 0x01xx, jobs 0x02xx, core 0x03xx, net 0x04xx,
// render 0x05xx, bytes 0x06xx, script 0x07xx, alloy 0x0Axx - core/reflect.h's own citation list).

// Parses `s` (`StrView`, not necessarily NUL-terminated) as a base-10 decimal literal and
// RNE-quantizes it to a raw i32 at `frac` fractional bits - the runtime-FRAC primitive both of
// RR-38's callers actually need (neither has a compile-time row type at the call site). Grammar:
// an optional leading `+`/`-`, then digits, then optionally `.` and more digits - at least one
// digit somewhere in the whole string, and nothing after the last digit. `ERR_FX_PARSE`: empty
// input, no digit anywhere (a lone sign or a lone `.`), a non-digit/non-`.`/non-sign byte, or
// trailing bytes past the parse. `ERR_FX_RANGE`: more than 18 fractional digits (the fractional
// denominator `10^n` would not fit `u64`), the scaled numerator overflows `u64`, the numerator
// does not fit `rne_div`'s own precondition at `frac` bits, or the rounded result does not fit
// `i32` - every one of these is checked explicitly here so a malformed or oversized STRING can
// never reach an internal `TL_ASSERT` (that is a bug-only mechanism; untrusted text is `Result`'s
// job, `CLAUDE.md`'s error-model split). `frac` is trusted (the caller's own `FieldKind`/
// `CvarDesc::frac_bits`, not user text) and asserted in `[0, 30]`, matching every real row.
inline Result<i32> fx_parse_decimal_raw(StrView s, u8 frac) {
    TL_ASSERT(frac <= 30u);
    if (s.len == 0u) { return Result<i32>{ 0, ERR_FX_PARSE }; }

    u32 i = 0;
    bool neg = false;
    if (s.ptr[0] == '+' || s.ptr[0] == '-') { neg = (s.ptr[0] == '-'); i = 1u; }

    const u32 int_start = i;
    while (i < s.len && s.ptr[i] >= '0' && s.ptr[i] <= '9') { ++i; }
    const u32 int_len = i - int_start;

    u32 frac_start = i;
    u32 frac_len = 0u;
    if (i < s.len && s.ptr[i] == '.') {
        ++i;
        frac_start = i;
        while (i < s.len && s.ptr[i] >= '0' && s.ptr[i] <= '9') { ++i; }
        frac_len = i - frac_start;
    }

    if (i != s.len) { return Result<i32>{ 0, ERR_FX_PARSE }; }                    // trailing bytes
    if (int_len == 0u && frac_len == 0u) { return Result<i32>{ 0, ERR_FX_PARSE }; }  // no digit at all
    if (frac_len > 18u) { return Result<i32>{ 0, ERR_FX_RANGE }; }   // 10^frac_len must fit u64

    u64 int_part = 0u;
    if (!detail::fx_accum_digits(s.ptr + int_start, int_len, &int_part)) { return Result<i32>{ 0, ERR_FX_RANGE }; }
    u64 frac_part = 0u;
    if (!detail::fx_accum_digits(s.ptr + frac_start, frac_len, &frac_part)) { return Result<i32>{ 0, ERR_FX_RANGE }; }

    u64 den = 1u;
    for (u32 k = 0; k < frac_len; ++k) { den *= 10u; }   // <= 10^18, fits u64 (frac_len checked above)

    // num = int_part * den + frac_part, each step overflow-checked.
    if (int_part != 0u && den > UINT64_MAX / int_part) { return Result<i32>{ 0, ERR_FX_RANGE }; }
    const u64 scaled = int_part * den;
    if (scaled > UINT64_MAX - frac_part) { return Result<i32>{ 0, ERR_FX_RANGE }; }
    const u64 num = scaled + frac_part;

    // rne_div(num << frac, den) must not overflow i64 - the same bound fx_lit<R> asserts
    // (this header, above): |num| < 2^(63 - frac).
    if (num >= (u64(1) << (63u - (u32)frac))) { return Result<i32>{ 0, ERR_FX_RANGE }; }

    const i64 signed_num = neg ? -i64(num) : i64(num);
    const i64 q = rne_div(signed_num * (i64(1) << frac), i64(den));
    if (q < i64(INT32_MIN) || q > i64(INT32_MAX)) { return Result<i32>{ 0, ERR_FX_RANGE }; }
    return Result<i32>{ i32(q), ERR_OK };
}

// The typed convenience wrapper over `fx_parse_decimal_raw` for a caller that DOES have a
// compile-time row `R` (this file's own tests; any future call site that isn't reflection-driven).
template <typename R>
Result<R> fx_parse_decimal(StrView s) {
    static_assert(sizeof(typename R::rep) == 4, "fx_parse_decimal_raw returns i32 - 32-bit rows only");
    const Result<i32> r = fx_parse_decimal_raw(s, (u8)R::FRAC_BITS);
    if (r.err != ERR_OK) { return Result<R>{ R{}, r.err }; }
    return Result<R>{ fx_raw<R>(typename R::rep(r.value)), ERR_OK };
}

}  // namespace fx

static_assert(__is_trivially_copyable(fx::fx<i32, 18>), "rows must survive a memcpy (snapshot = memcpy)");
static_assert(sizeof(fx::fx<i32, 18>) == 4 && alignof(fx::fx<i32, 18>) == 4, "a 32-bit row is exactly its rep");
static_assert(sizeof(fx::fx<i64, 36>) == 8 && alignof(fx::fx<i64, 36>) == 8, "a 64-bit local is exactly its rep");
static_assert(fx::fx<i32, 18>::ONE == 262144 && fx::fx<i32, 18>::INT_BITS == 13, "");
static_assert(fx::rne_shr(5, 1) == 2 && fx::rne_shr(7, 1) == 4 && fx::rne_shr(-5, 1) == -2 && fx::rne_shr(-7, 1) == -4,
              "ties to even, both signs");
static_assert(fx::rne_shr(6, 2) == 2 && fx::rne_shr(-6, 2) == -2 && fx::rne_shr(10, 2) == 2 && fx::rne_shr(11, 2) == 3, "");
static_assert(fx::rne_div(7, 2) == 4 && fx::rne_div(5, 2) == 2 && fx::rne_div(-7, 2) == -4 && fx::rne_div(-5, 2) == -2 &&
              fx::rne_div(7, -2) == -4 && fx::rne_div(-3, 6) == 0 && fx::rne_div(-9, 6) == -2, "RNE on the exact rational");
static_assert(fx::sat_add(INT32_MAX, 1) == INT32_MAX && fx::sat_sub(INT32_MIN, 1) == INT32_MIN, "");
static_assert(fx::sat_add(INT64_MAX, i64(1)) == INT64_MAX && fx::sat_sub(INT64_MIN, i64(1)) == INT64_MIN, "");
static_assert(fx::sat_mul(i64(1) << 40, i64(1) << 40) == INT64_MAX && fx::sat_mul(-(i64(1) << 40), i64(1) << 40) == INT64_MIN, "");
static_assert(fx::mulhi64(i64(1) << 40, i64(1) << 40) == (i64(1) << 16) && fx::mulhi64(-1, 1) == -1 && fx::mulhi64(-1, -1) == 0, "");
static_assert(fx::wrap_add(INT32_MAX, 1) == INT32_MIN && fx::wrap_neg(INT32_MIN) == INT32_MIN, "");
