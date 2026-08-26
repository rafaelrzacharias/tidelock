// ---------------------------------------------------------------------------------------------
// data_tables.cpp - the data-table compiler.
//
// Blocked on RR-21 (TODO.md, W3 assets+data lane notes): the compiler needs a C++-side reader of
// a Luau table returned by a data script, and src/script/script.h (a different, already-merged
// lane's module) exposes no such call. Header-first stub (docs/ROADMAP.md §0 rule 1): TL_FATAL
// until RR-21 resolves.
// ---------------------------------------------------------------------------------------------
#include "core/data_tables.h"

Result<DataTables*> data_compile(Span<const TableSchema>, Span<const StrView>,
                                 VMemArena*, NameHash, const VMemApi*) {
    TL_FATAL("unimplemented - RR-21, see TODO.md");
}

// Pure lookups over an already-compiled DataTable - not blocked on RR-21 (data_compile is the
// blocked half; a table built by a test fixture exercises these today).
DataHandle data_find_row(const DataTable* t, NameHash name) {
    u32 i = sorted_lower_bound<NameHash>(t->by_name.keys.data, t->by_name.keys.count, name);
    if (i >= t->by_name.keys.count || t->by_name.keys.data[i] != name) { return DataHandle{}; }
    return handle_make<DataHandle>((u32)t->by_name.vals.data[i], 1u);
}

const void* data_row(const DataTable* t, DataHandle id) {
    if (handle_is_null(id)) { return nullptr; }
    u32 idx = handle_index(id);
    TL_CHECK(idx < t->count);
    return t->rows + (u64)idx * t->schema->row->size;
}
