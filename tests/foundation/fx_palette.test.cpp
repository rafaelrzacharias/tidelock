// fx_palette.test.cpp - the rows, the world constants and the op table, re-checked at runtime
// (docs/FX-PALETTE.md §10.5 "fx_palette"). The static_asserts in fx_palette.h are the real
// gate; this file re-derives the numbers from docs/CANON.md independently so a header edit
// that moves a constant AND its assert together still fails here.
// Spec: docs/FX-PALETTE.md §2, §3, §3.1, §10.2; docs/CANON.md. Rubric: docs/TESTING.md §7.
#include "fx_test_util.h"

using namespace fx;

TL_TEST(fx_palette_rows_match_canon, "foundation,fx,smoke,fast") {
    // CANON "The fx palette": format per row, all 32-bit
    TL_EXPECT_EQ(pos_t::FRAC_BITS, 18);
    TL_EXPECT_EQ(vel_t::FRAC_BITS, 20);
    TL_EXPECT_EQ(invmass_t::FRAC_BITS, 18);
    TL_EXPECT_EQ(stiff_t::FRAC_BITS, 30);
    TL_EXPECT_EQ(q_t::FRAC_BITS, 30);
    TL_EXPECT_EQ(angle_t::FRAC_BITS, 30);
    TL_EXPECT_EQ(omega_t::FRAC_BITS, 20);
    TL_EXPECT_EQ(dt_t::FRAC_BITS, 30);
    TL_EXPECT_EQ(scalar_t::FRAC_BITS, 16);
    TL_EXPECT_EQ(lambda_t::FRAC_BITS, 16);
    TL_EXPECT_EQ(sizeof(pos_t), (usize)4);
    TL_EXPECT_EQ(sizeof(pos2_wide_t), (usize)8);
    TL_EXPECT_EQ(FX_PALETTE_REV, 1u);
    // ranges: +-2^INT_BITS in the row's unit
    TL_EXPECT_EQ(1 << pos_t::INT_BITS, 8192);        // +-8,192 m
    TL_EXPECT_EQ(1 << vel_t::INT_BITS, 2048);        // +-2,048 m/s
    TL_EXPECT_EQ(1 << q_t::INT_BITS, 2);             // +-2
    TL_EXPECT_EQ(1 << scalar_t::INT_BITS, 32768);    // +-32,768
    TL_EXPECT_EQ(1 << omega_t::INT_BITS, 2048);      // +-2,048 turn/s
}

TL_TEST(fx_palette_world_constants, "foundation,fx,smoke,fast") {
    // CANON "World constants", each re-derived here rather than copied from the header
    TL_EXPECT_EQ(TEXEL.v, (1 << 18) / 16);                          // 1/16 m
    TL_EXPECT_EQ(CHUNK_TEXELS, 128);
    TL_EXPECT_EQ(CHUNK_TEXELS * TEXEL.v, 8 << 18);                   // chunk = 8 m
    TL_EXPECT_EQ(CHUNK_GRID, 1024);
    TL_EXPECT_EQ((i64)CHUNK_GRID * CHUNK_TEXELS * TEXEL.v, (i64)8192 << 18);   // 8 km span
    TL_EXPECT_EQ(WORLD_HALF.v, 4096 << 18);
    TL_EXPECT_EQ(V_MAX_WORLD.v, 512 << 20);
    TL_EXPECT_EQ(TICK_HZ, 60);
    TL_EXPECT_EQ(SUBSTEPS, 8);
    TL_EXPECT_EQ(INV_H, 480);
    TL_EXPECT_EQ(MASS_RATIO_CLAMP, 4096);
    TL_EXPECT_EQ(1 << MASS_RATIO_SHIFT, MASS_RATIO_CLAMP);
    TL_EXPECT_EQ(MAX_STEPS, 5);
    // H = RNE(2^30 / 480): 2236962.133... -> 2236962; the residual is below one quantum
    TL_EXPECT_EQ(H.v, 2236962);
    TL_EXPECT_EQ(ref_rne_div((i64)1 << 30, 480), (i64)H.v);
    TL_EXPECT_TRUE((i64)H.v * 480 <= ((i64)1 << 30) && ((i64)1 << 30) - (i64)H.v * 480 < 480);
    // G_SUBSTEP = RNE(9.81 / 480 * 2^20) = RNE(10286530.56 / 480) = RNE(21430.27) = 21430
    TL_EXPECT_EQ(G_SUBSTEP.v, 21430);
    TL_EXPECT_EQ(ref_rne_div((i64)981 << 20, 48000), (i64)G_SUBSTEP.v);
    // 480 substeps of G_SUBSTEP is 9.81 m/s within the rounding of one constant
    TL_EXPECT_TRUE(fx_near_raw((i64)G_SUBSTEP.v * 480, ref_rne_div((i64)981 << 20, 100), 480));
    TL_EXPECT_EQ(TURN.v, 1 << 30);
    TL_EXPECT_EQ(QUARTER_TURN.v * 4, TURN.v);
    TL_EXPECT_EQ(HALF_TURN.v * 2, TURN.v);
}

TL_TEST(fx_palette_derivation_rule, "foundation,fx,fast") {
    // integer bits >= ceil(log2(range * margin)) per row (docs/FX-PALETTE.md §3)
    TL_EXPECT_GE((i64)1 << pos_t::INT_BITS, (i64)2 * 4096);
    TL_EXPECT_GE((i64)1 << vel_t::INT_BITS, (i64)4 * 512);
    TL_EXPECT_GE((i64)1 << invmass_t::INT_BITS, (i64)2 * MASS_RATIO_CLAMP);
    TL_EXPECT_GE((i64)1 << stiff_t::INT_BITS, (i64)2);
    TL_EXPECT_GE((i64)1 << omega_t::INT_BITS, (i64)2048);
    TL_EXPECT_GE((i64)1 << scalar_t::INT_BITS, (i64)32768);
    // and the margin is not wasted: one fewer integer bit would break the rule
    TL_EXPECT_LT((i64)1 << (pos_t::INT_BITS - 1), (i64)2 * 4096);
    TL_EXPECT_LT((i64)1 << (vel_t::INT_BITS - 1), (i64)4 * 512);
    TL_EXPECT_LT((i64)1 << (scalar_t::INT_BITS - 1), (i64)32768);
    // the XPBD denominator widened to fx<i64,30> has headroom: two clamped inv-masses + alpha~
    const i64 den_max = 2 * ((i64)MASS_RATIO_CLAMP << 30) + ((i64)2 << 30);
    TL_EXPECT_LT(den_max, (i64)1 << 62);
    // integrate step: a full-range vel_t times H lands well inside pos_t
    TL_EXPECT_EQ(mul<pos_t>(V_MAX_WORLD, H).v, (i32)ref_rne_shr((i64)V_MAX_WORLD.v * H.v, 32));
    TL_EXPECT_LT(mul<pos_t>(fx_raw<vel_t>(INT32_MAX), H).v, 5 << 18);   // 2048 m/s * 1/480 s = 4.27 m per substep
}

TL_TEST(fx_palette_op_table_shifts, "foundation,fx,fast") {
    // every listed product's shift, as the header's comments claim
    TL_EXPECT_EQ(vel_t::FRAC_BITS + dt_t::FRAC_BITS - pos_t::FRAC_BITS, 32);
    TL_EXPECT_EQ(invmass_t::FRAC_BITS + lambda_t::FRAC_BITS - pos_t::FRAC_BITS, 16);
    TL_EXPECT_EQ(omega_t::FRAC_BITS + dt_t::FRAC_BITS - angle_t::FRAC_BITS, 20);
    TL_EXPECT_EQ(q_t::FRAC_BITS + pos_t::FRAC_BITS - pos_t::FRAC_BITS, 30);
    TL_EXPECT_EQ(scalar_t::FRAC_BITS + pos_t::FRAC_BITS - pos_t::FRAC_BITS, 16);
    TL_EXPECT_EQ(pos_t::FRAC_BITS + pos_t::FRAC_BITS - pos2_wide_t::FRAC_BITS, 0);
    TL_EXPECT_EQ(vel_t::FRAC_BITS - pos_t::FRAC_BITS, 2);                 // mul_int widen
    TL_EXPECT_EQ(angle_t::FRAC_BITS - omega_t::FRAC_BITS, 10);            // mul_int narrow
    // the trait: listed true, unlisted false, order-sensitive
    TL_EXPECT_TRUE((fx_op_allowed<pos_t, vel_t, dt_t>::value));
    TL_EXPECT_TRUE((fx_op_allowed<pos2_wide_t, pos_t, pos_t>::value));
    TL_EXPECT_TRUE((fx_op_allowed<vel_t, pos_t, i32>::value));
    TL_EXPECT_FALSE((fx_op_allowed<pos_t, dt_t, vel_t>::value));
    TL_EXPECT_FALSE((fx_op_allowed<pos_t, vel_t, vel_t>::value));
    TL_EXPECT_FALSE((fx_op_allowed<pos_t, pos_t, i32>::value));
    TL_EXPECT_FALSE((fx_op_allowed<pos2_wide_t, pos2_wide_t, pos_t>::value));
    // the §3.1 identities, end to end: integrate one substep at V_MAX lands where CANON says
    // (512 m/s * 1/480 s = 1.0667 m = 279620.27 quanta -> 279620)
    TL_EXPECT_EQ(mul<pos_t>(V_MAX_WORLD, H).v, 279620);
    // v = dx * INV_H recovers the velocity within the H rounding (H is 1/480 to 1e-7)
    const pos_t dx = mul<pos_t>(V_MAX_WORLD, H);
    TL_EXPECT_TRUE(fx_near_raw(mul_int<vel_t>(dx, INV_H).v, V_MAX_WORLD.v, 2000));
    // gravity: one substep at rest falls G_SUBSTEP * H = 9.81/480^2 m = 4.26e-5 m = 11 quanta
    TL_EXPECT_EQ(mul<pos_t>(G_SUBSTEP, H).v, 11);
    // a quarter turn per second for one substep: omega x H = 1/1920 turn; H's own rounding
    // (2^30/480 is not an integer) puts the product one quantum under the exact 1/1920
    TL_EXPECT_TRUE(fx_near_raw(mul<angle_t>(fx_lit<omega_t>(1, 4), H).v, ref_rne_div((i64)1 << 30, 1920), 1));
}
