// fx_trace.test.cpp - the cross-ISA trace (docs/FX-PALETTE.md §10.5 "fx_crossisa"): a 1M-op
// trace of mul/div/mul_int/to/sqrt/sincos/atan2/normalize/rotate over seeded inputs, folded
// into one 64-bit hash that is PINNED here. The same binary logic must produce the same hash on
// clang-cl/x86-64, clang/linux x86-64 (CI) and clang/aarch64 (the Pi, nightly) - a differing
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
