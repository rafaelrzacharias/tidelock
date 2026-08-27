// table_reader.test.cpp - RR-21 (TODO.md, ruled 2026-08-26): script_eval/script_table_get/
// script_table_geti/script_table_len/script_table_next, direct coverage (core/data_compile.cpp
// exercises eval/get/geti/len already; script_table_next has no other caller, by design - the
// data-table compiler never uses it, RR-21's own binding condition).
#include "script/script.h"
#include "script_test_util.h"
#include <string.h>

TL_TEST(script_eval_returns_every_tagged_kind, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_DATA));

    Result<ScriptValue> nil_v = script_eval(f.vm, sv("nil"));
    TL_ASSERT_EQ(nil_v.err, ERR_OK);
    TL_EXPECT_EQ(nil_v.value.kind, (u8)SCRIPT_VAL_NIL);

    Result<ScriptValue> bool_v = script_eval(f.vm, sv("true"));
    TL_ASSERT_EQ(bool_v.err, ERR_OK);
    TL_EXPECT_EQ(bool_v.value.kind, (u8)SCRIPT_VAL_BOOL);
    TL_EXPECT_EQ(bool_v.value.i, (i64)1);

    Result<ScriptValue> int_v = script_eval(f.vm, sv("42"));
    TL_ASSERT_EQ(int_v.err, ERR_OK);
    TL_EXPECT_EQ(int_v.value.kind, (u8)SCRIPT_VAL_INT);
    TL_EXPECT_EQ(int_v.value.i, (i64)42);

    Result<ScriptValue> str_v = script_eval(f.vm, sv("\"granite\""));
    TL_ASSERT_EQ(str_v.err, ERR_OK);
    TL_EXPECT_EQ(str_v.value.kind, (u8)SCRIPT_VAL_STRING);
    TL_EXPECT_EQ(str_v.value.str_len, 7u);
    TL_EXPECT_EQ(memcmp(str_v.value.str, "granite", 7), 0);

    Result<ScriptValue> non_exact = script_eval(f.vm, sv("1.5"));
    TL_EXPECT_NE(non_exact.err, ERR_OK);

    Result<ScriptValue> fn = script_eval(f.vm, sv("function() end"));
    TL_EXPECT_NE(fn.err, ERR_OK);

    script_fixture_down(&f);
}

TL_TEST(script_table_get_geti_len_over_a_pinned_table, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_DATA));

    Result<ScriptValue> tbl = script_eval(f.vm, sv("{ 10, 20, 30, name = \"x\" }"));
    TL_ASSERT_EQ(tbl.err, ERR_OK);
    TL_ASSERT_EQ(tbl.value.kind, (u8)SCRIPT_VAL_TABLE);
    ScriptTableRef ref = tbl.value.table;

    TL_EXPECT_EQ(script_table_len(f.vm, ref), 3u);

    Result<ScriptValue> e1 = script_table_geti(f.vm, ref, 1u);
    TL_ASSERT_EQ(e1.err, ERR_OK);
    TL_EXPECT_EQ(e1.value.kind, (u8)SCRIPT_VAL_INT);
    TL_EXPECT_EQ(e1.value.i, (i64)10);

    Result<ScriptValue> e_oob = script_table_geti(f.vm, ref, 99u);
    TL_ASSERT_EQ(e_oob.err, ERR_OK);
    TL_EXPECT_EQ(e_oob.value.kind, (u8)SCRIPT_VAL_NIL);

    Result<ScriptValue> named = script_table_get(f.vm, ref, sv("name"));
    TL_ASSERT_EQ(named.err, ERR_OK);
    TL_EXPECT_EQ(named.value.kind, (u8)SCRIPT_VAL_STRING);

    Result<ScriptValue> absent = script_table_get(f.vm, ref, sv("nope"));
    TL_ASSERT_EQ(absent.err, ERR_OK);
    TL_EXPECT_EQ(absent.value.kind, (u8)SCRIPT_VAL_NIL);

    script_table_unref(f.vm, ref);
    script_fixture_down(&f);
}

// Round 1 review D9: SCRIPT_VALUE_STR_MAX's overflow path (script.h: "A longer string is
// ERR_SCRIPT_RUNTIME, not a silent truncation") had live code (to_script_value's LUA_TSTRING
// case) but no direct test. Source length is no longer the constraint here (round 1 review D4),
// so this exercises the VALUE bound cleanly.
TL_TEST(script_eval_string_exceeding_str_max_is_runtime_error, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_DATA));

    Result<ScriptValue> short_v = script_eval(f.vm, sv("string.rep(\"a\", 255)"));
    TL_EXPECT_EQ(short_v.err, ERR_OK);   // one under SCRIPT_VALUE_STR_MAX (256) - not the bound itself

    Result<ScriptValue> long_v = script_eval(f.vm, sv("string.rep(\"a\", 300)"));
    TL_EXPECT_NE(long_v.err, ERR_OK);

    script_fixture_down(&f);
}

TL_TEST(script_table_get_on_stale_ref_is_bad_arg, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_DATA));

    ScriptTableRef stale{};   // never referenced anything
    Result<ScriptValue> v = script_table_get(f.vm, stale, sv("x"));
    TL_EXPECT_EQ(v.err, (ErrCode)ERR_SCRIPT_BAD_ARG);
    TL_EXPECT_EQ(script_table_len(f.vm, stale), 0u);

    script_fixture_down(&f);
}

// Round 1 review D3: script_table_get used lua_gettable, which is metamethod-aware - a data
// script whose row table sets __index could raise (script_panic -> TL_FATAL, no pcall wraps that
// call) or have an absent key answered by the metamethod instead of this reader's own documented
// "SCRIPT_VAL_NIL for absent, never an error" contract. Fixed to lua_rawget, matching
// script_table_geti's existing lua_rawgeti choice. Completing this test (not crashing the
// process) is itself half the fix's proof; the SCRIPT_VAL_NIL check is the other half.
TL_TEST(script_table_get_ignores_metamethods, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_DATA));

    StrView src = sv("(function() local t = setmetatable({}, "
                     "{ __index = function() error(\"boom\") end }); return t end)()");
    Result<ScriptValue> tbl = script_eval(f.vm, src);
    TL_ASSERT_EQ(tbl.err, ERR_OK);
    TL_ASSERT_EQ(tbl.value.kind, (u8)SCRIPT_VAL_TABLE);
    ScriptTableRef ref = tbl.value.table;

    // If this call reached lua_gettable's old, metamethod-aware path, the __index above would
    // either abort the process (TL_FATAL) or hand back a synthesized value - neither is
    // SCRIPT_VAL_NIL for a key genuinely absent from the raw table.
    Result<ScriptValue> v = script_table_get(f.vm, ref, sv("anything"));
    TL_ASSERT_EQ(v.err, ERR_OK);
    TL_EXPECT_EQ(v.value.kind, (u8)SCRIPT_VAL_NIL);

    script_table_unref(f.vm, ref);
    script_fixture_down(&f);
}

// Round 1 review D9: script_table_next's table-key refusal (script.h: "a table key is
// ERR_SCRIPT_RUNTIME, never attempted") had live code, including a hand-written unref-on-refuse
// path (to_script_value pins a ref for the TABLE-kind key before the refusal sees it), but no
// direct test. If that unref path were missing, script_destroy's own live_bytes == 0 assert
// (vm.cpp) would fail inside script_fixture_down below - this test's teardown IS the leak check.
TL_TEST(script_table_next_table_key_is_refused, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_DATA));

    Result<ScriptValue> tbl = script_eval(f.vm, sv("{ [{}] = 1 }"));   // a table used as a key
    TL_ASSERT_EQ(tbl.err, ERR_OK);
    ScriptTableRef ref = tbl.value.table;

    ScriptValue key{};
    ScriptValue val{};
    TL_EXPECT_FALSE(script_table_next(f.vm, ref, &key, &val));
    TL_EXPECT_TRUE(script_last_error(f.vm)[0] != 0);   // a named refusal, not silent exhaustion

    script_table_unref(f.vm, ref);
    script_fixture_down(&f);
}

TL_TEST(script_table_next_walks_every_pair_exactly_once, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_DATA));

    Result<ScriptValue> tbl = script_eval(f.vm, sv("{ a = 1, b = 2, c = 3 }"));
    TL_ASSERT_EQ(tbl.err, ERR_OK);
    ScriptTableRef ref = tbl.value.table;

    ScriptValue key{};   // SCRIPT_VAL_NIL by zero-init: "begin"
    ScriptValue val{};
    u32 seen = 0u;
    i64 sum = 0;
    while (script_table_next(f.vm, ref, &key, &val)) {
        TL_ASSERT_EQ(key.kind, (u8)SCRIPT_VAL_STRING);
        TL_ASSERT_EQ(val.kind, (u8)SCRIPT_VAL_INT);
        sum += val.i;
        ++seen;
        TL_ASSERT_LE(seen, 3u);   // a runaway walk must not hang the suite
    }
    TL_EXPECT_EQ(seen, 3u);
    TL_EXPECT_EQ(sum, (i64)6);

    script_table_unref(f.vm, ref);
    script_fixture_down(&f);
}

TL_TEST(script_table_next_empty_table_returns_false_immediately, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_DATA));

    Result<ScriptValue> tbl = script_eval(f.vm, sv("{}"));
    TL_ASSERT_EQ(tbl.err, ERR_OK);
    ScriptTableRef ref = tbl.value.table;

    ScriptValue key{};
    ScriptValue val{};
    TL_EXPECT_FALSE(script_table_next(f.vm, ref, &key, &val));

    script_table_unref(f.vm, ref);
    script_fixture_down(&f);
}
