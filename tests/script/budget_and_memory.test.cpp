// budget_and_memory.test.cpp - the interrupt's safepoint budget and the pool's over-budget path.
// Spec: docs/LUAU-LAYER.md §10.11 rows budget_trip and memory_exhaustion (their VM halves - the
//   trampoline halves, "fatal in netcode / script_paused in dev", belong to the W3 lane that
//   builds the trampoline); §10.2 step 2 (the allocator contract) and step 8 (the budget).
// Rubric: docs/TESTING.md §7.
//
// These two rows are the only ones that exercise the callbacks at all. Everything else in this
// suite runs scripts that finish and allocate little; a VM whose interrupt never fired and whose
// allocator never refused has had two of its three §10.2 contracts untested.
#include "script/script.h"
#include "script_test_util.h"

TL_TEST(budget_trip, "script") {
    ScriptFixture f;
    // A budget small enough that a runaway loop trips it in milliseconds, and large enough that
    // the chunk's own prologue does not.
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM, 8u << 20, 2000u));

    // docs/LUAU-LAYER.md §10.2 step 8: the budget counts SAFEPOINTS, so a loop with no calls in
    // it still trips - Luau's interrupt fires on loop back edges. A runaway script is a
    // deterministic hang on every peer, which is why it must end in an error and not a watchdog.
    const ErrCode e = script_run_source(f.vm, "runaway", sv_lit("while true do end"));
    TL_EXPECT_EQ(e, ERR_SCRIPT_RUNTIME);
    // The message names the contract, not just "error": a budget trip and a script bug must be
    // told apart in a peer's log without a debugger.
    const char* msg = script_last_error(f.vm);
    bool found = false;
    for (u32 i = 0; msg[i] != '\0' && !found; ++i) {
        found = msg[i] == 'b' && msg[i + 1u] == 'u' && msg[i + 2u] == 'd' && msg[i + 3u] == 'g';
    }
    TL_EXPECT_TRUE(found);
    TL_EXPECT_LT(script_budget_left(f.vm), (i64)0);

    // The trip is not sticky across ticks: tick_begin reloads the budget and the VM works again.
    // A VM that stayed dead after one runaway would turn a recoverable dev mistake into a restart.
    script_tick_begin(f.vm);
    TL_EXPECT_EQ(script_budget_left(f.vm), (i64)2000);
    TL_EXPECT_EQ(script_run_source(f.vm, "after", sv_lit("local x = 1 + 1")), ERR_OK);

    // A budget big enough for honest work does not fire: the row above must not be passing
    // because the interrupt fires on everything.
    script_fixture_down(&f);
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM, 8u << 20, 1u << 20));
    TL_EXPECT_EQ(script_run_source(f.vm, "honest",
                                   sv_lit("local s = 0 for i = 1, 1000 do s = s + i end")), ERR_OK);
    TL_EXPECT_GT(script_budget_left(f.vm), (i64)0);
    script_fixture_down(&f);
}

TL_TEST(memory_exhaustion, "script") {
    ScriptFixture f;
    // A pool far too small for the allocation the script asks for. docs/LUAU-LAYER.md §10.2 step
    // 2: over budget the adaptor returns NULL, Luau raises LUA_ERRMEM ("not enough memory"), and
    // that unwinds to the nearest protected call - always ours.
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM, 1u << 20, 1u << 20));

    const ErrCode e = script_run_source(f.vm, "exhaust", sv_lit("local t = table.create(1000000)"));
    TL_EXPECT_EQ(e, ERR_SCRIPT_RUNTIME);
    const char* msg = script_last_error(f.vm);
    bool oom = false;
    for (u32 i = 0; msg[i] != '\0' && !oom; ++i) {
        oom = msg[i] == 'm' && msg[i + 1u] == 'e' && msg[i + 2u] == 'm' && msg[i + 3u] == 'o';
    }
    TL_EXPECT_TRUE(oom);

    // The VM survives it. An out-of-memory that left the state unusable would make the budget a
    // crash switch rather than a bound, and the whole point of a budget is that it is survivable.
    TL_EXPECT_EQ(script_run_source(f.vm, "after", sv_lit("local x = 1 + 1")), ERR_OK);

    // The pool is inside its budget throughout - the refusal came from the budget, not from the
    // OS, which is what makes it identical on every peer.
    const MemPoolStats* st = script_pool_stats(f.vm);
    TL_EXPECT_LE(st->live_bytes, (u64)(1u << 20));
    TL_EXPECT_LE(st->peak_bytes, (u64)(1u << 20));
    script_fixture_down(&f);
}

TL_TEST(sortedpairs_order_is_a_function_of_the_key_set, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));

    // The property `sortedpairs` exists for, stated the way docs/LESSONS.md demands: not "two
    // identical tables walk identically" (which any implementation passes, including the `pairs`
    // this replaced), but DIVERGENT HISTORIES THAT CONVERGE - the same key set reached by
    // different insertion orders, with deletions and reinsertions in between, must walk the same.
    // That is exactly the property Luau's hash-part order does NOT have, and the reason §1.1
    // removed `pairs` from the sim VM.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local function walk(t)\n"
        "  local out = {}\n"
        "  for k in sortedpairs(t) do out[#out+1] = tostring(k) end\n"
        "  return table.concat(out, ',')\n"
        "end\n"
        // History A: ascending inserts.
        "local a = {}\n"
        "for i = 1, 40 do a['k' .. i] = i end\n"
        // History B: descending inserts, with churn that reshapes the hash part.
        "local b = {}\n"
        "for i = 40, 1, -1 do b['k' .. i] = i end\n"
        "for i = 1, 40, 2 do b['k' .. i] = nil end\n"
        "for i = 1, 40, 2 do b['k' .. i] = i end\n"
        // History C: interleaved, plus keys added and removed that are not in the final set.
        "local c = {}\n"
        "for i = 1, 40 do\n"
        "  c['zz' .. i] = i\n"
        "  c['k' .. (41 - i)] = 41 - i\n"
        "end\n"
        "for i = 1, 40 do c['zz' .. i] = nil end\n"
        "local wa, wb, wc = walk(a), walk(b), walk(c)\n"
        "assert(wa == wb, wa .. ' vs ' .. wb)\n"
        "assert(wa == wc, wa .. ' vs ' .. wc)\n"
        // ...and the walk is not vacuously short: 40 keys, and the order is the bytewise one
        // ('k1,k10,k11,...' - not the numeric one, because these keys are STRINGS).
        "local n = 0 for _ in sortedpairs(a) do n = n + 1 end\n"
        "assert(n == 40, n)\n"
        "assert(string.sub(wa, 1, 11) == 'k1,k10,k11,', string.sub(wa, 1, 11))\n"));
    script_fixture_down(&f);
}
