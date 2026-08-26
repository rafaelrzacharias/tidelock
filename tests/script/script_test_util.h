#pragma once
// ---------------------------------------------------------------------------------------------
// script_test_util.h - one VM, up and down, for the tests/script suite.
//
// Spec: docs/LUAU-LAYER.md §10.11 (the test list this serves), §10.2 (what a VM needs to exist).
// Purpose: every test in this directory wants "a live VM of kind K and nothing else"; without a
//   shared fixture each would re-derive the reserve table, the arena and the teardown, and the
//   first one to get the teardown wrong would leak an OS reservation per test.
// Invariants: hermetic to tests/ - the real OS vmem table comes from tests/foundation, the same
//   one the mem suite uses. Reserves are deliberately SMALL (a few MB, not the 64 MB of
//   docs/MEMORY.md §6): the suite runs hundreds of these and the budget is what is under test,
//   never the size.
// Determinism: nothing here reads a clock or an address; a VM is built from constants.
// Threading: one fixture per test, single-threaded, like every test body.
// Includes: runner/tl_test.h, script/script.h, foundation/vmem_test_api.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/vmem_test_api.h"
#include "foundation/interner.h"
#include "runner/tl_test.h"
#include "script/script.h"

// A VM plus the two things that outlive it: the OS vmem table and the arena its ScriptVm lives
// in. Held by value in the test body; ~1.3 KB, well inside the 1 MB Windows child stack that
// caught the ECS lane's World-sized fixtures (docs/LESSONS.md).
struct ScriptFixture {
    VMemApi   api;
    VMemArena perm;
    ScriptVm* vm;
};

// The ONE interner for this process, built on first use. Not per-fixture: `StrId` is
// process-stable by docs/CANON.md, and script_install_useratom TL_FATALs on a second, different
// Interner precisely because two numberings for one name would let a proxy built against one
// read the wrong field through the other. A per-test interner tripped that guard on the second
// test, which is the guard working - so the fixture models the real shape instead.
// Tests may hold statics (docs/TESTING.md §8 R-2, docs/LESSONS.md); src/ may not.
inline Interner* test_interner() {
    static VMemApi api;
    static VMemArena chars;
    static VMemArena meta;
    static Interner in;
    static bool ready = false;
    if (!ready) {
        api = test_vmem_api();
        if (vmem_arena_init(&chars, 0x5c13u, 1u << 20, 0u, &api) != ERR_OK) return nullptr;
        if (vmem_arena_init(&meta, 0x5c14u, 1u << 20, 0u, &api) != ERR_OK) return nullptr;
        interner_init(&in, &chars, &meta, 1024u);
        ready = true;
    }
    return &in;
}

// Builds a VM of `kind` with a `budget_bytes` pool and the given safepoint budget. Returns false
// (with vm == nullptr) if any step failed, so a caller's TL_ASSERT_TRUE reports the failure
// rather than dereferencing null.
inline bool script_fixture_up(ScriptFixture* f, ScriptVmKind kind, u64 budget_bytes,
                              u32 budget_safepoints) {
    f->vm = nullptr;
    f->api = test_vmem_api();
    if (vmem_arena_init(&f->perm, 0x5c11u, 1u << 20, 0u, &f->api) != ERR_OK) return false;
    ScriptVmDesc d = {};
    d.pool_id = 0x5c12u;
    d.pool_reserve_bytes = budget_bytes < (u64)(64u << 20) ? (u64)(64u << 20) : budget_bytes;
    d.pool_budget_bytes = budget_bytes;
    d.budget_safepoints = budget_safepoints;
    d.gc_step_kb = 16u;
    d.interner = test_interner();
    d.perm = &f->perm;
    d.os = &f->api;
    Result<ScriptVm*> r = kind == SCRIPT_VM_SIM  ? script_create_sim(&d)
                        : kind == SCRIPT_VM_UI   ? script_create_ui(&d)
                                                 : script_create_data(&d);
    if (r.err != ERR_OK) return false;
    f->vm = r.value;
    return true;
}

// The common case: a sim VM with an 8 MB budget and a budget large enough that no honest test
// script trips it. A test about the budget passes its own number.
inline bool script_fixture_up(ScriptFixture* f, ScriptVmKind kind) {
    return script_fixture_up(f, kind, 8u << 20, 1u << 20);
}

// Destroys the VM and releases the fixture's own arena. Safe on a failed script_fixture_up.
inline void script_fixture_down(ScriptFixture* f) {
    if (f->vm != nullptr) {
        script_destroy(f->vm);
        f->vm = nullptr;
    }
    if (f->perm.base != nullptr) {
        f->api.release(f->api.ctx, f->perm.base, f->perm.reserved);
        f->perm.base = nullptr;
    }
}

// True iff `src` runs to completion in `vm`. On failure the test's own message should quote
// script_last_error(vm), which is why this does not assert on its own.
inline bool script_ok(ScriptVm* vm, const char* src) {
    return script_run_source(vm, "test", sv(src)) == ERR_OK;
}
