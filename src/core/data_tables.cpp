// ---------------------------------------------------------------------------------------------
// data_tables.cpp - the pure DataTable lookups. data_compile itself lives in
// src/script/data_compile.cpp - the module DAG only lets a module that can see BOTH core AND
// script (docs/ARCHITECTURE.md §1: "script": (script, core, ...); "core" cannot include "script")
// drive the data VM this compiler needs, and script/ is the one downstream of core that can.
// ---------------------------------------------------------------------------------------------
#include "core/data_tables.h"

// Pure lookups over an already-compiled DataTable - independent of data_compile's own module.
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
