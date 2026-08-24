// driver_args.test.cpp - tl_driver's command line. Spec: docs/TESTING.md §9.2.
//
// Why it exists (W1 runner review 2): the driver skeleton's entire contract today IS its parser -
// which flags exist, which are required, which pairs are mutually exclusive - and it shipped with
// no test at all, inside a TU with `main` where nothing could call it. The driver is the harness
// (docs/TESTING.md §3: dual-sim, record/replay and the worker sweep are all driver invocations),
// so a command line it misreads is a determinism gate that ran the wrong thing and said nothing.
//
// Note that this file tests the PARSER, not the boot: a valid invocation exits 70 today
// (EX_SOFTWARE), which is exactly what must never be confused with the real contract's 0 or 3.
#include "runner/tl_test.h"
#include "driver/driver_args.h"

// argv[0] is the program name, as in a real invocation - the parser must skip it.
#define DRV_ARGS(...) \
    const char* raw[] = { "tl_driver", __VA_ARGS__ }; \
    DriverArgs a = driver_args_default(); \
    const char* bad = nullptr; \
    const ErrCode err = driver_parse_args((int)(sizeof(raw) / sizeof(raw[0])), (char**)raw, &a, &bad)

TL_TEST(driver_args_minimal_valid, "driver,fast") {
    DRV_ARGS("--scene", "s.luau", "--seed", "7", "--ticks", "600");
    TL_ASSERT_EQ(err, ERR_OK);
    TL_EXPECT_EQ(a.seed, (i64)7);
    TL_EXPECT_EQ(a.ticks, (i64)600);
    TL_EXPECT_TRUE(tl_mem_eq(a.scene, "s.luau", 7));
    // Everything not given keeps its not-given sentinel - the boot must be able to tell "absent"
    // from "0" for every optional numeric flag.
    TL_EXPECT_EQ(a.workers, (i64)-1);
    TL_EXPECT_EQ(a.snapshot_every, (i64)-1);
    TL_EXPECT_EQ(a.ballast_bytes, (i64)-1);
    TL_EXPECT_NULL(a.workers_sweep);
    TL_EXPECT_NULL(a.record_path);
    TL_EXPECT_NULL(a.replay_path);
    TL_EXPECT_NULL(a.csv_path);
    TL_EXPECT_FALSE(a.verify);
    TL_EXPECT_FALSE(a.dual);
}

TL_TEST(driver_args_every_flag_round_trips, "driver,fast") {
    // Every flag docs/TESTING.md §9.2 names, in one invocation that breaks none of the rules.
    DRV_ARGS("--scene", "harness_a.luau", "--seed", "0", "--ticks", "1",
             "--replay", "in.tlri", "--verify", "--dual",
             "--workers-sweep", "1,2,8,16", "--dump-probes", "probes",
             "--csv", "out.csv", "--snapshot-every", "60", "--ballast", "0");
    TL_ASSERT_EQ(err, ERR_OK);
    TL_EXPECT_EQ(a.seed, (i64)0);              // seed 0 is a legal seed, not "not given"
    TL_EXPECT_EQ(a.ticks, (i64)1);
    TL_EXPECT_EQ(a.snapshot_every, (i64)60);
    TL_EXPECT_EQ(a.ballast_bytes, (i64)0);     // ballast 0 is legal: no ballast
    TL_EXPECT_TRUE(a.verify);
    TL_EXPECT_TRUE(a.dual);
    TL_EXPECT_TRUE(tl_mem_eq(a.replay_path, "in.tlri", 8));
    TL_EXPECT_TRUE(tl_mem_eq(a.workers_sweep, "1,2,8,16", 9));
    TL_EXPECT_TRUE(tl_mem_eq(a.dump_probes_dir, "probes", 7));
    TL_EXPECT_TRUE(tl_mem_eq(a.csv_path, "out.csv", 8));
}

TL_TEST(driver_args_required_flags, "driver,fast") {
    {
        DRV_ARGS("--seed", "1", "--ticks", "1");
        TL_EXPECT_EQ(err, DRV_ERR_NO_SCENE);
    }
    {
        DRV_ARGS("--scene", "s.luau", "--ticks", "1");
        TL_EXPECT_EQ(err, DRV_ERR_NO_SEED);
    }
    {
        DRV_ARGS("--scene", "s.luau", "--seed", "1");
        TL_EXPECT_EQ(err, DRV_ERR_NO_TICKS);
    }
    {
        const char* raw[] = { "tl_driver" };
        DriverArgs a = driver_args_default();
        const char* bad = nullptr;
        TL_EXPECT_EQ(driver_parse_args(1, (char**)raw, &a, &bad), DRV_ERR_NO_SCENE);
    }
}

TL_TEST(driver_args_mutual_exclusions, "driver,fast") {
    {
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks", "1", "--workers", "4", "--workers-sweep", "1,2");
        TL_EXPECT_EQ(err, DRV_ERR_WORKERS_CONFLICT);
    }
    {
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks", "1", "--record", "o.tlri", "--replay", "i.tlri");
        TL_EXPECT_EQ(err, DRV_ERR_RECORD_REPLAY);
    }
    {   // --verify without --replay: verifying a recording that was never opened
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks", "1", "--verify");
        TL_EXPECT_EQ(err, DRV_ERR_VERIFY_NO_REPLAY);
    }
    {   // ...including alongside --record, where it reads as "verify what I am writing"
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks", "1", "--record", "o.tlri", "--verify");
        TL_EXPECT_EQ(err, DRV_ERR_VERIFY_NO_REPLAY);
    }
    {   // --record and --replay each alone are fine
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks", "1", "--record", "o.tlri");
        TL_EXPECT_EQ(err, ERR_OK);
    }
}

TL_TEST(driver_args_rejects_a_non_number, "driver,fast") {
    // atoll answered 0 for all of these, so `--ticks abc` was a silent zero-tick run and
    // `--seed abc` a silent seed 0 - in a binary whose contract is byte-identical output for the
    // same seed (docs/TESTING.md §9.2).
    {
        DRV_ARGS("--scene", "s", "--seed", "abc", "--ticks", "1");
        TL_EXPECT_EQ(err, DRV_ERR_NOT_A_NUMBER);
    }
    {
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks", "abc");
        TL_EXPECT_EQ(err, DRV_ERR_NOT_A_NUMBER);
    }
    {
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks", "12x");   // trailing garbage
        TL_EXPECT_EQ(err, DRV_ERR_NOT_A_NUMBER);
    }
    {
        DRV_ARGS("--scene", "s", "--seed", "-3", "--ticks", "1");    // a negative is not a count
        TL_EXPECT_EQ(err, DRV_ERR_NOT_A_NUMBER);
    }
    {
        DRV_ARGS("--scene", "s", "--seed", "", "--ticks", "1");
        TL_EXPECT_EQ(err, DRV_ERR_NOT_A_NUMBER);
    }
    {   // i64 overflow rejects rather than wrapping into a plausible-looking seed
        DRV_ARGS("--scene", "s", "--seed", "99999999999999999999", "--ticks", "1");
        TL_EXPECT_EQ(err, DRV_ERR_NOT_A_NUMBER);
    }
}

TL_TEST(driver_args_number_parser_edges, "driver,fast") {
    i64 v = -1;
    TL_EXPECT_TRUE(drv_parse_u63("0", &v));                    TL_EXPECT_EQ(v, (i64)0);
    TL_EXPECT_TRUE(drv_parse_u63("1", &v));                    TL_EXPECT_EQ(v, (i64)1);
    TL_EXPECT_TRUE(drv_parse_u63("007", &v));                  TL_EXPECT_EQ(v, (i64)7);
    TL_EXPECT_TRUE(drv_parse_u63("9223372036854775807", &v));  TL_EXPECT_EQ(v, (i64)0x7fffffffffffffff);
    TL_EXPECT_FALSE(drv_parse_u63("9223372036854775808", &v)); // I64_MAX + 1
    TL_EXPECT_FALSE(drv_parse_u63("", &v));
    TL_EXPECT_FALSE(drv_parse_u63(nullptr, &v));
    TL_EXPECT_FALSE(drv_parse_u63(" 1", &v));                  // no leading whitespace
    TL_EXPECT_FALSE(drv_parse_u63("1 ", &v));
    TL_EXPECT_FALSE(drv_parse_u63("+1", &v));
    TL_EXPECT_FALSE(drv_parse_u63("-1", &v));
    TL_EXPECT_FALSE(drv_parse_u63("0x10", &v));
    TL_EXPECT_FALSE(drv_parse_u63("1e3", &v));
    // A rejected parse must not have written to the destination.
    v = 4242;
    TL_EXPECT_FALSE(drv_parse_u63("nope", &v));
    TL_EXPECT_EQ(v, (i64)4242);
}

TL_TEST(driver_args_sweep_must_be_a_sweep, "driver,fast") {
    TL_EXPECT_TRUE(drv_parse_sweep("1"));
    TL_EXPECT_TRUE(drv_parse_sweep("1,2,8,16"));
    TL_EXPECT_TRUE(drv_parse_sweep("16,8,2,1"));
    // A sweep that silently runs one configuration is a worker-sweep gate that swept nothing.
    TL_EXPECT_FALSE(drv_parse_sweep("abc"));
    TL_EXPECT_FALSE(drv_parse_sweep(""));
    TL_EXPECT_FALSE(drv_parse_sweep(nullptr));
    TL_EXPECT_FALSE(drv_parse_sweep("1,"));         // trailing comma
    TL_EXPECT_FALSE(drv_parse_sweep(",1"));         // leading comma
    TL_EXPECT_FALSE(drv_parse_sweep("1,,2"));       // empty element
    TL_EXPECT_FALSE(drv_parse_sweep("1, 2"));       // a space is not a separator
    TL_EXPECT_FALSE(drv_parse_sweep("0"));          // a sweep leg of zero workers
    TL_EXPECT_FALSE(drv_parse_sweep("1,0,4"));
    TL_EXPECT_FALSE(drv_parse_sweep("-1"));
    {
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks", "1", "--workers-sweep", "abc");
        TL_EXPECT_EQ(err, DRV_ERR_BAD_SWEEP);
    }
}

TL_TEST(driver_args_counts_must_be_counts, "driver,fast") {
    {   // A zero-tick run would boot, step nothing, write an empty CSV and exit 0 - a determinism
        // job reporting success having verified nothing.
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks", "0");
        TL_EXPECT_EQ(err, DRV_ERR_BAD_TICKS);
    }
    {
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks", "1", "--workers", "0");
        TL_EXPECT_EQ(err, DRV_ERR_BAD_WORKERS);
    }
    {
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks", "1", "--snapshot-every", "0");
        TL_EXPECT_EQ(err, DRV_ERR_BAD_SNAPSHOT);
    }
}

TL_TEST(driver_args_rejects_malformed_invocations, "driver,fast") {
    {
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks", "1", "--frobnicate");
        TL_EXPECT_EQ(err, DRV_ERR_UNKNOWN_FLAG);
        TL_EXPECT_NOT_NULL(bad);
    }
    {   // A flag whose value is missing must not silently swallow the next flag as its value.
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks");
        TL_EXPECT_EQ(err, DRV_ERR_MISSING_VALUE);
    }
    {
        DRV_ARGS("--scene");
        TL_EXPECT_EQ(err, DRV_ERR_MISSING_VALUE);
    }
    {   // An empty path is not a path.
        DRV_ARGS("--scene", "", "--seed", "1", "--ticks", "1");
        TL_EXPECT_EQ(err, DRV_ERR_EMPTY_VALUE);
    }
    {   // A misspelled flag is rejected, not treated as a positional argument.
        DRV_ARGS("--scene", "s", "--seed", "1", "--ticks", "1", "--Verify");
        TL_EXPECT_EQ(err, DRV_ERR_UNKNOWN_FLAG);
    }
    {   // A bare positional argument likewise.
        DRV_ARGS("scene.luau", "--seed", "1", "--ticks", "1");
        TL_EXPECT_EQ(err, DRV_ERR_UNKNOWN_FLAG);
    }
}

TL_TEST(driver_args_rejection_leaves_no_partial_state, "driver,fast") {
    // docs/CPP-SUBSET.md §3: there are no partial states. A rejected command line must not hand
    // the caller half a configuration - the boot would otherwise run a scene it was never told
    // to run, at a seed nobody asked for.
    DRV_ARGS("--scene", "s.luau", "--seed", "7", "--ticks", "600", "--csv", "o.csv", "--frobnicate");
    TL_ASSERT_EQ(err, DRV_ERR_UNKNOWN_FLAG);
    TL_EXPECT_NULL(a.scene);
    TL_EXPECT_NULL(a.csv_path);
    TL_EXPECT_EQ(a.seed, (i64)-1);
    TL_EXPECT_EQ(a.ticks, (i64)-1);
    const DriverArgs d = driver_args_default();
    TL_EXPECT_TRUE(tl_mem_eq(&a, &d, sizeof(DriverArgs)));
}

TL_TEST(driver_args_every_error_has_a_name, "driver,fast") {
    // Errors carry no strings (docs/CPP-SUBSET.md §3); the name table is how a code becomes a
    // message, so an unnamed code would print "unknown" at the one moment a user needs it.
    const ErrCode all[] = {
        DRV_ERR_UNKNOWN_FLAG, DRV_ERR_MISSING_VALUE, DRV_ERR_NOT_A_NUMBER, DRV_ERR_EMPTY_VALUE,
        DRV_ERR_NO_SCENE, DRV_ERR_NO_SEED, DRV_ERR_NO_TICKS, DRV_ERR_WORKERS_CONFLICT,
        DRV_ERR_RECORD_REPLAY, DRV_ERR_VERIFY_NO_REPLAY, DRV_ERR_BAD_WORKERS, DRV_ERR_BAD_TICKS,
        DRV_ERR_BAD_SWEEP, DRV_ERR_BAD_SNAPSHOT,
    };
    const char* unknown = drv_err_name((ErrCode)0xffff);
    for (usize i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        TL_EXPECT_NE(drv_err_name(all[i]), unknown);
        TL_EXPECT_NE(all[i], ERR_OK);
    }
    TL_EXPECT_TRUE(tl_mem_eq(drv_err_name(ERR_OK), "ok", 3));
}
