// ---------------------------------------------------------------------------------------------
// save.cpp - the save file writer/reader.
//
// Header-first stub (docs/ROADMAP.md §0 rule 1): TL_FATAL until the next lane commit fills it in
// with the real header + name-table + arena-block framing over core/encoder.h.
// ---------------------------------------------------------------------------------------------
#include "core/save.h"

ErrCode save_write(const SaveDesc*, const PlatformApi*, StrView, VMemArena*) {
    TL_FATAL("unimplemented");
}

ErrCode save_read(const SaveDesc*, const PlatformApi*, StrView, VMemArena*, u64*, u64*) {
    TL_FATAL("unimplemented");
}
