// fx_trace.test.cpp - the cross-ISA trace (docs/FX-PALETTE.md §10.5 "fx_crossisa"): a 1M-op
// trace of mul/div/mul_int/to/sqrt/sincos/atan2/normalize/rotate over seeded inputs, folded
// into one 64-bit hash that is PINNED here. The same binary logic must produce the same hash on
// clang-cl/x86-64, clang/linux x86-64 (CI) and clang/aarch64 (the CI arm64 legs) - a differing
// value is a determinism bug, UB by default hypothesis (docs/TESTING.md §4). The driver job of
// the spec is the same trace hashed through the driver once the runner+driver lane lands; until
// then this test IS the trace, and the pinned constant is the PC's answer.
// Spec: docs/FX-PALETTE.md §10.5; docs/DETERMINISM.md §6. Rubric: docs/TESTING.md §7 (byte-stability).
#include "fx_test_util.h"
#include "foundation/det_math.h"

using namespace fx;

static const u64 FX_TRACE_PINNED = 0x1a1803512f224fadull;   // clang-cl x86-64 dev, 2026-08-23

// FNV-1a 64 over raw values (docs/CANON.md: the NameHash constants), no hash lane needed.
static u64 fold(u64 h, u64 v) {
    for (u32 i = 0; i < 8; ++i) {
        h ^= (v >> (8 * i)) & 0xffu;
        h *= 0x100000001b3ull;
    }
    return h;
}

static u64 run_trace(u64 seed, u32 ops) {
    FxRng rng = { seed };
    u64 h = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < ops; ++i) {
        const i32 a = fx_rng_i32(&rng), b = fx_rng_i32(&rng), c = fx_rng_i32(&rng);
        // every op below is in-contract for any a, b, c: results fit by construction
        const pos_t p = mul<pos_t>(fx_raw<vel_t>(a), H);                          // |a| * H fits
        const angle_t th = mul<angle_t>(fx_raw<omega_t>(b >> 2), H);                  // 512 turn/s * H = 1.07 turn < 2
        const q_t qq = mul<q_t>(fx_raw<q_t>(a >> 1), fx_raw<q_t>(b >> 1));          // |q| < 1 each
        const pos_t dl = mul<pos_t>(fx_raw<invmass_t>(a >> 2), fx_raw<lambda_t>(b >> 14));   // 2^29 * 2^17 >> 16 = 2^30
        const vel_t v = mul_int<vel_t>(fx_raw<pos_t>(c >> 11), INV_H);              // |c|/2^11 * 480 * 4 < 2^31
        const scalar_t sc = to<scalar_t>(fx_raw<invmass_t>(a));
        const pos_t sq = sqrt<pos_t>(fx_raw<pos2_wide_t>(mul_wide(fx_raw<pos_t>(a >> 1), fx_raw<pos_t>(a >> 1)) + mul_wide(fx_raw<pos_t>(b >> 1), fx_raw<pos_t>(b >> 1))));
        q_t s, co;
        sincos(fx_raw<angle_t>(c), &s, &co);
        const angle_t at = atan2q(fx_raw<q_t>(a), fx_raw<q_t>(b));
        const i32 den = b >> 8;
        const i32 num = (i32)(((i64)den * (i64)(a >> 1)) >> 31);                     // |num| < |den| / 2
        const q_t dv = den == 0 ? fx_raw<q_t>(0) : div<q_t>(fx_raw<pos_t>(num), fx_raw<pos_t>(den));
        vec2<pos_t> d = { fx_raw<pos_t>(a >> 3), fx_raw<pos_t>(b >> 3) };
        if (d.x.v == 0 && d.y.v == 0) d.x = fx_raw<pos_t>(1);
        const vec2<q_t> n = normalize(d);
        const vec2<pos_t> r = rotate(d, s, co);
        h = fold(h, (u64)(u32)p.v);
        h = fold(h, (u64)(u32)th.v);
        h = fold(h, (u64)(u32)qq.v);
        h = fold(h, (u64)(u32)dl.v);
        h = fold(h, (u64)(u32)v.v);
        h = fold(h, (u64)(u32)sc.v);
        h = fold(h, (u64)(u32)sq.v);
        h = fold(h, (u64)(u32)s.v);
        h = fold(h, (u64)(u32)co.v);
        h = fold(h, (u64)(u32)at.v);
        h = fold(h, (u64)(u32)dv.v);
        h = fold(h, (u64)(u32)n.x.v);
        h = fold(h, (u64)(u32)n.y.v);
        h = fold(h, (u64)(u32)r.x.v);
        h = fold(h, (u64)(u32)r.y.v);
    }
    return h;
}

// The first trace (W1 fx) covers mul/div/mul_int(widen)/to(narrow)/sqrt(S=0)/sincos/atan2q/
// normalize/rotate and nothing else: no to<R> widening, no sqrt at S = 18 or 30, no rsqrt, no
// lerp, no dot/cross/len, no mul_int narrowing, no sat/abs/min/max/clamp tier, no i64 helpers,
// and div only at |q| < 1/2. This second trace (W1 fx review 3) covers the rest, each on every
// sign class and on the magnitude classes its contract admits; it has its own pin so the first
// pin never moves.
static u64 run_trace_b(u64 seed, u32 ops) {
    FxRng rng = { seed };
    u64 h = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < ops; ++i) {
        const i32 a = fx_rng_i32(&rng), b = fx_rng_i32(&rng), c = fx_rng_i32(&rng);
        const i64 w = fx_rng_i64(&rng);
        // to<R>: widen pos -> q (|a| < 2^19), pos -> pos2_wide (exact), scalar -> q (|a| < 2^17)
        const q_t wq = to<q_t>(fx_raw<pos_t>(a >> 13));
        const pos2_wide_t ww = to<pos2_wide_t>(fx_raw<pos_t>(a));
        const q_t ws = to<q_t>(fx_raw<scalar_t>(b >> 15));
        // sqrt at S = 18 (pos -> pos) and S = 30 (q -> q), rsqrt<q_t> on [1/4, 2)
        const pos_t s18 = sqrt<pos_t>(fx_raw<pos_t>(a & INT32_MAX));
        const q_t s30 = sqrt<q_t>(fx_raw<q_t>(b & INT32_MAX));
        const q_t rs = rsqrt<q_t>(fx_raw<q_t>((c & 0x3fffffff) | 0x10000000));
        // mul_int narrowing: angle x INV_H -> omega (|a| < 2^21 keeps 480 * a / 2^10 in range)
        const omega_t om = mul_int<omega_t>(fx_raw<angle_t>(a >> 10), INV_H);
        // div with quotients up to the row's range: pos/pos -> pos (S = 18) and q/q -> q
        const i32 dd = (b >> 4) == 0 ? 1 : (b >> 4);
        const pos_t dp = div<pos_t>(fx_raw<pos_t>((i32)(((i64)dd * (i64)(a >> 1)) >> 31)), fx_raw<pos_t>(dd));   // |q| < 1/2 m... scaled: exact rational at S = 18
        const i32 dm = ((c & INT32_MAX) >> 1) | 0x20000000;                                                           // |den| in [1/2, 1)
        const q_t dq = div<q_t>(fx_raw<q_t>(a >> 2), fx_raw<q_t>(c < 0 ? -dm : dm));                                   // |num| < 1/2: |q| < 1
        // sat_mul<R> through the clamp, sat tier, abs/min/max/clamp/sign
        const q_t sm = sat_mul<q_t>(fx_raw<q_t>(a), fx_raw<q_t>(b));
        const pos_t sa = sat_add(fx_raw<pos_t>(a), fx_raw<pos_t>(b));
        const pos_t ss = sat_sub(fx_raw<pos_t>(a), fx_raw<pos_t>(b));
        const pos_t sn = sat_neg(fx_raw<pos_t>(a));
        const pos_t ab = abs(fx_raw<pos_t>(a));
        const pos_t mn = min(fx_raw<pos_t>(a), fx_raw<pos_t>(b));
        const pos_t mx = max(fx_raw<pos_t>(a), fx_raw<pos_t>(b));
        const pos_t cl = clamp(fx_raw<pos_t>(c), mn, mx);
        const i32 sg = sign(fx_raw<pos_t>(a)) + 2 * sign(fx_raw<pos2_wide_t>(w));
        const i32 fl = fx_to_int_floor(fx_raw<pos_t>(a));
        // lerp, dot/cross (pos x q -> pos; pos x pos -> wide), len, atan2(pos_t)
        const pos_t lp = lerp<pos_t>(fx_raw<pos_t>(a >> 1), fx_raw<pos_t>(b >> 1), fx_raw<q_t>(c >> 1));
        const vec2<pos_t> p = { fx_raw<pos_t>(a >> 2), fx_raw<pos_t>(b >> 2) };
        const vec2<q_t> n = { fx_raw<q_t>(c >> 1), fx_raw<q_t>((a ^ b) >> 1) };
        const pos_t dt = dot<pos_t>(p, n);
        const pos_t cr = cross<pos_t>(p, n);
        const pos2_wide_t d2 = dot<pos2_wide_t>(p, p);
        const pos_t ln = len(p);
        const angle_t at = atan2(p.y, p.x.v == 0 && p.y.v == 0 ? fx_raw<pos_t>(1) : p.x);
        // the i64 quanta helpers and the integer roots
        const i64 mh = mulhi64(w, (i64)a * (i64)b);
        const i64 s64 = sat_mul(w, (i64)c);
        const i64 sa64 = sat_add(w, (i64)a << 32);
        const u32 r32 = isqrt32((u32)a);
        const u64 r64 = isqrt64((u64)w);
        h = fold(h, (u64)(u32)wq.v);  h = fold(h, (u64)ww.v);        h = fold(h, (u64)(u32)ws.v);
        h = fold(h, (u64)(u32)s18.v); h = fold(h, (u64)(u32)s30.v);  h = fold(h, (u64)(u32)rs.v);
        h = fold(h, (u64)(u32)om.v);  h = fold(h, (u64)(u32)dp.v);   h = fold(h, (u64)(u32)dq.v);
        h = fold(h, (u64)(u32)sm.v);  h = fold(h, (u64)(u32)sa.v);   h = fold(h, (u64)(u32)ss.v);
        h = fold(h, (u64)(u32)sn.v);  h = fold(h, (u64)(u32)ab.v);   h = fold(h, (u64)(u32)mn.v);
        h = fold(h, (u64)(u32)mx.v);  h = fold(h, (u64)(u32)cl.v);   h = fold(h, (u64)(u32)sg);
        h = fold(h, (u64)(u32)fl);    h = fold(h, (u64)(u32)lp.v);   h = fold(h, (u64)(u32)dt.v);
        h = fold(h, (u64)(u32)cr.v);  h = fold(h, (u64)d2.v);        h = fold(h, (u64)(u32)ln.v);
        h = fold(h, (u64)(u32)at.v);  h = fold(h, (u64)mh);          h = fold(h, (u64)s64);
        h = fold(h, (u64)sa64);       h = fold(h, (u64)r32);         h = fold(h, r64);
    }
    return h;
}

static const u64 FX_TRACE_B_PINNED = 0x14179b6d064d0ca6ull;   // W1 fx review 3: clang-cl x86-64 dev, 2026-08-23

TL_TEST(fx_trace_b_hash_pinned, "foundation,fx,det,crossisa,fast") {
    const u64 h = run_trace_b(0x7469646c6f636b31ull, 1u << 15);   // 2^15 x 30 values ~ 1M
    TL_EXPECT_EQ(h, FX_TRACE_B_PINNED);
}

TL_TEST(fx_trace_hash_pinned, "foundation,fx,det,crossisa,fast") {
    // 2^16 iterations x 15 results ~ 1M folded values. The constant below was produced on the
    // reference PC (clang-cl, x86-64, dev tier) and must be reproduced by every tier, compiler
    // and ISA - re-pin ONLY with a recorded reason (a kernel change), never to make a lane green.
    const u64 h = run_trace(0x7469646c6f636b31ull /* "tidelock1", docs/CANON.md TL_HASH_SEED */, 1u << 16);
    TL_EXPECT_EQ(h, FX_TRACE_PINNED);
    // and it is a function of the seed, not of anything else
    TL_EXPECT_NE(run_trace(1, 1u << 10), run_trace(2, 1u << 10));
    TL_EXPECT_EQ(run_trace(3, 1u << 10), run_trace(3, 1u << 10));
}
