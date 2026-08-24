// jobs.cpp - the worker pool (docs/JOBS.md §6). HEADER-FIRST SLICE: the contracts of jobs.h and
// atomic.h are declared and compiled here; the pool itself lands in the next commit. This TU
// exists so the two headers - and every static_assert in them - are actually seen by a compiler:
// a header no TU includes has never been compiled (LESSONS.md, the same class as a template with
// no call site).
#include "foundation/jobs.h"
#include "foundation/atomic.h"

extern const u32 tl_foundation_unit;
const u32 tl_foundation_unit = 0;
