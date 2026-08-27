// cvar.test.cpp - registration/lookup, typed get/set, READONLY/SIM refusal, the sim-applier
// bypass seam, the SIM fingerprint fold order, and the pure format/parse round trip.
// Spec: docs/TOOLING.md §3, §9.1, §9.2, §9.3.5. Rubric: docs/TESTING.md §7.
//
// Local variable naming: TL_TEST's generated function signature is `(TestCtx* t)` (tests/
// runner/tl_test.h), so every local CvarTable here is named `tab`, never `t` - a `CvarTable t;`
// would redeclare the injected parameter.
#include "runner/tl_test.h"
#include "core/cvar.h"

#include <string.h>

namespace {
TL_CVAR(i32, cv_i, -5, 0, "an int cvar");
TL_CVAR(u32, cv_u, 7u, 0, "a uint cvar");
TL_CVAR(f32, cv_f, 1.5f, 0, "a float cvar");
TL_CVAR(bool, cv_b, true, 0, "a bool cvar");
TL_CVAR(u32, cv_ro, 42u, CVAR_READONLY, "a readonly cvar");
TL_CVAR(bool, cv_sim, false, CVAR_SIM, "a sim-fingerprinted cvar");
TL_CVAR(bool, cv_sim2, true, CVAR_SIM, "another sim cvar");

void build(CvarTable* tab) {
    cvar_table_init(tab);
    cvar_register(tab, &CVAR_cv_i);
    cvar_register(tab, &CVAR_cv_u);
    cvar_register(tab, &CVAR_cv_f);
    cvar_register(tab, &CVAR_cv_b);
    cvar_register(tab, &CVAR_cv_ro);
    cvar_register(tab, &CVAR_cv_sim);
    cvar_register(tab, &CVAR_cv_sim2);
}
}  // namespace

TL_TEST(cvar_register_defaults_and_find, "core,editor,cvar,fast") {
    CvarTable tab;
    build(&tab);
    TL_ASSERT_EQ(tab.count, 7u);
    TL_EXPECT_EQ(cvar_get_i32(&tab, "cv_i"_id), (i32)-5);
    TL_EXPECT_EQ(cvar_get_u32(&tab, "cv_u"_id), 7u);
    TL_EXPECT_EQ(cvar_get_f32(&tab, "cv_f"_id), 1.5f);
    TL_EXPECT_EQ(cvar_get_bool(&tab, "cv_b"_id), true);
    TL_EXPECT_NULL(cvar_find(&tab, "nope"_id));
    TL_ASSERT_NOT_NULL(cvar_find(&tab, "cv_i"_id));
    TL_EXPECT_EQ(cvar_find(&tab, "cv_i"_id)->kind, (u8)CVAR_I32);
}

TL_TEST(cvar_sorted_index_is_key_ascending, "core,editor,cvar,fast") {
    CvarTable tab;
    build(&tab);
    // The sorted index must actually be sorted by key - binary search correctness depends on it.
    for (u32 i = 1; i < tab.count; ++i) {
        TL_EXPECT_LT(tab.desc[tab.sorted[i - 1]]->key, tab.desc[tab.sorted[i]]->key);
    }
}

TL_TEST(cvar_set_ordinary_path_updates_value, "core,editor,cvar,fast") {
    CvarTable tab;
    build(&tab);
    TL_ASSERT_EQ(cvar_set_i32(&tab, "cv_i"_id, 99), ERR_OK);
    TL_EXPECT_EQ(cvar_get_i32(&tab, "cv_i"_id), (i32)99);
    TL_ASSERT_EQ(cvar_set_u32(&tab, "cv_u"_id, 123u), ERR_OK);
    TL_EXPECT_EQ(cvar_get_u32(&tab, "cv_u"_id), 123u);
    TL_ASSERT_EQ(cvar_set_f32(&tab, "cv_f"_id, -2.25f), ERR_OK);
    TL_EXPECT_EQ(cvar_get_f32(&tab, "cv_f"_id), -2.25f);
    TL_ASSERT_EQ(cvar_set_bool(&tab, "cv_b"_id, false), ERR_OK);
    TL_EXPECT_EQ(cvar_get_bool(&tab, "cv_b"_id), false);
}

TL_TEST(cvar_set_unknown_key_is_not_found, "core,editor,cvar,fast") {
    CvarTable tab;
    build(&tab);
    TL_EXPECT_EQ(cvar_set_raw(&tab, "nope"_id, 0u), ERR_CVAR_NOT_FOUND);
}

TL_TEST(cvar_readonly_refuses_write, "core,editor,cvar,fast") {
    CvarTable tab;
    build(&tab);
    TL_EXPECT_EQ(cvar_get_u32(&tab, "cv_ro"_id), 42u);
    TL_EXPECT_EQ(cvar_set_raw(&tab, "cv_ro"_id, 1u), ERR_CVAR_READONLY);
    TL_EXPECT_EQ(cvar_get_u32(&tab, "cv_ro"_id), 42u);   // unchanged
    // Even the sim-applier bypass still refuses READONLY (this header's contract block).
    TL_EXPECT_EQ(cvar_apply_sim_raw(&tab, "cv_ro"_id, 1u), ERR_CVAR_READONLY);
}

TL_TEST(cvar_sim_flag_refuses_ordinary_write_but_not_the_applier_seam, "core,editor,cvar,fast") {
    CvarTable tab;
    build(&tab);
    TL_EXPECT_EQ(cvar_get_bool(&tab, "cv_sim"_id), false);
    TL_EXPECT_EQ(cvar_set_raw(&tab, "cv_sim"_id, 1u), ERR_CVAR_SIM_UNROUTED);
    TL_EXPECT_EQ(cvar_get_bool(&tab, "cv_sim"_id), false);   // unchanged
    TL_EXPECT_EQ(cvar_set_bool(&tab, "cv_sim"_id, true), ERR_CVAR_SIM_UNROUTED);
    // The seam a future CMD_SET_CVAR applier calls (cvar.h's contract block) bypasses it.
    TL_ASSERT_EQ(cvar_apply_sim_raw(&tab, "cv_sim"_id, 1u), ERR_OK);
    TL_EXPECT_EQ(cvar_get_bool(&tab, "cv_sim"_id), true);
}

TL_TEST(cvar_sim_fold_covers_only_sim_flagged_in_sorted_order, "core,editor,cvar,fast") {
    CvarTable tab;
    build(&tab);
    const u64 h0 = cvar_sim_fold_bits(&tab, TL_HASH_SEED);
    // Changing a non-SIM cvar must not move the fold.
    TL_ASSERT_EQ(cvar_set_i32(&tab, "cv_i"_id, 12345), ERR_OK);
    TL_EXPECT_EQ(cvar_sim_fold_bits(&tab, TL_HASH_SEED), h0);
    // Changing a SIM cvar (through the applier seam, since the ordinary path refuses it) must.
    TL_ASSERT_EQ(cvar_apply_sim_raw(&tab, "cv_sim"_id, 1u), ERR_OK);
    TL_EXPECT_NE(cvar_sim_fold_bits(&tab, TL_HASH_SEED), h0);
    // Deterministic: folding twice from the same state gives the same hash.
    const u64 h1 = cvar_sim_fold_bits(&tab, TL_HASH_SEED);
    const u64 h2 = cvar_sim_fold_bits(&tab, TL_HASH_SEED);
    TL_EXPECT_EQ(h1, h2);
}

TL_TEST(cvar_kind_is_registered_correctly, "core,editor,cvar,fast") {
    // Every typed getter in this file TL_CHECKs its cvar's registered kind (cvar.h's contract);
    // this asserts the registration itself is what a call site relies on.
    CvarTable tab;
    build(&tab);
    TL_EXPECT_EQ(cvar_find(&tab, "cv_f"_id)->kind, (u8)CVAR_F32);
    TL_EXPECT_NE(cvar_find(&tab, "cv_f"_id)->kind, (u8)CVAR_I32);
}

TL_TEST(cvar_format_per_kind, "core,editor,cvar,fast") {
    CvarTable tab;
    build(&tab);
    char buf[32];
    TL_ASSERT_TRUE(cvar_format(&tab, "cv_i"_id, buf, sizeof(buf)) > 0);
    TL_EXPECT_EQ(strcmp(buf, "-5"), 0);
    TL_ASSERT_TRUE(cvar_format(&tab, "cv_u"_id, buf, sizeof(buf)) > 0);
    TL_EXPECT_EQ(strcmp(buf, "7"), 0);
    TL_ASSERT_TRUE(cvar_format(&tab, "cv_b"_id, buf, sizeof(buf)) > 0);
    TL_EXPECT_EQ(strcmp(buf, "1"), 0);
    TL_ASSERT_TRUE(cvar_format(&tab, "cv_f"_id, buf, sizeof(buf)) > 0);
    TL_EXPECT_EQ(strcmp(buf, "1.5"), 0);
    TL_ASSERT_EQ(cvar_set_f32(&tab, "cv_f"_id, -3.0f), ERR_OK);
    TL_ASSERT_TRUE(cvar_format(&tab, "cv_f"_id, buf, sizeof(buf)) > 0);
    TL_EXPECT_EQ(strcmp(buf, "-3"), 0);
    TL_EXPECT_EQ(cvar_format(&tab, "nope"_id, buf, sizeof(buf)), 0u);   // unregistered
}

TL_TEST(cvar_format_truncation_reports_zero, "core,editor,cvar,fast") {
    CvarTable tab;
    build(&tab);
    char tiny[2];
    // "-5" + NUL needs 3 bytes; a 2-byte buffer must report 0, never a partial/unterminated write.
    TL_EXPECT_EQ(cvar_format(&tab, "cv_i"_id, tiny, sizeof(tiny)), 0u);
}

TL_TEST(cvar_parse_and_set_per_kind, "core,editor,cvar,fast") {
    CvarTable tab;
    build(&tab);
    TL_ASSERT_EQ(cvar_parse_and_set(&tab, "cv_i"_id, "-17"), ERR_OK);
    TL_EXPECT_EQ(cvar_get_i32(&tab, "cv_i"_id), (i32)-17);
    TL_ASSERT_EQ(cvar_parse_and_set(&tab, "cv_u"_id, "999"), ERR_OK);
    TL_EXPECT_EQ(cvar_get_u32(&tab, "cv_u"_id), 999u);
    TL_ASSERT_EQ(cvar_parse_and_set(&tab, "cv_b"_id, "false"), ERR_OK);
    TL_EXPECT_EQ(cvar_get_bool(&tab, "cv_b"_id), false);
    TL_ASSERT_EQ(cvar_parse_and_set(&tab, "cv_b"_id, "1"), ERR_OK);
    TL_EXPECT_EQ(cvar_get_bool(&tab, "cv_b"_id), true);
    TL_ASSERT_EQ(cvar_parse_and_set(&tab, "cv_f"_id, "-2.25"), ERR_OK);
    TL_EXPECT_EQ(cvar_get_f32(&tab, "cv_f"_id), -2.25f);
}

TL_TEST(cvar_parse_and_set_rejects_malformed, "core,editor,cvar,fast") {
    CvarTable tab;
    build(&tab);
    // The class of malformed input docs/LESSONS.md calls out for a strict numeric CLI parser.
    TL_EXPECT_EQ(cvar_parse_and_set(&tab, "cv_i"_id, ""), ERR_CVAR_PARSE);
    TL_EXPECT_EQ(cvar_parse_and_set(&tab, "cv_i"_id, "12x"), ERR_CVAR_PARSE);
    TL_EXPECT_EQ(cvar_parse_and_set(&tab, "cv_i"_id, "abc"), ERR_CVAR_PARSE);
    TL_EXPECT_EQ(cvar_parse_and_set(&tab, "cv_i"_id, "-"), ERR_CVAR_PARSE);
    TL_EXPECT_EQ(cvar_parse_and_set(&tab, "cv_u"_id, "-1"), ERR_CVAR_PARSE);   // out of u32 range
    TL_EXPECT_EQ(cvar_parse_and_set(&tab, "cv_b"_id, "yes"), ERR_CVAR_PARSE);
    TL_EXPECT_EQ(cvar_parse_and_set(&tab, "cv_f"_id, "1e3"), ERR_CVAR_PARSE);   // no exponent form
    TL_EXPECT_EQ(cvar_parse_and_set(&tab, "cv_f"_id, "1.2.3"), ERR_CVAR_PARSE);
    // A malformed value on a READONLY cvar still refuses as READONLY first (checked before parse
    // even matters, since cvar_set_raw runs after a successful parse) - a malformed value never
    // reaches a write either way, so assert the value truly never changed.
    TL_EXPECT_EQ(cvar_parse_and_set(&tab, "cv_ro"_id, "1"), ERR_CVAR_READONLY);
    TL_EXPECT_EQ(cvar_get_u32(&tab, "cv_ro"_id), 42u);
}

TL_TEST(cvar_fx_raw_format_and_parse_round_trip, "core,editor,cvar,fast") {
    CvarTable tab;
    cvar_table_init(&tab);
    CvarDesc fx_desc = { "cv_fx"_id, "cv_fx", "an fx-raw cvar", (u32)196608 /*3.0 at FRAC=16*/, (u8)CVAR_FX_RAW, 0, 16, 0 };
    cvar_register(&tab, &fx_desc);
    u8 frac = 0;
    TL_EXPECT_EQ(cvar_get_fx_raw(&tab, "cv_fx"_id, &frac), (i32)196608);
    TL_EXPECT_EQ(frac, (u8)16);
    char buf[32];
    TL_ASSERT_TRUE(cvar_format(&tab, "cv_fx"_id, buf, sizeof(buf)) > 0);
    TL_EXPECT_EQ(strcmp(buf, "raw:196608"), 0);
    TL_ASSERT_EQ(cvar_parse_and_set(&tab, "cv_fx"_id, "raw:-4096"), ERR_OK);
    TL_EXPECT_EQ(cvar_get_fx_raw(&tab, "cv_fx"_id, &frac), (i32)-4096);
}

TL_TEST(cvar_fx_raw_accepts_bare_decimal_literal, "core,editor,cvar,fast") {
    // RR-38 (docs/FX-PALETTE.md §9 R-10): CVAR_FX_RAW now accepts a bare decimal literal too,
    // RNE-quantized at the cvar's own registered frac_bits via fx::fx_parse_decimal_raw -
    // "raw:<i32>" (tested above) is no longer the only accepted form.
    CvarTable tab;
    cvar_table_init(&tab);
    CvarDesc fx_desc = { "cv_fx"_id, "cv_fx", "an fx-raw cvar", (u32)196608 /*3.0 at FRAC=16*/, (u8)CVAR_FX_RAW, 0, 16, 0 };
    cvar_register(&tab, &fx_desc);
    u8 frac = 0;

    TL_ASSERT_EQ(cvar_parse_and_set(&tab, "cv_fx"_id, "3.0"), ERR_OK);
    TL_EXPECT_EQ(cvar_get_fx_raw(&tab, "cv_fx"_id, &frac), (i32)196608);

    TL_ASSERT_EQ(cvar_parse_and_set(&tab, "cv_fx"_id, "-1.5"), ERR_OK);
    TL_EXPECT_EQ(cvar_get_fx_raw(&tab, "cv_fx"_id, &frac), -(i32)(3 * 65536 / 2));   // -1.5 * 65536

    // A malformed or out-of-range decimal literal is ERR_CVAR_PARSE, same as every other kind's
    // own malformed-input row above - fx::fx_parse_decimal_raw's own ERR_FX_PARSE/ERR_FX_RANGE
    // never leaks through as a different code (cvar_parse_and_set's own contract: one error for
    // "value did not parse").
    TL_EXPECT_EQ(cvar_parse_and_set(&tab, "cv_fx"_id, ""), ERR_CVAR_PARSE);
    TL_EXPECT_EQ(cvar_parse_and_set(&tab, "cv_fx"_id, "1.2.3"), ERR_CVAR_PARSE);
    TL_EXPECT_EQ(cvar_parse_and_set(&tab, "cv_fx"_id, "abc"), ERR_CVAR_PARSE);
    TL_EXPECT_EQ(cvar_parse_and_set(&tab, "cv_fx"_id, "999999999"), ERR_CVAR_PARSE);   // out of i32 range
    // Still refused after every malformed attempt above - unchanged from the last successful set.
    TL_EXPECT_EQ(cvar_get_fx_raw(&tab, "cv_fx"_id, &frac), -(i32)(3 * 65536 / 2));
}

TL_TEST(cvar_register_grows_count_per_distinct_key, "core,editor,cvar,fast") {
    // The fatal preconditions (duplicate key, full table - registration-time misconfiguration,
    // matching every other registration door in the tree, reflect.h's TL_COMPONENT precedent)
    // are asserted by inspection rather than a TL_TEST_EXPECT_FATAL child-process run here; this
    // exercises the success path two DISTINCT keys take through cvar_register.
    CvarTable tab;
    cvar_table_init(&tab);
    TL_ASSERT_EQ(tab.count, 0u);
    cvar_register(&tab, &CVAR_cv_i);
    TL_EXPECT_EQ(tab.count, 1u);
    cvar_register(&tab, &CVAR_cv_u);
    TL_EXPECT_EQ(tab.count, 2u);
}
