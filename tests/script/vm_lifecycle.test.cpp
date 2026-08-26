// vm_lifecycle.test.cpp - VM construction, teardown, the pool, and the capability facts.
// Spec: docs/LUAU-LAYER.md §10.2 (the construction sequence), §10.11 rows memory_exhaustion (the
//   pool halves) and interner_atoms (the part this lane can honestly answer); docs/MEMORY.md §8.6.
// Rubric: docs/TESTING.md §7.
//
// Every row here runs in EVERY tier. That matters: until TODO.md RR-18 is ruled, this file is the
// script suite's only coverage in dev and netcode, because nothing in it compiles Luau source.
#include "script/script.h"
#include "script_test_util.h"

TL_TEST(vm_lifecycle_all_kinds, "script") {
    // Creation alone proves a great deal: script_sandbox_open VERIFIES its own removal list and
    // fails creation with ERR_SCRIPT_SANDBOX if any name survived (docs/LUAU-LAYER.md §10.2 step
    // 4), so a sim VM that exists is a sim VM whose globals are gone - checkable with no compiler.
    static const ScriptVmKind KINDS[] = { SCRIPT_VM_SIM, SCRIPT_VM_UI, SCRIPT_VM_DATA };
    for (u32 i = 0; i < 3u; ++i) {
        ScriptFixture f;
        TL_ASSERT_TRUE(script_fixture_up(&f, KINDS[i]));
        TL_EXPECT_EQ(script_kind(f.vm), KINDS[i]);
        TL_EXPECT_TRUE(script_init_open(f.vm));

        // The VM's whole heap is its own pool: a live lua_State has allocated from it, and every
        // byte is inside the budget. (script_destroy asserts the counter returns to zero after
        // lua_close - the memory_exhaustion row's second half, which lives in the module because
        // the pool is gone by the time a test could look.)
        const MemPoolStats* st = script_pool_stats(f.vm);
        TL_ASSERT_NOT_NULL(st);
        TL_EXPECT_GT(st->live_bytes, (u64)0);
        TL_EXPECT_GE(st->peak_bytes, st->live_bytes);

        script_fixture_down(&f);
    }
}

TL_TEST(vm_rejects_bad_desc, "script") {
    // docs/LUAU-LAYER.md §10.2: "A step that fails returns Result<ScriptVm*> with SCRIPT_* codes;
    // no partial VM survives." Each row below is one malformed field; a VM must never come back.
    VMemApi api = test_vmem_api();
    VMemArena perm;
    TL_ASSERT_EQ(vmem_arena_init(&perm, 0x5c21u, 1u << 20, 0u, &api), ERR_OK);

    ScriptVmDesc good = {};
    good.pool_id = 0x5c22u;
    good.pool_reserve_bytes = 64u << 20;
    good.pool_budget_bytes = 8u << 20;
    good.budget_safepoints = 1u << 20;
    good.gc_step_kb = 16u;
    good.perm = &perm;
    good.os = &api;

    TL_EXPECT_EQ(script_create_sim(nullptr).err, ERR_SCRIPT_BAD_ARG);

    ScriptVmDesc d = good; d.perm = nullptr;
    TL_EXPECT_EQ(script_create_sim(&d).err, ERR_SCRIPT_BAD_ARG);
    d = good; d.os = nullptr;
    TL_EXPECT_EQ(script_create_sim(&d).err, ERR_SCRIPT_BAD_ARG);
    d = good; d.pool_reserve_bytes = 0;
    TL_EXPECT_EQ(script_create_sim(&d).err, ERR_SCRIPT_BAD_ARG);
    d = good; d.pool_budget_bytes = 0;
    TL_EXPECT_EQ(script_create_sim(&d).err, ERR_SCRIPT_BAD_ARG);
    // A budget larger than the reserve is not a big budget, it is a wrong reserve table: the pool
    // would TL_FATAL "arena over reserve" on the carve that crossed it, far from the mistake.
    d = good; d.pool_budget_bytes = d.pool_reserve_bytes + 1u;
    TL_EXPECT_EQ(script_create_sim(&d).err, ERR_SCRIPT_BAD_ARG);

    // ...and the good desc still works, so the rows above rejected the FIELD, not the shape.
    Result<ScriptVm*> r = script_create_sim(&good);
    TL_EXPECT_EQ(r.err, ERR_OK);
    TL_ASSERT_NOT_NULL(r.value);
    script_destroy(r.value);
    api.release(api.ctx, perm.base, perm.reserved);
}

TL_TEST(vm_pool_budget_refuses_creation, "script") {
    // docs/LUAU-LAYER.md §10.11 memory_exhaustion, the first half: over budget, the pool returns
    // null, Luau cannot build its state, and creation reports ERR_SCRIPT_OOM. No partial VM, no
    // trap - the budget is a fingerprinted input (docs/MEMORY.md §7 R-2), so every peer refuses
    // the same allocation at the same point.
    ScriptFixture f;
    TL_EXPECT_FALSE(script_fixture_up(&f, SCRIPT_VM_SIM, 4u * 1024u, 1u << 20));
    TL_EXPECT_NULL(f.vm);
    script_fixture_down(&f);

    // One granule over the same edge, the VM builds. Without this row the one above would pass
    // for any reason at all, including a fixture that never reached pool_init.
    ScriptFixture g;
    TL_EXPECT_TRUE(script_fixture_up(&g, SCRIPT_VM_SIM, 8u << 20, 1u << 20));
    script_fixture_down(&g);
}

TL_TEST(vm_seal_needs_no_compiler, "script") {
    // The seal is pure C API, so it is checkable in every tier - unlike the READONLY BEHAVIOUR it
    // installs, which needs a script to observe and lives in sandbox.test.cpp.
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));
    TL_EXPECT_TRUE(script_init_open(f.vm));
    TL_EXPECT_EQ(script_seal(f.vm), ERR_OK);
    TL_EXPECT_FALSE(script_init_open(f.vm));
    TL_EXPECT_EQ(script_seal(f.vm), ERR_SCRIPT_SEALED);
    TL_EXPECT_NE(script_last_error(f.vm)[0], '\0');   // the second seal left a message
    script_fixture_down(&f);
}

TL_TEST(vm_tick_bracket_resets_the_budget, "script") {
    // docs/LUAU-LAYER.md §10.7 step 1, the VM half: tick_begin reloads the safepoint budget and
    // runs the GC step. Both are cost control; neither can change state, which is why the bracket
    // is testable with no world and no script.
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM, 8u << 20, 5000u));
    TL_EXPECT_EQ(script_budget_left(f.vm), (i64)5000);
    script_tick_begin(f.vm);
    TL_EXPECT_EQ(script_budget_left(f.vm), (i64)5000);
    script_tick_end(f.vm);
    script_tick_begin(f.vm);
    TL_EXPECT_EQ(script_budget_left(f.vm), (i64)5000);
    // A hundred brackets on an idle VM must not grow the heap: the GC step and the dev leak
    // collect are the whole point of the bracket, and a bracket that leaked would be worse than
    // no bracket at all.
    const u64 before = script_pool_stats(f.vm)->live_bytes;
    for (u32 i = 0; i < 100u; ++i) { script_tick_begin(f.vm); script_tick_end(f.vm); }
    TL_EXPECT_LE(script_pool_stats(f.vm)->live_bytes, before);
    script_fixture_down(&f);
}

TL_TEST(vm_capability_facts_are_consistent, "script") {
    // The three "this build cannot do X" facts are reported, not guessed, and each is queryable
    // rather than discovered by a trap. Their VALUES are a build property; what this row pins is
    // that the report and the behaviour agree - a fact that lies is worse than no fact.
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));

    // TODO.md RR-20: CodeGen is not vendored in rev 1.
    TL_EXPECT_FALSE(script_codegen_available());
    // TODO.md RR-19: Luau's useratom callback carries no context, so it is not installed.
    TL_EXPECT_FALSE(script_useratom_installed());

    // TODO.md RR-18: where the compiler cannot run, script_run_source REFUSES with a code -
    // it does not trap, and it does not quietly succeed.
    const ErrCode e = script_run_source(f.vm, "probe", sv_lit("return 1"));
    if (script_can_compile_in_process()) {
        TL_EXPECT_EQ(e, ERR_OK);
    } else {
        TL_EXPECT_EQ(e, ERR_SCRIPT_NO_COMPILER);
        TL_EXPECT_NE(script_last_error(f.vm)[0], '\0');
    }
    // script_eval_int takes the same path, so the two entry points cannot disagree.
    const Result<i64> r = script_eval_int(f.vm, sv_lit("1 + 1"));
    TL_EXPECT_EQ(r.err, script_can_compile_in_process() ? ERR_OK : ERR_SCRIPT_NO_COMPILER);
    if (r.err == ERR_OK) TL_EXPECT_EQ(r.value, (i64)2);

    // Bad arguments are rejected before the compiler is even consulted, in every tier.
    TL_EXPECT_EQ(script_run_source(f.vm, "probe", StrView{ nullptr, 0 }), ERR_SCRIPT_BAD_ARG);
    TL_EXPECT_EQ(script_eval_int(f.vm, StrView{ nullptr, 0 }).err, ERR_SCRIPT_BAD_ARG);
    script_fixture_down(&f);
}
