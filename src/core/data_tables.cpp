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

DataHandle data_find_row(const DataTable*, NameHash) {
    TL_FATAL("unimplemented - RR-21, see TODO.md");
}

const void* data_row(const DataTable*, DataHandle) {
    TL_FATAL("unimplemented - RR-21, see TODO.md");
}
