// data_tables.test.cpp - data_find_row/data_row over a hand-built DataTable (data_compile itself
// is blocked on RR-21, TODO.md; these two lookups are not).
#include "runner/tl_test.h"
#include "core/data_tables.h"
#include "foundation/vmem_test_api.h"

#define TL_FIELDS_DtRow(X, XA, XH) \
    X(i32, value)
TL_POOL_ROW(DtRow)

TL_TEST(data_find_row_and_row_lookup, "core,data") {
    VMemApi api = test_vmem_api();
    VMemArena arena;
    TL_ASSERT_EQ(vmem_arena_init(&arena, "data_tables_test_arena"_id, 1u * 1024u * 1024u, 0u, &api), ERR_OK);

    TableSchema schema{ &DtRow_info, "granite"_id, 8u, 0u };
    DataTable table{};
    table.schema = &schema;
    DtRow rows[3] = { { 10 }, { 20 }, { 30 } };
    table.rows = (u8*)rows;
    table.count = 3u;
    sorted_map_init(&table.by_name, &arena, 8u);
    sorted_map_put(&table.by_name, "first"_id, (u16)0);
    sorted_map_put(&table.by_name, "second"_id, (u16)1);
    sorted_map_put(&table.by_name, "third"_id, (u16)2);

    DataHandle h = data_find_row(&table, "second"_id);
    TL_ASSERT_FALSE(handle_is_null(h));
    const DtRow* row = (const DtRow*)data_row(&table, h);
    TL_ASSERT_NOT_NULL(row);
    TL_EXPECT_EQ(row->value, 20);

    DataHandle missing = data_find_row(&table, "nope"_id);
    TL_EXPECT_TRUE(handle_is_null(missing));
    TL_EXPECT_NULL(data_row(&table, DataHandle{}));
}
