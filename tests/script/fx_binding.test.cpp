// fx_binding.test.cpp - the `fx` table: literal constructors, argument checks, and every op
// compared against the C++ helper it wraps.
// Spec: docs/LUAU-LAYER.md §10.11 rows fx_literals and fx_argument_checks; §10.3 is the
//   signature table under test. docs/FX-PALETTE.md §3/§3.1 owns the rows and the op table.
// Rubric: docs/TESTING.md §7.
//
// The comparison is the point. A binding that merely "returns a number" proves nothing: every op
// below is evaluated in Luau AND in C++ over a seeded sweep, and the two raw bit patterns must be
// equal, because the whole reason the table exists is that a script must get the SAME bits the
// solver would.
#include <stdio.h>   // snprintf - the io exemption of docs/TESTING.md §8 R-2

#include "foundation/det_math.h"
#include "foundation/fx_palette.h"
#include "foundation/rng.h"
#include "script/script.h"
#include "script_test_util.h"

namespace {

// Evaluates `expr` in `vm` and returns its integer value, or `fallback` with `ok` cleared. The
// error text is left in script_last_error for the caller's failure message.
i64 eval(ScriptVm* vm, const char* expr, bool* ok) {
    Result<i64> r = script_eval_int(vm, sv(expr));
    *ok = r.err == ERR_OK;
    return r.value;
}

// True iff `expr` FAILS in `vm` - the shape every argument-check row needs. A binding that
// accepts a bad argument and returns a wrong number is the failure this catches; a binding that
// accepts it and returns a right number is still a failure, because the contract is the check.
bool eval_raises(ScriptVm* vm, const char* expr) {
    Result<i64> r = script_eval_int(vm, sv(expr));
    return r.err != ERR_OK;
}

}  // namespace

TL_TEST(fx_literals, "script") {
    TL_SKIP_WITHOUT_COMPILER();
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));
    bool ok = false;

    // docs/LUAU-LAYER.md §10.11: fx.pos(12.5) == 12.5 x 2^18, exactly.
    TL_EXPECT_EQ(eval(f.vm, "fx.pos(12.5)", &ok), (i64)(12.5 * 262144.0));
    TL_EXPECT_TRUE(ok);
    TL_EXPECT_EQ(eval(f.vm, "fx.pos(12.5)", &ok), (i64)fx::fx_lit<pos_t>(25, 2).v);

    // Exactly representable, sign-symmetric, and the zero case.
    TL_EXPECT_EQ(eval(f.vm, "fx.pos(-0.25)", &ok), (i64)fx::fx_lit<pos_t>(-1, 4).v);
    TL_EXPECT_EQ(eval(f.vm, "fx.pos(0)", &ok), (i64)0);
    TL_EXPECT_EQ(eval(f.vm, "fx.q(1)", &ok), (i64)q_t::ONE);
    TL_EXPECT_EQ(eval(f.vm, "fx.scalar(-1)", &ok), (i64)(-scalar_t::ONE));
    TL_EXPECT_EQ(eval(f.vm, "fx.angle(0.25)", &ok), (i64)fx::QUARTER_TURN.v);
    TL_EXPECT_TRUE(ok);

    // NOT representable: 0.1 is not a dyadic rational, so no rounding is allowed to rescue it.
    TL_EXPECT_TRUE(eval_raises(f.vm, "fx.pos(0.1)"));
    TL_EXPECT_TRUE(eval_raises(f.vm, "fx.pos(1/3)"));
    // Out of range: q_t is +-2, so 2 itself is 2^31 raw and does not fit.
    TL_EXPECT_TRUE(eval_raises(f.vm, "fx.q(2)"));
    TL_EXPECT_TRUE(eval_raises(f.vm, "fx.pos(8192)"));
    // fx.raw takes any i32 and nothing wider.
    TL_EXPECT_EQ(eval(f.vm, "fx.raw(-2147483648)", &ok), (i64)INT32_MIN);
    TL_EXPECT_TRUE(ok);
    TL_EXPECT_TRUE(eval_raises(f.vm, "fx.raw(2147483648)"));
    TL_EXPECT_TRUE(eval_raises(f.vm, "fx.raw(-2147483649)"));

    // The constants a script reads are the palette's own, bit for bit.
    TL_EXPECT_EQ(eval(f.vm, "fx.H", &ok), (i64)fx::H.v);
    TL_EXPECT_EQ(eval(f.vm, "fx.INV_H", &ok), (i64)fx::INV_H);
    TL_EXPECT_EQ(eval(f.vm, "fx.G_SUBSTEP", &ok), (i64)fx::G_SUBSTEP.v);
    TL_EXPECT_EQ(eval(f.vm, "fx.TEXEL", &ok), (i64)fx::TEXEL.v);
    TL_EXPECT_EQ(eval(f.vm, "fx.V_MAX_WORLD", &ok), (i64)fx::V_MAX_WORLD.v);
    TL_EXPECT_EQ(eval(f.vm, "fx.POS", &ok), (i64)pos_t::FRAC_BITS);
    TL_EXPECT_EQ(eval(f.vm, "fx.OMEGA", &ok), (i64)omega_t::FRAC_BITS);
    TL_EXPECT_TRUE(ok);
    script_fixture_down(&f);
}

TL_TEST(fx_ops_match_cpp, "script") {
    TL_SKIP_WITHOUT_COMPILER();
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));
    char expr[192];
    u32 checked = 0;

    // A seeded sweep, generated INSIDE each helper's contract (docs/LESSONS.md: a full-range
    // property test leaves the contract and traps in dev, and then it is a trap where it is not
    // vacuous). Every row compares the Luau result with the C++ helper's raw bits.
    for (u32 i = 0; i < 256u; ++i) {
        const u64 r = rng_for(0x5c12u, i, 7u, 1u, 0u);
        const i32 a = (i32)(i64)((r & 0xffffffffu) % 2000001u) - 1000000;      // +-1e6
        const i32 b = (i32)(i64)(((r >> 32) & 0xffffffffu) % 2000001u) - 1000000;
        const i32 qv = (i32)(i64)((r >> 13) % (u64)(2u * (u32)q_t::ONE)) - q_t::ONE;  // q in +-1
        const i32 sv_ = (i32)(i64)((r >> 27) % (u64)(2u * (u32)scalar_t::ONE)) - scalar_t::ONE;

        bool ok = false;
        // fx.mul_q(q, a) is mul<A>(q_t, A) for every row A: the shift is always 30.
        (void)snprintf(expr, sizeof(expr), "fx.mul_q(%d, %d)", (int)qv, (int)a);
        TL_EXPECT_EQ(eval(f.vm, expr, &ok), (i64)fx::mul<pos_t>(fx::fx_raw<q_t>(qv), fx::fx_raw<pos_t>(a)).v);
        TL_EXPECT_TRUE(ok);

        // fx.mul_scalar(s, a) is mul<A>(scalar_t, A): the shift is always 16.
        (void)snprintf(expr, sizeof(expr), "fx.mul_scalar(%d, %d)", (int)sv_, (int)a);
        TL_EXPECT_EQ(eval(f.vm, expr, &ok), (i64)fx::mul<vel_t>(fx::fx_raw<scalar_t>(sv_), fx::fx_raw<vel_t>(a)).v);

        // fx.div_q(a, b) is div<q_t>(A, A), same row both sides.
        // div<q_t> ASSERTS when the quotient leaves q_t's +-2 range, so the oracle cannot be
        // called to discover whether it is in range (docs/LESSONS.md: generate inside the
        // contract). The raw quotient is computed here first, and only in-range pairs are swept.
        if (b != 0) {
            const i64 raw_q = fx::rne_div((i64)a * ((i64)1 << q_t::FRAC_BITS), (i64)b);
            if (raw_q >= (i64)INT32_MIN && raw_q <= (i64)INT32_MAX) {
                (void)snprintf(expr, sizeof(expr), "fx.div_q(%d, %d)", (int)a, (int)b);
                TL_EXPECT_EQ(eval(f.vm, expr, &ok),
                             (i64)fx::div<q_t>(fx::fx_raw<pos_t>(a), fx::fx_raw<pos_t>(b)).v);
            }
        }

        // fx.mul_pos_vel_dt(x, v) is the integrate step x + mul<pos_t>(v, H).
        (void)snprintf(expr, sizeof(expr), "fx.mul_pos_vel_dt(%d, %d)", (int)a, (int)b);
        TL_EXPECT_EQ(eval(f.vm, expr, &ok),
                     (i64)(fx::fx_raw<pos_t>(a) + fx::mul<pos_t>(fx::fx_raw<vel_t>(b), fx::H)).v);

        // fx.vel_from_delta(dx) is mul_int<vel_t>(pos_t, INV_H) - exact, a two-bit widening.
        const i32 small = a / 512;                       // inside the |dx| * 1920 < 2^31 contract
        (void)snprintf(expr, sizeof(expr), "fx.vel_from_delta(%d)", (int)small);
        TL_EXPECT_EQ(eval(f.vm, expr, &ok),
                     (i64)fx::mul_int<vel_t>(fx::fx_raw<pos_t>(small), fx::INV_H).v);

        // fx.lerp(a, b, t) is lerp<A>(A, A, q_t), one RNE.
        (void)snprintf(expr, sizeof(expr), "fx.lerp(%d, %d, %d)", (int)a, (int)b, (int)qv);
        TL_EXPECT_EQ(eval(f.vm, expr, &ok),
                     (i64)fx::lerp<pos_t>(fx::fx_raw<pos_t>(a), fx::fx_raw<pos_t>(b), fx::fx_raw<q_t>(qv)).v);

        // fx.dist / fx.normalize: the pos x pos path, inside the broadphase-sized contract.
        const i32 dx = a / 64, dy = b / 64;
        (void)snprintf(expr, sizeof(expr), "fx.dist(0, 0, %d, %d)", (int)dx, (int)dy);
        const fx::vec2<pos_t> d = { fx::fx_raw<pos_t>(dx), fx::fx_raw<pos_t>(dy) };
        TL_EXPECT_EQ(eval(f.vm, expr, &ok), (i64)fx::len(d).v);
        if (dx != 0 || dy != 0) {
            const fx::vec2<q_t> u = fx::normalize(d);
            (void)snprintf(expr, sizeof(expr), "select(1, fx.normalize(%d, %d))", (int)dx, (int)dy);
            TL_EXPECT_EQ(eval(f.vm, expr, &ok), (i64)u.x.v);
            (void)snprintf(expr, sizeof(expr), "select(2, fx.normalize(%d, %d))", (int)dx, (int)dy);
            TL_EXPECT_EQ(eval(f.vm, expr, &ok), (i64)u.y.v);
        }

        // fx.sincos / fx.atan2 / fx.atan2_q against det_math, bit for bit (never "close enough":
        // the kernels are pure integer, so the binding must reproduce them exactly).
        const i32 ang = (i32)(u32)(r >> 7);
        q_t s = fx::fx_raw<q_t>(0), c = fx::fx_raw<q_t>(0);
        fx::sincos(fx::fx_raw<angle_t>(ang), &s, &c);
        (void)snprintf(expr, sizeof(expr), "select(1, fx.sincos(%d))", (int)ang);
        TL_EXPECT_EQ(eval(f.vm, expr, &ok), (i64)s.v);
        (void)snprintf(expr, sizeof(expr), "select(2, fx.sincos(%d))", (int)ang);
        TL_EXPECT_EQ(eval(f.vm, expr, &ok), (i64)c.v);
        if (dx != 0 || dy != 0) {
            (void)snprintf(expr, sizeof(expr), "fx.atan2(%d, %d)", (int)dy, (int)dx);
            TL_EXPECT_EQ(eval(f.vm, expr, &ok),
                         (i64)fx::atan2(fx::fx_raw<pos_t>(dy), fx::fx_raw<pos_t>(dx)).v);
        }

        // The saturating quanta ops, over the full i32 range - saturation is the point.
        const i32 big_a = (i32)(u32)(r >> 3), big_b = (i32)(u32)(r >> 19);
        (void)snprintf(expr, sizeof(expr), "fx.sat_add(%d, %d)", (int)big_a, (int)big_b);
        TL_EXPECT_EQ(eval(f.vm, expr, &ok), (i64)fx::sat_add(big_a, big_b));
        (void)snprintf(expr, sizeof(expr), "fx.sat_sub(%d, %d)", (int)big_a, (int)big_b);
        TL_EXPECT_EQ(eval(f.vm, expr, &ok), (i64)fx::sat_sub(big_a, big_b));
        checked += 1u;
    }
    // Per docs/LESSONS.md: count what was actually swept, or a row that generated nothing passes.
    TL_EXPECT_EQ(checked, 256u);
    script_fixture_down(&f);
}

TL_TEST(fx_argument_checks, "script") {
    TL_SKIP_WITHOUT_COMPILER();
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));

    // docs/LUAU-LAYER.md §10.3: every binding that receives an integer checks x == floor(x) and
    // range, and fails loud. One row per rejection class, over the whole table.
    static const char* const BAD[] = {
        "fx.raw(1.5)", "fx.raw('1')", "fx.raw(nil)", "fx.raw({})", "fx.raw(true)",
        "fx.mul_q(1.5, 2)", "fx.mul_q(2, 1.5)", "fx.mul_q('a', 2)", "fx.mul_q(2)",
        "fx.mul_scalar(1.5, 2)", "fx.mul_scalar(2, nil)",
        "fx.div_q(1.5, 2)", "fx.div_q(2, 0)",
        "fx.mul_pos_vel_dt(1.5, 2)", "fx.mul_pos_vel_dt(2, 1.5)",
        "fx.vel_from_delta(1.5)", "fx.vel_from_delta(2147483647)",
        "fx.dist(0, 0, 1.5, 0)", "fx.dist(0, 0, 0)",
        "fx.normalize(1.5, 0)", "fx.sincos(1.5)", "fx.atan2(1.5, 0)", "fx.atan2(0, 0)",
        "fx.atan2_q(0, 0)", "fx.lerp(1.5, 0, 0)", "fx.lerp(0, 0, 1.5)",
        "fx.sat_add(1.5, 0)", "fx.sat_sub(0, 1.5)",
        "fx.imin(1.5, 0)", "fx.imax(0, 1.5)", "fx.iabs(1.5)", "fx.iclamp(0, 3, 1)",
        "fx.str(1.5, 18)", "fx.str(0, 31)", "fx.str(0, -1)",
        "fx.pos('1')", "fx.pos(nil)", "fx.pos(0/0)",
        nullptr,
    };
    u32 rejected = 0;
    for (const char* const* p = BAD; *p != nullptr; ++p) {
        const bool raised = eval_raises(f.vm, *p);
        TL_EXPECT_TRUE(raised);
        if (raised) rejected += 1u;
    }
    TL_EXPECT_EQ(rejected, 38u);          // the row count, so a shortened list cannot pass quietly

    bool ok = false;
    // The integer helpers `math` used to provide, and the decimal formatter.
    TL_EXPECT_EQ(eval(f.vm, "fx.imin(-3, 7)", &ok), (i64)-3);
    TL_EXPECT_EQ(eval(f.vm, "fx.imax(-3, 7)", &ok), (i64)7);
    TL_EXPECT_EQ(eval(f.vm, "fx.iabs(-3)", &ok), (i64)3);
    TL_EXPECT_EQ(eval(f.vm, "fx.iclamp(9, -3, 7)", &ok), (i64)7);
    TL_EXPECT_EQ(eval(f.vm, "fx.iclamp(-9, -3, 7)", &ok), (i64)-3);
    TL_EXPECT_TRUE(ok);
    // fx.str is exact for a dyadic value and truncating past nine places, as its contract says.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "assert(fx.str(fx.pos(12.5), fx.POS) == '12.500000000', fx.str(fx.pos(12.5), fx.POS))\n"
        "assert(fx.str(fx.pos(-0.25), fx.POS) == '-0.250000000', fx.str(fx.pos(-0.25), fx.POS))\n"
        "assert(fx.str(1, fx.POS) == '0.000003814', fx.str(1, fx.POS))\n"));

    // fx.rng_below / fx.rng_q exist and refuse outside a running system, which is where every
    // call is until the W3 trampoline publishes one (docs/LUAU-LAYER.md §10.3).
    TL_EXPECT_TRUE(eval_raises(f.vm, "fx.rng_below(1, 0, 10)"));
    TL_EXPECT_TRUE(eval_raises(f.vm, "fx.rng_q(1, 0)"));
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local ok, err = pcall(fx.rng_below, 1, 0, 10)\n"
        "assert(not ok and string.find(err, 'no system is running') ~= nil, tostring(err))\n"));

    // fx.to_f64 is absent from the sim VM by design: a scaled double must not exist there.
    TL_EXPECT_TRUE(script_ok(f.vm, "assert(fx.to_f64 == nil)"));
    script_fixture_down(&f);
}
