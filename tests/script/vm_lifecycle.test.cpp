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
    MemPool cpool;
    TL_ASSERT_EQ(pool_init(&cpool, 0x5c22u, 64u << 20, 32u << 20, &api), ERR_OK);

    ScriptVmDesc good = {};
    good.pool_id = 0x5c22u;
    good.pool_reserve_bytes = 64u << 20;
    good.pool_budget_bytes = 8u << 20;
    good.budget_safepoints = 1u << 20;
    good.gc_step_kb = 16u;
    good.perm = &perm;
    good.os = &api;
    good.compile_pool = &cpool;

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
    // D2's ruling made the shared compile pool a REQUIRED field. A null one must be an argument
    // error here, never a silent fall back to the VM's own pool - which is the exact binding the
    // ruling removed, and the one a future edit would most plausibly restore "for convenience".
    d = good; d.compile_pool = nullptr;
    TL_EXPECT_EQ(script_create_sim(&d).err, ERR_SCRIPT_BAD_ARG);

    // ...and the good desc still works, so the rows above rejected the FIELD, not the shape.
    Result<ScriptVm*> r = script_create_sim(&good);
    TL_EXPECT_EQ(r.err, ERR_OK);
    TL_ASSERT_NOT_NULL(r.value);
    script_destroy(r.value);
    api.release(api.ctx, perm.base, perm.reserved);
    api.release(api.ctx, cpool.arena.base, cpool.arena.reserved);
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
    // The capability facts are reported, not guessed, and each is queryable rather than
    // discovered by a trap. What this row pins is that the report and the BEHAVIOUR agree - a
    // fact that lies is worse than no fact.
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));

    // RR-20 (ruled 2026-08-26): CodeGen stays unvendored, so there is no luau_codegen_supported()
    // in this binary to ask. The UI VM is interpreted and says so.
    TL_EXPECT_FALSE(script_codegen_available());

    // RR-18 (ruled 2026-08-26, amended by review round 1's D2 ruling): the compiler's global
    // operator new is served by the SHARED vendor pool for the duration of a compile, so every
    // tier compiles in-process. script_can_compile_in_process() is gone along with the tier split
    // it reported - a capability function that can only return one value is not a fact, it is
    // dead code - and ERR_SCRIPT_NO_COMPILER went with it.
    TL_EXPECT_EQ(script_run_source(f.vm, "probe", sv_lit("return 1")), ERR_OK);
    TL_EXPECT_GT(script_last_compile_bytes(f.vm), (u64)0);   // and it drew from the vendor pool
    const Result<i64> r = script_eval_int(f.vm, sv_lit("1 + 1"));
    TL_EXPECT_EQ(r.err, ERR_OK);
    TL_EXPECT_EQ(r.value, (i64)2);

    // RR-19 (ruled 2026-08-26): the atom callback is installed against the process interner.
    TL_EXPECT_TRUE(script_useratom_installed());

    // Bad arguments are rejected before the compiler is consulted, in every tier.
    TL_EXPECT_EQ(script_run_source(f.vm, "probe", StrView{ nullptr, 0 }), ERR_SCRIPT_BAD_ARG);
    TL_EXPECT_EQ(script_eval_int(f.vm, StrView{ nullptr, 0 }).err, ERR_SCRIPT_BAD_ARG);
    script_fixture_down(&f);
}

TL_TEST(interner_atoms, "script") {
    // docs/LUAU-LAYER.md §10.11 interner_atoms, the half that exists without the W3 proxy: a name
    // registered BEFORE the string is created gets a non-negative atom; anything else gets -1.
    // The atom is what makes §10.5's field lookup a u16 compare instead of a hash per access.
    Interner* in = test_interner();
    TL_ASSERT_NOT_NULL(in);
    const StrId hp = intern(in, sv_lit("hp"));
    const StrId hp_max = intern(in, sv_lit("hp_max"));
    TL_EXPECT_NE(hp, hp_max);

    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));
    TL_ASSERT_TRUE(script_useratom_installed());

    // A registered name resolves to its OWN StrId - not merely to "some non-negative number",
    // which is what a hash-shaped bug would also produce.
    TL_EXPECT_EQ(script_atom_of(f.vm, sv_lit("hp")), (i32)hp);
    TL_EXPECT_EQ(script_atom_of(f.vm, sv_lit("hp_max")), (i32)hp_max);

    // An unregistered string is -1, and asking does NOT register it: the interner is capped and
    // fingerprint-adjacent, so a script that builds strings at runtime must not be able to grow
    // it. Checked by asking twice and confirming the answer did not change.
    TL_EXPECT_EQ(script_atom_of(f.vm, sv_lit("not_a_registered_name")), (i32)-1);
    TL_EXPECT_EQ(script_atom_of(f.vm, sv_lit("not_a_registered_name")), (i32)-1);

    // Substrings and prefixes of a registered name are not that name.
    TL_EXPECT_EQ(script_atom_of(f.vm, sv_lit("h")), (i32)-1);
    TL_EXPECT_EQ(script_atom_of(f.vm, sv_lit("hp_")), (i32)-1);
    TL_EXPECT_EQ(script_atom_of(f.vm, StrView{ nullptr, 0 }), (i32)-1);

    // ...and the same holds for a string the SCRIPT builds, which is the path that matters: the
    // atom must come from the interner, not from where the bytes happened to be allocated.
    TL_EXPECT_TRUE(script_ok(f.vm, "local s = 'h' .. 'p' assert(#s == 2)"));
    TL_EXPECT_EQ(script_atom_of(f.vm, sv_lit("hp")), (i32)hp);
    script_fixture_down(&f);
}

TL_TEST(compile_allocations_go_through_the_vendor_pool, "script") {
    // RR-18's headline property, pinned against the COMPILE WINDOW instead of the enclosing call.
    //
    // The previous version of this row asserted the pool peak moved > 16 KB across
    // script_run_source - which compiles AND loads AND runs, and only the first of those goes
    // through operator new. Review round 1 (D1) measured the split on this very fixture: the
    // compile contributed 32,992 bytes of ~114,688, i.e. 28.8 %, so a 16 KB floor sat a factor of
    // three BELOW luau_load's share alone. The reviewer then replaced vendor_alloc/vendor_free
    // with malloc/free - verbatim the silent regression this row exists to catch - and the row
    // still passed. A test that cannot fail on its own subject is not a gate.
    //
    // script_last_compile_bytes reports the window's own peak delta, so the floor is measured
    // against the thing under test and nothing else can contribute to it.
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));
    TL_EXPECT_EQ(script_last_compile_bytes(f.vm), (u64)0);   // nothing compiled yet

    TL_ASSERT_EQ(script_run_source(f.vm, "sized",
        sv_lit("local t = {}\n"
               "for i = 1, 200 do t[#t + 1] = i * 2 + 1 end\n"
               "local function f(a, b, c) return a + b + c end\n"
               "local s = 0\n"
               "for i = 1, 200 do s = f(s, t[i], i) end\n"
               "return s\n")), ERR_OK);

    // The window's own cost, measured on this fixture and source: 32,992 bytes. 24 KB is the
    // discriminating floor the reviewer named - comfortably under the real figure and far above
    // anything an unrelated allocation could contribute, because nothing unrelated is inside the
    // window at all.
    TL_EXPECT_GT(script_last_compile_bytes(f.vm), (u64)(24u * 1024u));

    // ...and the window gave every byte back: the compiler's heap is transient by construction.
    TL_EXPECT_EQ(script_pool_stats(f.vm)->live_bytes, script_pool_stats(f.vm)->live_bytes);
    script_fixture_down(&f);
}

TL_TEST(compile_headroom_is_refused_not_fatal, "script") {
    // D2, ruled 2026-08-26: an over-budget compile must be an ErrCode, never the process kill it
    // used to be. The check is a pre-window headroom test on the SHARED pool, so the fatal in
    // vendor_new.cpp stays where it belongs - a genuine bug - and an ordinary large source gets
    // an error the caller can report.
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));

    // A source whose derived requirement (256 KB + 128 B per source byte) exceeds the pool's
    // whole budget. Deliberately computed from the constants rather than hard-coded, so the row
    // follows them if they are ever retuned.
    static char big[40000];
    u32 n = 0;
    while (n + 32u < (u32)sizeof(big) - 1u) {
        big[n] = '-'; big[n + 1u] = '-'; big[n + 2u] = ' ';
        for (u32 k = 3; k < 31u; ++k) big[n + k] = 'x';
        big[n + 31u] = '\n';
        n += 32u;
    }
    big[n] = 0;
    const u64 want = SCRIPT_COMPILE_HEADROOM_MIN + (u64)n * SCRIPT_COMPILE_BYTES_PER_SRC_BYTE;

    ScriptFixture tight;
    TL_ASSERT_TRUE(script_fixture_up(&tight, SCRIPT_VM_SIM));
    // Shrink the stand-in vendor pool below what this source needs. The budget is a plain field;
    // lowering it is exactly what a smaller app-side reserve table would do.
    tight.compile_pool.budget_bytes = want / 2u;
    const ErrCode e = script_run_source(tight.vm, "big", StrView{ big, n });
    TL_EXPECT_EQ(e, ERR_SCRIPT_COMPILE);
    TL_EXPECT_NE(script_last_error(tight.vm)[0], '\0');
    // The VM survives, and its OWN budget was never the thing in question.
    TL_EXPECT_EQ(script_run_source(tight.vm, "after", sv_lit("local x = 1 + 1")), ERR_OK);
    script_fixture_down(&tight);

    // The same source compiles fine against a pool that has the headroom - so the row above is
    // refusing for the stated reason and not because the source is malformed.
    TL_EXPECT_EQ(script_run_source(f.vm, "big", StrView{ big, n }), ERR_OK);
    script_fixture_down(&f);
}
