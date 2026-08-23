// fxcheck.cpp - layers 1 and 2 of the det-math oracle (docs/FX-PALETTE.md §4.4, §10.5):
//   1. exhaustive: sincos over all 2^30 turn fractions, isqrt32 over all 2^32 inputs, sqrt<pos_t>
//      and atan2 over seeded samples + every boundary, each against a double/long double
//      reference (a Q30 ulp is 9.3e-10; double's error is 1e-16 - the only place double could
//      be wrong is within 1e-7 ulp of a rounding boundary, and the worst cases are handed to
//      tools/fxcheck/oracle.py verify, which re-evaluates them at 60 digits);
//   2. differential: our sincos vs the vendored FixPointCS Fixed32::UnitSin on its native Q30
//      quarter-turn input, and our atan2 vs Fixed32::Atan2 converted to turns - proves the port.
// Output: a summary per kernel (max |err| in ulps and the argmax), and worst.tsv for the oracle.
// tools/ is exempt from the C++ subset (docs/CPP-SUBSET.md §0): STL, doubles, printf are fine.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "foundation/det_math.h"

// The panic ABI for a tools build: print and abort.
extern "C" {
void tl_fatal(const char* file, u32 line, const char* msg) { std::fprintf(stderr, "%s:%u: fatal: %s\n", file, line, msg); std::abort(); }
void tl_check_failed(const char* file, u32 line, const char* expr) { std::fprintf(stderr, "%s:%u: check failed: %s\n", file, line, expr); std::abort(); }
void tl_assert_failed(const char* file, u32 line, const char* expr) { std::fprintf(stderr, "%s:%u: assert failed: %s\n", file, line, expr); std::abort(); }
}

#define FP_ASSERT(x) ((void)0)
#define FP_CUSTOM_INVALID_ARGS
namespace FixedUtil {
void InvalidArgument(const char*, const char*, int32_t) {}
void InvalidArgument(const char*, const char*, int32_t, int32_t) {}
void InvalidArgument(const char*, const char*, int64_t) {}
void InvalidArgument(const char*, const char*, int64_t, int64_t) {}
}  // namespace FixedUtil
#include "vendor/FixPointCS/Fixed32.h"

using namespace fx;

static const double TWO_PI = 6.283185307179586476925286766559;
static const double Q30D = 1073741824.0;

struct Worst {
    double err = 0;
    i64 in0 = 0, in1 = 0;
    i64 ours = 0;
};

static void note(Worst* w, double err, i64 in0, i64 in1, i64 ours) {
    if (err > w->err) { w->err = err; w->in0 = in0; w->in1 = in1; w->ours = ours; }
}

static u64 splitmix(u64* s) {
    u64 z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// ---------------------------------------------------------------------------------------------
static int check_sincos(FILE* tsv, bool quick) {
    // Layer 1: every turn fraction. Reference: long double sin/cos of 2*pi*k/2^30 (x87 80-bit on
    // Linux, plain double on MSVC - both far below a Q30 ulp).
    Worst ws, wc, wdiff_s;
    u64 sin2cos2_worst = 0;
    u64 odd_fail = 0, half_fail = 0, exact_fail = 0;
    const u32 step = quick ? 1024u : 1u;
    for (u64 k = 0; k < (u64(1) << 30); k += step) {
        const angle_t a = fx_raw<angle_t>(i32(k));
        q_t s, c;
        sincos(a, &s, &c);
        const long double t = (long double)k / 1073741824.0L * 6.283185307179586476925286766559L;
        const long double rs = sinl(t) * 1073741824.0L, rc = cosl(t) * 1073741824.0L;
        note(&ws, (double)fabsl((long double)s.v - rs), (i64)k, 0, s.v);
        note(&wc, (double)fabsl((long double)c.v - rc), (i64)k, 0, c.v);
        // identities that need no reference
        const q_t sn = sin(-a);
        if (sn.v != -s.v) ++odd_fail;                                          // sin(-a) == -sin(a)
        const q_t sh = sin(HALF_TURN - a);
        if (sh.v != s.v) ++half_fail;                                          // sin(1/2 - a) == sin(a)
        const i64 nrm = (i64)s.v * s.v + (i64)c.v * c.v;                       // Q60
        const u64 dev = (u64)((nrm > ((i64)1 << 60) ? nrm - ((i64)1 << 60) : ((i64)1 << 60) - nrm) >> 30);
        if (dev > sin2cos2_worst) sin2cos2_worst = dev;
        // Layer 2: the reference's UnitSin on the same quarter-turn input
        const int32_t z = (int32_t)(uint32_t(k) << 2);
        const int32_t ref = Fixed32::UnitSin(z);
        note(&wdiff_s, (double)std::llabs((long long)ref - (long long)s.v), (i64)k, 0, s.v);
    }
    if (sin(fx_raw<angle_t>(0)).v != 0) ++exact_fail;
    if (sin(QUARTER_TURN).v != q_t::ONE) ++exact_fail;
    if (sin(HALF_TURN).v != 0) ++exact_fail;
    if (sin(QUARTER_TURN + HALF_TURN).v != -q_t::ONE) ++exact_fail;
    if (cos(fx_raw<angle_t>(0)).v != q_t::ONE) ++exact_fail;
    if (cos(QUARTER_TURN).v != 0) ++exact_fail;
    if (cos(HALF_TURN).v != -q_t::ONE) ++exact_fail;
    std::printf("sincos   %s: max|err| sin %.4f ulp at raw %lld, cos %.4f ulp at raw %lld; sin^2+cos^2 within %llu ulp;"
                " odd-symmetry fails %llu, half-turn-symmetry fails %llu, exact-point fails %llu;"
                " differential vs FixPointCS UnitSin: max %.0f ulp at raw %lld\n",
                quick ? "(1/1024 sample)" : "(exhaustive 2^30)",
                ws.err, (long long)ws.in0, wc.err, (long long)wc.in0, (unsigned long long)sin2cos2_worst,
                (unsigned long long)odd_fail, (unsigned long long)half_fail, (unsigned long long)exact_fail,
                wdiff_s.err, (long long)wdiff_s.in0);
    std::fprintf(tsv, "sin\t%lld\t%lld\t%.6f\n", (long long)ws.in0, (long long)ws.ours, ws.err);
    std::fprintf(tsv, "cos\t%lld\t%lld\t%.6f\n", (long long)wc.in0, (long long)wc.ours, wc.err);
    return (odd_fail || half_fail || exact_fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
static int check_isqrt(bool quick) {
    // isqrt32 exhaustive: the floor sqrt is exact iff y*y <= x < (y+1)*(y+1).
    u64 bad = 0;
    const u64 step = quick ? 4096u : 1u;
    for (u64 x = 0; x < (u64(1) << 32); x += step) {
        const u64 y = isqrt32((u32)x);
        if (!(y * y <= x && x < (y + 1) * (y + 1))) ++bad;
    }
    // and the boundaries around every perfect square up to 2^16 (full, even when quick)
    for (u64 y = 0; y <= 65535; ++y) {
        const u64 sq = y * y;
        if (isqrt32((u32)sq) != y) ++bad;
        if (sq > 0 && isqrt32((u32)(sq - 1)) != y - 1) ++bad;
        if (sq + 2 * y < (u64(1) << 32) && isqrt32((u32)(sq + 2 * y)) != y) ++bad;
    }
    // isqrt64 property: 2^24 seeded + boundaries
    u64 seed = 0x69737172743634ull;
    for (u32 i = 0; i < (1u << 24); ++i) {
        const u64 x = splitmix(&seed) >> (splitmix(&seed) & 63u);
        const u64 y = isqrt64(x);
        const bool lo = y * y <= x;
        const bool hi = (y + 1) * (y + 1) > x || (y + 1) == (u64(1) << 32);   // (2^32)^2 overflows: sqrt(2^64-1) = 2^32-1
        if (!(lo && hi)) ++bad;
    }
    if (isqrt64(~u64(0)) != 0xffffffffull) ++bad;
    if (isqrt64(0) != 0 || isqrt64(1) != 1 || isqrt64(3) != 1 || isqrt64(4) != 2) ++bad;
    // sqrt<pos_t> nearest property over seeded Q36 inputs: |y - sqrt(n)| <= 0.5 exactly (integer check)
    u64 bad_sqrt = 0;
    for (u32 i = 0; i < (1u << 24); ++i) {
        const u64 n = splitmix(&seed) >> 3;          // < 2^61: sqrt fits pos_t (y < 2^30.5)
        const pos2_wide_t w = fx_raw<pos2_wide_t>((i64)n);
        const u64 y = (u64)sqrt<pos_t>(w).v;
        // nearest: (y - 1/2)^2 <= n <= (y + 1/2)^2  <=>  4y^2 - 4y + 1 <= 4n <= 4y^2 + 4y + 1
        const unsigned __int128 n4 = (unsigned __int128)n * 4;
        const unsigned __int128 lo = (unsigned __int128)4 * y * y - 4 * y + 1;
        const unsigned __int128 hi = (unsigned __int128)4 * y * y + 4 * y + 1;
        if (y == 0 ? n4 > 1 : !(lo <= n4 && n4 <= hi)) ++bad_sqrt;
        if (y > (u64)INT32_MAX) ++bad_sqrt;
    }
    std::printf("isqrt    %s: isqrt32/isqrt64 floor violations %llu; sqrt<pos_t> nearest violations %llu (2^24 samples)\n",
                quick ? "(1/4096 sample)" : "(exhaustive 2^32)", (unsigned long long)bad, (unsigned long long)bad_sqrt);
    return (bad || bad_sqrt) ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
static double atan2_turns_ref(double y, double x) {
    double t = std::atan2(y, x) / TWO_PI;           // (-1/2, 1/2]
    return t * Q30D;
}

static int check_atan2(FILE* tsv, bool quick) {
    Worst w, wdiff;
    u64 seed = 0x6174616e32ull;
    const u32 n = quick ? (1u << 20) : (1u << 24);
    // seeded samples over the full q_t / pos_t raw range
    for (u32 i = 0; i < n; ++i) {
        const i32 y = (i32)(u32)splitmix(&seed), x = (i32)(u32)splitmix(&seed);
        if (x == 0 && y == 0) continue;
        const i32 r = atan2q(fx_raw<q_t>(y), fx_raw<q_t>(x)).v;
        const double ref = atan2_turns_ref((double)y, (double)x);
        // the branch cut: y == 0, x < 0 is +1/2 turn in both; y < 0 tiny, x < 0 -> both near -1/2
        note(&w, std::fabs((double)r - ref), y, x, r);
        // Layer 2: the reference polynomial on OUR exact ratio, in octant 0 (0 <= y <= x), Q30
        // radians -> turns. (Fixed32::Atan2 itself returns Q16 radians, whose quantum is ~2,600
        // Q30-turn ulps, so the public function cannot serve as a reference at this resolution;
        // its reciprocal-polynomial ratio and our exact division differ below that too.)
        if (y >= 0 && x > 0 && y <= x) {
            const i64 z = rne_div((i64)y * ((i64)1 << 30), (i64)x);
            const int32_t fr = FixedUtil::AtanPoly5Lut8((int32_t)z);              // Q30 radians
            const double fr_turns = (double)fr / TWO_PI;
            note(&wdiff, std::fabs((double)r - fr_turns), y, x, r);
        }
    }
    // octant boundaries and axes, every sign and magnitude class
    static const i32 mags[] = { 1, 2, 3, 7, 1000, 1 << 14, (1 << 30) - 1, 1 << 30, INT32_MAX, INT32_MIN + 1, INT32_MIN };
    u64 axis_fail = 0;
    for (i32 y : mags) {
        for (i32 x : mags) {
            for (int sy = 0; sy < 2; ++sy) {
                for (int sx = 0; sx < 2; ++sx) {
                    const i32 yy = sy ? (y == INT32_MIN ? INT32_MIN : -y) : y;
                    const i32 xx = sx ? (x == INT32_MIN ? INT32_MIN : -x) : x;
                    const i32 r = atan2q(fx_raw<q_t>(yy), fx_raw<q_t>(xx)).v;
                    note(&w, std::fabs((double)r - atan2_turns_ref((double)yy, (double)xx)), yy, xx, r);
                }
            }
        }
        // axes: exact
        if (atan2q(fx_raw<q_t>(0), fx_raw<q_t>(y)).v != (y < 0 ? HALF_TURN.v : 0)) ++axis_fail;
        if (atan2q(fx_raw<q_t>(y), fx_raw<q_t>(0)).v != (y < 0 ? -QUARTER_TURN.v : QUARTER_TURN.v)) ++axis_fail;
        // diagonals: exact 1/8 multiples
        if (y != INT32_MIN) {
            if (atan2q(fx_raw<q_t>(y), fx_raw<q_t>(y)).v != (y < 0 ? -3 * (1 << 27) : (1 << 27))) ++axis_fail;
            if (atan2q(fx_raw<q_t>(-y), fx_raw<q_t>(y)).v != (y < 0 ? 3 * (1 << 27) : -(1 << 27))) ++axis_fail;
        }
    }
    // round trip: atan2(sin a, cos a) == a for all 2^16 sampled a (plus the full sweep when not quick)
    Worst wrt;
    const u32 rstep = quick ? 1u << 14 : 1u << 6;
    for (u64 k = 0; k < (u64(1) << 30); k += rstep) {
        const angle_t a = fx_raw<angle_t>(i32(k));
        q_t s, c;
        sincos(a, &s, &c);
        const i32 r = atan2q(s, c).v;
        // compare mod one turn, mapped to (-1/2, 1/2]
        i64 d = (((i64)r - (i64)k) % (1 << 30) + (1 << 30)) % (1 << 30);
        if (d > (1 << 29)) d -= (1 << 30);
        note(&wrt, (double)std::llabs((long long)d), (i64)k, 0, r);
    }
    std::printf("atan2    (%u samples + boundaries): max|err| %.4f ulp at (y=%lld, x=%lld); axis/diagonal exactness fails %llu;"
                " atan2(sin a, cos a) vs a: max %.0f ulp at raw %lld; differential vs FixPointCS AtanPoly5Lut8 on our ratio (rad -> turns): max %.2f ulp\n",
                n, w.err, (long long)w.in0, (long long)w.in1, (unsigned long long)axis_fail, wrt.err, (long long)wrt.in0, wdiff.err);
    std::fprintf(tsv, "atan2q\t%lld\t%lld\t%lld\t%.6f\n", (long long)w.in0, (long long)w.in1, (long long)w.ours, w.err);
    return axis_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    bool quick = false;
    const char* out = "worst.tsv";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--quick")) quick = true;
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else { std::fprintf(stderr, "usage: fxcheck [--quick] [--out worst.tsv]\n"); return 2; }
    }
    FILE* tsv = std::fopen(out, "w");
    if (!tsv) { std::perror(out); return 2; }
    std::fprintf(tsv, "# kind\tinput(s)\tours\tclaimed_err_ulps - re-verify with: python tools/fxcheck/oracle.py verify %s\n", out);
    int rc = 0;
    rc |= check_sincos(tsv, quick);
    rc |= check_isqrt(quick);
    rc |= check_atan2(tsv, quick);
    std::fclose(tsv);
    std::printf("fxcheck: %s (worst cases -> %s)\n", rc ? "FAIL" : "ok", out);
    return rc;
}
