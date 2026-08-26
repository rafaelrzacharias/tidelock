// data_compile.test.cpp - docs/ASSETS-AND-DATA.md §8.5: two compiles of the same scripts hash
// identically; RR-21's binding condition pinned directly - a compiled table's bytes are
// invariant to the SOURCE table's field insertion order (script_table_next's walk order must
// never reach a hashed output, and this compiler never calls script_table_next at all - it walks
// schema-ordered via script_table_get/script_table_geti).
#include "runner/tl_test.h"
#include "platform/platform_test_util.h"
#include "core/data_tables.h"
#include "foundation/mem_pool.h"

#define TL_FIELDS_TestMaterial(X, XA, XH) \
    X(i32, density) X(i32, hardness)
TL_POOL_ROW(TestMaterial)

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

TL_TEST(data_compile_two_field_orders_hash_identically, "core,data") {
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
