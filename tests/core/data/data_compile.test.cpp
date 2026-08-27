// data_compile.test.cpp - docs/ASSETS-AND-DATA.md §8.5: two compiles of the same scripts hash
// identically; RR-21's binding condition pinned directly - a compiled table's bytes are
// invariant to the SOURCE table's field insertion order AND to Luau's own internal table walk
// order (script_table_next's walk order must never reach a hashed output, and this compiler
// never calls script_table_next at all - it walks schema-ordered via script_table_get/
// script_table_geti).
//
// Round 1 review D2 (2026-08-27, PR #14): the original single test here
// (data_compile_two_field_orders_hash_identically, kept below under a renamed, narrowed claim)
// did not discriminate - its two source strings walk in IDENTICAL script_table_next order (Luau
// places a small string-keyed table by KEY HASH, not insertion order, so varying the LITERAL
// field order in the source text can never vary the table's real layout - the premise the
// original test implicitly relied on was false), and the reviewer measured that a compile_table
// mutated to assign fields POSITIONALLY from a raw script_table_next walk (bypassing
// script_table_get's named lookup entirely) still passed all six tests in this file. Fixed below
// by data_compile_fields_are_name_keyed_not_walk_order_keyed: a six-field row with pairwise
// distinct values, so ANY non-identity assignment is directly observable per field regardless of
// what Luau's real walk order turns out to be, plus a witness (a throwaway VM, independent of
// data_compile's own) that walks the identical row table with script_table_next and asserts its
// real order is not simply the schema's declared field order - the one case that would let a
// positional-walk defect hide from the per-field check, same as it hid from the original test.
// Verified against the exact defect it excludes: temporarily mutating compile_table's field loop
// to assign positionally from a raw script_table_next walk (recorded in TODO.md's W3 assets+data
// lane notes, with the command and the real failure output) makes this new test fail; reverting
// the mutation makes it pass again.
#include "runner/tl_test.h"
#include "platform/platform_test_util.h"
#include "core/data_tables.h"
#include "foundation/mem_pool.h"
#include "script/script_test_util.h"
#include <stdio.h>
#include <string.h>

#define TL_FIELDS_TestMaterial(X, XA, XH) \
    X(i32, density) X(i32, hardness)
TL_POOL_ROW(TestMaterial)

#define TL_FIELDS_TestWidget(X, XA, XH) \
    X(i32, v1) X(i32, v2) X(i32, v3) X(i32, v4) X(i32, v5) X(i32, v6)
TL_POOL_ROW(TestWidget)

namespace {

struct Fixture {
    const PlatformApi* platform;
    MemPool compile_pool;
    VMemArena perm;
};

void fixture_init(TestCtx* t, Fixture* f) {
    f->platform = platform_test_init();
    TL_ASSERT_NOT_NULL(f->platform);
    TL_ASSERT_EQ(pool_init(&f->compile_pool, "data_compile_test_pool"_id, 16u * 1024u * 1024u,
                           8u * 1024u * 1024u, &f->platform->vmem), ERR_OK);
    TL_ASSERT_EQ(vmem_arena_init(&f->perm, "data_compile_test_perm"_id, 16u * 1024u * 1024u, 0u,
                                 &f->platform->vmem), ERR_OK);
}

void fixture_shutdown(Fixture* f) { platform_test_shutdown(f->platform); }

}  // namespace

// Narrowed claim (round 1 review D2): this proves the compiler doesn't accidentally depend on the
// SOURCE TEXT's literal field order - a real property, trivially true for a name-keyed lookup and
// still worth guarding against a future regression - but it does NOT by itself prove the compiler
// is immune to Luau's own internal walk order (see this file's top-of-file note and the test
// below, which is the one that actually pins RR-21's binding condition).
TL_TEST(data_compile_source_field_order_does_not_affect_hash, "core,data") {
    Fixture f{};
    fixture_init(t, &f);

    TableSchema schema{ &TestMaterial_info, sv("materials"), 8u, 0u };
    Span<const TableSchema> schemas{ &schema, 1u };

    StrView src_a = sv("{ materials = { { name = \"granite\", density = 2700, hardness = 6 } } }");
    StrView src_b = sv("{ materials = { { name = \"granite\", hardness = 6, density = 2700 } } }");

    Result<DataTables*> r_a = data_compile(schemas, Span<const StrView>{ &src_a, 1u },
                                           &f.perm, "dt_a"_id, &f.platform->vmem, &f.compile_pool);
    TL_ASSERT_EQ(r_a.err, ERR_OK);
    Result<DataTables*> r_b = data_compile(schemas, Span<const StrView>{ &src_b, 1u },
                                           &f.perm, "dt_b"_id, &f.platform->vmem, &f.compile_pool);
    TL_ASSERT_EQ(r_b.err, ERR_OK);

    TL_EXPECT_EQ(r_a.value->hash, r_b.value->hash);
    TL_ASSERT_EQ(r_a.value->t[0].count, 1u);
    TL_ASSERT_EQ(r_b.value->t[0].count, 1u);
    const TestMaterial* row_a = (const TestMaterial*)(r_a.value->t[0].rows);
    const TestMaterial* row_b = (const TestMaterial*)(r_b.value->t[0].rows);
    TL_EXPECT_EQ(row_a->density, row_b->density);
    TL_EXPECT_EQ(row_a->hardness, row_b->hardness);
    TL_EXPECT_EQ(row_a->density, 2700);
    TL_EXPECT_EQ(row_a->hardness, 6);

    DataHandle h = data_find_row(&r_a.value->t[0], sv_hash(sv("granite")));
    TL_ASSERT_FALSE(handle_is_null(h));
    TL_EXPECT_EQ(((const TestMaterial*)data_row(&r_a.value->t[0], h))->density, 2700);

    fixture_shutdown(&f);
}

// THE pin for RR-21's binding determinism condition (round 1 review D2). Six pairwise-distinct
// field values mean any assignment that isn't exactly name-keyed is directly observable per
// field, independent of what Luau's real internal walk order happens to be for this key set -
// unlike the renamed test above, which could not tell a correct compile from a positional one.
TL_TEST(data_compile_fields_are_name_keyed_not_walk_order_keyed, "core,data") {
    const char* ROW_LITERAL = "{ name = \"w\", v1 = 11, v2 = 22, v3 = 33, v4 = 44, v5 = 55, v6 = 66 }";

    // Part 1: assert the premise. Witness Luau's ACTUAL walk order over the identical row table
    // with a throwaway VM independent of data_compile's own - if it happened to equal the
    // schema's declared field order (v1..v6), a positional-walk defect would be invisible to Part
    // 2 below, the same blind spot that let the original pin pass under the mutation. Fail loudly
    // here instead of silently shipping a pin that cannot discriminate.
    {
        ScriptFixture wf{};
        TL_ASSERT_TRUE(script_fixture_up(&wf, SCRIPT_VM_DATA));
        Result<ScriptValue> tbl = script_eval(wf.vm, sv(ROW_LITERAL));
        TL_ASSERT_EQ(tbl.err, ERR_OK);
        ScriptTableRef ref = tbl.value.table;

        char digit_at[6] = { 0, 0, 0, 0, 0, 0 };
        u32 vi = 0;
        ScriptValue key{};
        ScriptValue val{};
        while (script_table_next(wf.vm, ref, &key, &val)) {
            TL_ASSERT_EQ(key.kind, (u8)SCRIPT_VAL_STRING);
            if (key.str_len == 4u && memcmp(key.str, "name", 4) == 0) { continue; }
            TL_ASSERT_EQ(key.str_len, 2u);
            TL_ASSERT_EQ(key.str[0], 'v');
            TL_ASSERT_LT(vi, 6u);
            digit_at[vi] = key.str[1];
            ++vi;
        }
        TL_ASSERT_EQ(vi, 6u);
        const bool identity_order = digit_at[0] == '1' && digit_at[1] == '2' && digit_at[2] == '3' &&
                                    digit_at[3] == '4' && digit_at[4] == '5' && digit_at[5] == '6';
        TL_ASSERT_FALSE(identity_order);

        script_table_unref(wf.vm, ref);
        script_fixture_down(&wf);
    }

    // Part 2: compile through the real data_compile path and check every field lands where its
    // NAME says, not where the witnessed walk order (Part 1) would have put it.
    Fixture f{};
    fixture_init(t, &f);

    TableSchema schema{ &TestWidget_info, sv("widgets"), 8u, 0u };
    Span<const TableSchema> schemas{ &schema, 1u };
    char src_buf[128];
    u32 n = 0;
    const char* prefix = "{ widgets = { ";
    while (prefix[n] != 0) { src_buf[n] = prefix[n]; ++n; }
    u32 rn = 0;
    while (ROW_LITERAL[rn] != 0) { src_buf[n] = ROW_LITERAL[rn]; ++n; ++rn; }
    src_buf[n] = ' '; ++n; src_buf[n] = '}'; ++n; src_buf[n] = '}'; ++n; src_buf[n] = 0;
    StrView src{ src_buf, n };

    Result<DataTables*> r = data_compile(schemas, Span<const StrView>{ &src, 1u },
                                         &f.perm, "dt_widget"_id, &f.platform->vmem, &f.compile_pool);
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_ASSERT_EQ(r.value->t[0].count, 1u);
    const TestWidget* row = (const TestWidget*)(r.value->t[0].rows);
    TL_EXPECT_EQ(row->v1, 11);
    TL_EXPECT_EQ(row->v2, 22);
    TL_EXPECT_EQ(row->v3, 33);
    TL_EXPECT_EQ(row->v4, 44);
    TL_EXPECT_EQ(row->v5, 55);
    TL_EXPECT_EQ(row->v6, 66);

    fixture_shutdown(&f);
}

TL_TEST(data_compile_missing_field_named_error, "core,data") {
    Fixture f{};
    fixture_init(t, &f);

    TableSchema schema{ &TestMaterial_info, sv("materials"), 8u, 0u };
    Span<const TableSchema> schemas{ &schema, 1u };
    StrView src = sv("{ materials = { { name = \"granite\", density = 2700 } } }");   // hardness missing, no default_row

    Result<DataTables*> r = data_compile(schemas, Span<const StrView>{ &src, 1u },
                                         &f.perm, "dt_missing"_id, &f.platform->vmem, &f.compile_pool);
    TL_EXPECT_EQ(r.err, ERR_DATA_MISSING_FIELD);

    fixture_shutdown(&f);
}

TL_TEST(data_compile_out_of_range_int_named_error, "core,data") {
    Fixture f{};
    fixture_init(t, &f);

    TableSchema schema{ &TestMaterial_info, sv("materials"), 8u, 0u };
    Span<const TableSchema> schemas{ &schema, 1u };
    StrView src = sv("{ materials = { { name = \"granite\", density = 2700, hardness = 99999999999 } } }");   // exact integer, outside i32's range

    Result<DataTables*> r = data_compile(schemas, Span<const StrView>{ &src, 1u },
                                         &f.perm, "dt_range"_id, &f.platform->vmem, &f.compile_pool);
    TL_EXPECT_EQ(r.err, ERR_DATA_BAD_INT);

    fixture_shutdown(&f);
}

TL_TEST(data_compile_unknown_table_named_error, "core,data") {
    Fixture f{};
    fixture_init(t, &f);

    TableSchema schema{ &TestMaterial_info, sv("materials"), 8u, 0u };
    Span<const TableSchema> schemas{ &schema, 1u };
    StrView src = sv("{ species = {} }");   // no `materials` key at all

    Result<DataTables*> r = data_compile(schemas, Span<const StrView>{ &src, 1u },
                                         &f.perm, "dt_unknown"_id, &f.platform->vmem, &f.compile_pool);
    TL_EXPECT_EQ(r.err, ERR_DATA_UNKNOWN_TABLE);

    fixture_shutdown(&f);
}

TL_TEST(data_compile_duplicate_name_named_error, "core,data") {
    Fixture f{};
    fixture_init(t, &f);

    TableSchema schema{ &TestMaterial_info, sv("materials"), 8u, 0u };
    Span<const TableSchema> schemas{ &schema, 1u };
    StrView src = sv("{ materials = { "
                     "{ name = \"granite\", density = 2700, hardness = 6 }, "
                     "{ name = \"granite\", density = 2600, hardness = 5 } } }");

    Result<DataTables*> r = data_compile(schemas, Span<const StrView>{ &src, 1u },
                                         &f.perm, "dt_dup"_id, &f.platform->vmem, &f.compile_pool);
    TL_EXPECT_EQ(r.err, ERR_DATA_DUPLICATE_NAME);

    fixture_shutdown(&f);
}

// Round 1 review D4: script_eval staged "return (<expr>)" into a fixed char buf[SCRIPT_ERR_MAX]
// (1024, the ERROR-MESSAGE bound, reused by accident as a SOURCE bound), capping a data script at
// ~1014 bytes - measured failure at 21 two-field rows, well under a real Alloy material table.
// Fixed by staging in vm.cpp's load_wrapped_expr (via tl_luau_alloc over the VM's compile_pool,
// sized to the real expression, freed once load_chunk returns). 40 rows here is comfortably past
// both the old cap and the 21-row measured failure point, so this exercises the fix rather than
// happening to fit.
TL_TEST(data_compile_source_larger_than_the_old_1024_byte_cap, "core,data") {
    Fixture f{};
    fixture_init(t, &f);

    char src_buf[4096];
    u32 n = 0;
    const char* prefix = "{ materials = { ";
    for (u32 i = 0; prefix[i] != 0; ++i) { src_buf[n++] = prefix[i]; }
    for (u32 row = 0; row < 40u; ++row) {
        char piece[96];
        int m = snprintf(piece, sizeof(piece), "{ name = \"m%u\", density = %u, hardness = %u }, ",
                         row, 1000u + row, row % 10u);
        TL_ASSERT_GT(m, 0);
        for (int k = 0; k < m; ++k) { src_buf[n++] = piece[(u32)k]; }
        TL_ASSERT_LT(n, (u32)sizeof(src_buf) - 8u);
    }
    const char* suffix = "} }";
    for (u32 i = 0; suffix[i] != 0; ++i) { src_buf[n++] = suffix[i]; }
    StrView src{ src_buf, n };
    TL_ASSERT_GT(n, 1024u);   // proves this source actually exercises the old cap, not just close to it

    TableSchema schema{ &TestMaterial_info, sv("materials"), 64u, 0u };
    Span<const TableSchema> schemas{ &schema, 1u };
    Result<DataTables*> r = data_compile(schemas, Span<const StrView>{ &src, 1u },
                                         &f.perm, "dt_big"_id, &f.platform->vmem, &f.compile_pool);
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_EXPECT_EQ(r.value->t[0].count, 40u);
    DataHandle h = data_find_row(&r.value->t[0], sv_hash(sv("m39")));
    TL_ASSERT_FALSE(handle_is_null(h));
    TL_EXPECT_EQ(((const TestMaterial*)data_row(&r.value->t[0], h))->density, 1039);

    fixture_shutdown(&f);
}

// Round 1 review D9: ERR_DATA_SCRIPT had live code (data_compile's own script_eval failure path)
// but no direct test - every existing failure test hit one of compile_field's OWN named codes
// instead. A syntax error never reaches compile_field at all.
TL_TEST(data_compile_syntax_error_named_error, "core,data") {
    Fixture f{};
    fixture_init(t, &f);

    TableSchema schema{ &TestMaterial_info, sv("materials"), 8u, 0u };
    Span<const TableSchema> schemas{ &schema, 1u };
    StrView src = sv("{ materials = { ");   // unterminated - a Luau compile error, not a data error

    Result<DataTables*> r = data_compile(schemas, Span<const StrView>{ &src, 1u },
                                         &f.perm, "dt_syntax"_id, &f.platform->vmem, &f.compile_pool);
    TL_EXPECT_EQ(r.err, ERR_DATA_SCRIPT);

    fixture_shutdown(&f);
}

// Round 1 review D9: ERR_DATA_TOO_MANY_ROWS had live code (compile_table's count > max_rows
// check) but no direct test.
TL_TEST(data_compile_too_many_rows_named_error, "core,data") {
    Fixture f{};
    fixture_init(t, &f);

    TableSchema schema{ &TestMaterial_info, sv("materials"), 1u, 0u };   // max_rows = 1
    Span<const TableSchema> schemas{ &schema, 1u };
    StrView src = sv("{ materials = { "
                     "{ name = \"a\", density = 1, hardness = 1 }, "
                     "{ name = \"b\", density = 2, hardness = 2 } } }");

    Result<DataTables*> r = data_compile(schemas, Span<const StrView>{ &src, 1u },
                                         &f.perm, "dt_toomany"_id, &f.platform->vmem, &f.compile_pool);
    TL_EXPECT_EQ(r.err, ERR_DATA_TOO_MANY_ROWS);

    fixture_shutdown(&f);
}

// Round 1 review D9: ERR_DATA_TABLE_LIMIT had live code (data_compile's schemas.count > MAX_TABLES
// check) but no direct test.
TL_TEST(data_compile_too_many_schemas_named_error, "core,data") {
    Fixture f{};
    fixture_init(t, &f);

    TableSchema schemas_buf[MAX_TABLES + 1u];
    for (u32 i = 0; i < MAX_TABLES + 1u; ++i) {
        schemas_buf[i] = TableSchema{ &TestMaterial_info, sv("materials"), 8u, 0u };
    }
    Span<const TableSchema> schemas{ schemas_buf, MAX_TABLES + 1u };
    StrView src = sv("{ materials = {} }");

    Result<DataTables*> r = data_compile(schemas, Span<const StrView>{ &src, 1u },
                                         &f.perm, "dt_toomanyschemas"_id, &f.platform->vmem, &f.compile_pool);
    TL_EXPECT_EQ(r.err, ERR_DATA_TABLE_LIMIT);

    fixture_shutdown(&f);
}
