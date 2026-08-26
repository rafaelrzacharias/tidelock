// sandbox.test.cpp - the sim VM's library set, removal list, replacements and freeze.
// Spec: docs/LUAU-LAYER.md §10.11 rows sandbox_removed_globals, sandbox_readonly,
//   sandbox_tostring, sortedpairs_order; docs/CANON.md "Luau sim VM" owns the removal list.
// Rubric: docs/TESTING.md §7.
//
// Every assertion is made FROM Luau, through script_run_source, on purpose: the test binary may
// not include a Luau header (tools/audit/includes.py confines them to src/script), so the only
// honest way to ask "is `math` nil in this VM" is to ask the VM.
#include "script/script.h"
#include "script_test_util.h"

TL_TEST(sandbox_removed_globals, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));

    // docs/CANON.md's list, name for name. `assert` survives (it is base-library and pure), which
    // is what makes this shape possible at all.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local gone = {'math','os','io','debug','pairs','next','coroutine','loadstring',"
        "'collectgarbage','gcinfo','getfenv','setfenv','newproxy','print',"
        "'rawequal','rawget','rawset'}\n"
        "for i = 1, #gone do assert(_G[gone[i]] == nil, gone[i] .. ' survived') end\n"
        "assert(string.rep == nil, 'string.rep survived')\n"
        "assert(table.foreach == nil, 'table.foreach survived')\n"
        "assert(table.foreachi == nil, 'table.foreachi survived')\n"));
    TL_EXPECT_EQ(script_last_error(f.vm)[0], '\0');

    // What must still be there: the sim VM is restricted, not crippled.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "assert(ipairs ~= nil and sortedpairs ~= nil and select ~= nil)\n"
        "assert(type(string.sub) == 'function' and type(table.insert) == 'function')\n"
        "assert(type(fx) == 'table')\n"));
    script_fixture_down(&f);
}

TL_TEST(sandbox_ui_vm_keeps_everything, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_UI));
    // The UI VM removes nothing (docs/LUAU-LAYER.md §10.2 step 4): it never touches sim state, and
    // an inspector that cannot walk a table with `pairs` is not an inspector.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "assert(pairs ~= nil and next ~= nil and math ~= nil and coroutine ~= nil and os ~= nil)\n"
        "local n = 0 for _ in pairs({a=1, b=2}) do n = n + 1 end assert(n == 2)\n"));
    // ...and it is the only VM that may turn a row back into a double.
    TL_EXPECT_TRUE(script_ok(f.vm, "assert(type(fx.to_f64) == 'function')"));
    script_fixture_down(&f);
}

// True iff `needle` occurs in `hay`. Spelled out: the test binary has no <string.h> grant and
// docs/CPP-SUBSET.md §1 bans the STL, so a substring check is four lines rather than a call.
static bool err_mentions(const char* hay, const char* needle) {
    for (u32 i = 0; hay[i] != '\0'; ++i) {
        u32 j = 0;
        while (needle[j] != '\0' && hay[i + j] == needle[j]) ++j;
        if (needle[j] == '\0') return true;
    }
    return false;
}

// Runs one expression in `vm` and asserts it was REFUSED by the data VM's reference guard - the
// call failed AND the message is the guard's, not some other error. Both halves matter: a typo in
// the source would also make script_ok false, and a row that accepted any failure would pass on
// it (the "cannot fail on its subject" class review round 2 was about).
static bool refuses_reference(ScriptVm* vm, const char* src) {
    if (script_run_source(vm, "ref", sv(src)) == ERR_OK) return false;
    return err_mentions(script_last_error(vm), "no deterministic string form");
}

TL_TEST(sandbox_data_vm_stringifying_a_reference_raises, "script") {
    // RULED 2026-08-26 (Rafael), ship round F-1, option (a) - error at the mistake.
    //
    // The finding: the deterministic tostring/format were gated to the SIM VM, so the data VM
    // kept stock both - and stock tostring of a reference is `luaL_tolstring`'s default case,
    // which prints the object POINTER (vendor/luau/VM/src/laux.cpp:606-612 at the 0.696 pin).
    // The data VM's OUTPUT is hashed (docs/LUAU-LAYER.md §1), so `"tier_" .. tostring(x)` with a
    // table `x` writes an address into a hashed table: the peers disagree, and it surfaces as a
    // fingerprint mismatch on handshake rather than as an error on the line that made it. Exactly
    // the D4 math.random shape, through a different door, and ruled the same way.
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_DATA));

    // Every reference kind a data file can actually construct. lightuserdata is guarded by the
    // same predicate and is deliberately absent here: Luau has no script-side constructor for it
    // (it arrives from the engine), so a row would have to fake one.
    TL_EXPECT_TRUE(refuses_reference(f.vm, "return tostring({})"));
    TL_EXPECT_TRUE(refuses_reference(f.vm, "return tostring(function() end)"));
    TL_EXPECT_TRUE(refuses_reference(f.vm, "return tostring(newproxy(false))"));
    TL_EXPECT_TRUE(refuses_reference(f.vm, "return tostring(coroutine.create(function() end))"));
    TL_EXPECT_TRUE(refuses_reference(f.vm, "return tostring(buffer.create(8))"));

    // ...and through string.format, by BOTH conversions. `%*` is the one that leaks under stock
    // Luau (stock `%s` type-errors on a table instead), so a fix that handled only `%s` would
    // leave the real channel open - which is why CANON's contract now names both.
    TL_EXPECT_TRUE(refuses_reference(f.vm, "return string.format('%s', {})"));
    TL_EXPECT_TRUE(refuses_reference(f.vm, "return string.format('%*', {})"));
    TL_EXPECT_TRUE(refuses_reference(f.vm, "return string.format('a %* b', coroutine.create(function() end))"));

    // The control the ruling asks for: the data VM is restricted, not crippled. Value types keep
    // the STOCK conversion - not the sim VM's substitution - because F-1 closes the address
    // channel and changes nothing else about how a data file formats its numbers.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "assert(tostring(12) == '12' and tostring(true) == 'true' and tostring(nil) == 'nil')\n"
        "assert(tostring('hi') == 'hi' and tostring(2.5) == '2.5')\n"
        "assert(string.format('%s %d', 'a', 3) == 'a 3')\n"
        "assert(string.format('%*', 7) == '7')\n"
        "assert(string.format('%%s') == '%s')\n"
        "assert(string.format('%10s|', 'x') == '         x|')\n"));
    TL_EXPECT_EQ(script_last_error(f.vm)[0], '\0');

    // The process is alive and the VM still usable after a raise - the refusal is an error, not a
    // TL_FATAL. A guard that took the VM down with it would turn an authoring typo into a crash.
    TL_EXPECT_FALSE(script_ok(f.vm, "return tostring({})"));
    TL_EXPECT_TRUE(script_ok(f.vm, "assert(1 + 1 == 2)"));
    script_fixture_down(&f);

    // The sim VM's half is UNCHANGED, pinned here beside the data VM's so the two philosophies
    // sit in one place: sim SUBSTITUTES (a sim script that logs a table must keep running),
    // data RAISES. A later edit that unified them - in either direction - turns this red.
    ScriptFixture g;
    TL_ASSERT_TRUE(script_fixture_up(&g, SCRIPT_VM_SIM));
    TL_EXPECT_TRUE(script_ok(g.vm,
        "assert(tostring({}) == 'table')\n"
        "assert(string.format('%*', {}) == 'table')\n"));
    TL_EXPECT_EQ(script_last_error(g.vm)[0], '\0');
    script_fixture_down(&g);

    // ...and so is the UI VM's: it keeps stock both, so an address is REACHABLE there by design
    // (§10.2 step 5 - it feeds no hashed state and the inspector wants it). Asserting the address
    // is present rather than merely "not equal to 'table'" is what makes this row notice if the
    // guard is ever widened to all three VMs by accident.
    ScriptFixture u;
    TL_ASSERT_TRUE(script_fixture_up(&u, SCRIPT_VM_UI));
    TL_EXPECT_TRUE(script_ok(u.vm,
        "local s = tostring({})\n"
        "assert(string.sub(s, 1, 7) == 'table: ', s)\n"));
    script_fixture_down(&u);
}

TL_TEST(sandbox_data_vm_removals, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_DATA));
    // docs/LUAU-LAYER.md §10.2 step 4: the data VM removes os, io, loadstring, getfenv, setfenv.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "assert(os == nil and io == nil and loadstring == nil)\n"
        "assert(getfenv == nil and setfenv == nil)\n"));
    // It keeps the stock set otherwise - a data file is arithmetic and tables, and its OUTPUT is
    // what is hashed (docs/ASSETS-AND-DATA.md §3), not the VM it was built in.
    TL_EXPECT_TRUE(script_ok(f.vm, "assert(math ~= nil and pairs ~= nil and type(fx.pos) == 'function')"));

    // ...EXCEPT the two that are seeded from an address and the wall clock (ruled 2026-08-26,
    // review round 1's D4). This assertion previously read `assert(math ~= nil)` and nothing
    // more, which BLESSED the hole - it is what turned an omission into a decision nobody made,
    // so the replacement names both functions rather than the table.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "assert(math.random == nil, 'math.random survived in the data VM')\n"
        "assert(math.randomseed == nil, 'math.randomseed survived in the data VM')\n"
        "assert(math.floor ~= nil and math.abs ~= nil, 'the pure half of math must remain')\n"
        "assert(math.floor(2.7) == 2 and math.max(1, 5) == 5)\n"));

    // The property the removal buys, stated as the failure it prevents: a data table's output is
    // hashed, so two VMs in one process must agree. Any expression a data file can write is a
    // pure function of its inputs now, and the same expression in two VMs gives the same bytes.
    ScriptFixture g;
    TL_ASSERT_TRUE(script_fixture_up(&g, SCRIPT_VM_DATA));
    const Result<i64> a = script_eval_int(f.vm, sv_lit("math.floor(math.pi * 1000000)"));
    const Result<i64> b = script_eval_int(g.vm, sv_lit("math.floor(math.pi * 1000000)"));
    TL_EXPECT_EQ(a.err, ERR_OK);
    TL_EXPECT_EQ(b.err, ERR_OK);
    TL_EXPECT_EQ(a.value, b.value);
    script_fixture_down(&g);
    // The sim VM's fx.to_f64 exclusion is not a data-VM rule: a data file writes literals.
    TL_EXPECT_TRUE(script_ok(f.vm, "assert(fx.to_f64 == nil)"));
    script_fixture_down(&f);
}

TL_TEST(sandbox_readonly, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));

    // Before the seal, init may still write globals (that is how ecs.component binds a name).
    TL_EXPECT_TRUE(script_init_open(f.vm));
    TL_EXPECT_TRUE(script_ok(f.vm, "Health = 1"));

    TL_ASSERT_EQ(script_seal(f.vm), ERR_OK);
    TL_EXPECT_FALSE(script_init_open(f.vm));
    // A second seal is an error, not a no-op: a double-seal in the wiring is a real mistake.
    TL_EXPECT_EQ(script_seal(f.vm), ERR_SCRIPT_SEALED);

    // docs/LUAU-LAYER.md §10.2 step 11: after the seal a global assignment raises. Checked
    // through pcall so the test sees the MESSAGE, not just a failure - "readonly" is the
    // specific promise, and any other error would mean something else broke.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local ok, err = pcall(function() Other = 1 end)\n"
        "assert(not ok, 'a global assignment was allowed after the seal')\n"
        "assert(string.find(err, 'readonly') ~= nil, err)\n"));
    // The binding tables are frozen too, so a script cannot make its own function look official.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local ok = pcall(function() fx.pos = 1 end) assert(not ok)\n"
        "local ok2 = pcall(function() string.sub = nil end) assert(not ok2)\n"
        "local ok3 = pcall(function() table.insert = nil end) assert(not ok3)\n"));

    // Review round 1, D3: the STRING METATABLE is the fourth table a sealed sim script can reach
    // - `getmetatable('')` is not on the removal list, the table is permanently rooted by the VM,
    // and the freeze used to miss it. A field written there survived across ticks and across
    // chunks, which is a live breach of docs/LUAU-LAYER.md §0. The write must raise.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local mt = getmetatable('')\n"
        "assert(mt ~= nil, 'the string metatable is reachable - that is the point')\n"
        "local ok, err = pcall(function() mt.tl_hidden = 41 end)\n"
        "assert(not ok, 'a sealed sim VM accepted a write to the string metatable')\n"
        "assert(string.find(err, 'readonly') ~= nil, err)\n"
        "local ok2 = pcall(function() mt.__index = nil end)\n"
        "assert(not ok2, 'the metatable__index was writable')\n"));

    // ...and the property the write would have bought: nothing a script can reach survives to a
    // later chunk. Stated as the ACROSS-TICKS shape the defect actually took, so a future freeze
    // that re-narrows itself fails here rather than in a dual-sim run three lanes later.
    TL_EXPECT_TRUE(script_ok(f.vm, "local ok = pcall(function() getmetatable('').tl_probe = 7 end)"));
    script_tick_begin(f.vm);
    script_tick_end(f.vm);
    TL_EXPECT_TRUE(script_ok(f.vm,
        "assert(getmetatable('').tl_probe == nil, 'state crossed a tick boundary in the Luau heap')"));
    script_fixture_down(&f);
}

TL_TEST(sandbox_tostring, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));
    // docs/LUAU-LAYER.md §10.2 step 5: no address may reach a script. The stock tostring prints
    // `table: 0x...`, which differs per run and per peer.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "assert(tostring({}) == 'table', tostring({}))\n"
        "assert(tostring(print or function() end) == 'function')\n"
        "assert(tostring(12) == '12' and tostring(true) == 'true' and tostring(nil) == 'nil')\n"));
    // string.format routes %s through it - and %*, Luau's own extension, which is the path that
    // actually leaks an address (stock %s type-errors on a table instead).
    TL_EXPECT_TRUE(script_ok(f.vm,
        "assert(string.format('%s', {}) == 'table', string.format('%s', {}))\n"
        "assert(string.format('%*', {}) == 'table', string.format('%*', {}))\n"
        "assert(string.format('%s %d', 'a', 3) == 'a 3')\n"
        "assert(string.format('%%s') == '%s')\n"
        "assert(string.format('%10s|', 'x') == '         x|')\n"));
    script_fixture_down(&f);
}

TL_TEST(sortedpairs_order, "script") {
    ScriptFixture f;
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_SIM));

    // docs/LUAU-LAYER.md §10.2.1: numbers first ascending, then strings bytewise.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local t = { [3]='c', b='B', [1]='a', a='A', [2]='b' }\n"
        "local seen = ''\n"
        "for k, v in sortedpairs(t) do seen = seen .. tostring(k) .. '=' .. v .. ' ' end\n"
        "assert(seen == '1=a 2=b 3=c a=A b=B ', seen)\n"));

    // Bytewise, and shorter first on a common prefix - not a locale collation, not length-first.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local t = { ab=1, a=2, B=3, aa=4, ['']=5 }\n"
        "local order = {}\n"
        "for k in sortedpairs(t) do order[#order+1] = k end\n"
        "assert(table.concat(order, ',') == ',B,a,aa,ab', table.concat(order, ','))\n"));

    // Negative and large integer keys are ordered by VALUE, not by their string form.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local t = { [-5]='a', [10]='b', [-1]='c', [2]='d' }\n"
        "local order = {}\n"
        "for k in sortedpairs(t) do order[#order+1] = k end\n"
        "assert(table.concat(order, ',') == '-5,-1,2,10', table.concat(order, ','))\n"));

    // A key set with more entries than one merge pass: the sort has to actually be a sort.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local t = {}\n"
        "for i = 1, 64 do t['k' .. string.format('%03d', 65 - i)] = i end\n"
        "local prev = nil local n = 0\n"
        "for k in sortedpairs(t) do\n"
        "  if prev ~= nil then assert(prev < k, prev .. ' >= ' .. k) end\n"
        "  prev = k n = n + 1\n"
        "end\n"
        "assert(n == 64, n)\n"));

    // A value cleared mid-walk is SKIPPED, never re-ordered: the key array was fixed at the start.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local t = { a=1, b=2, c=3, d=4 }\n"
        "local seen = ''\n"
        "for k in sortedpairs(t) do seen = seen .. k t.c = nil end\n"
        "assert(seen == 'abd', seen)\n"));

    // Unsupported key types are an error naming the type, and a non-integer number key is an
    // error too (a float key would order by a value the palette forbids anywhere else).
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local ok, err = pcall(function() for _ in sortedpairs({ [true]=1 }) do end end)\n"
        "assert(not ok and string.find(err, 'unsupported key type') ~= nil, tostring(err))\n"
        "local ok2, err2 = pcall(function() for _ in sortedpairs({ [1.5]=1 }) do end end)\n"
        "assert(not ok2 and string.find(err2, 'not an integer') ~= nil, tostring(err2))\n"
        "local ok3 = pcall(function() for _ in sortedpairs(7) do end end)\n"
        "assert(not ok3)\n"));

    // The empty table walks zero times rather than erroring on the n == 0 path.
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local n = 0 for _ in sortedpairs({}) do n = n + 1 end assert(n == 0)\n"
        "local m = 0 for _ in sortedpairs({ only = 1 }) do m = m + 1 end assert(m == 1)\n"));

    // Same code in all three VMs (§10.2.1), so the data VM's table compiler gets the same order.
    script_fixture_down(&f);
    TL_ASSERT_TRUE(script_fixture_up(&f, SCRIPT_VM_DATA));
    TL_EXPECT_TRUE(script_ok(f.vm,
        "local order = {}\n"
        "for k in sortedpairs({ b=1, [2]=2, a=3, [1]=4 }) do order[#order+1] = tostring(k) end\n"
        "assert(table.concat(order, ',') == '1,2,a,b', table.concat(order, ','))\n"));
    script_fixture_down(&f);
}

TL_TEST(fx_table_per_vm_kind, "script") {
    // Review round 1, D7: vm.h's contract said the data VM got "the literal constructors and the
    // constants" while script_bind_fx bound the full table to every kind, and nothing checked
    // either claim. Reconciled in favour of the code - every op is a pure integer function, so
    // none can make a hashed data table peer-divergent, and refusing them bought nothing - and
    // pinned here so the contract and the binding cannot drift apart again silently.
    static const ScriptVmKind KINDS[] = { SCRIPT_VM_SIM, SCRIPT_VM_UI, SCRIPT_VM_DATA };
    for (u32 i = 0; i < 3u; ++i) {
        ScriptFixture f;
        TL_ASSERT_TRUE(script_fixture_up(&f, KINDS[i]));
        // The whole table, in every kind: literals, constants, the op set and the integer helpers.
        TL_EXPECT_TRUE(script_ok(f.vm,
            "local want = {'pos','vel','invmass','stiff','q','angle','omega','scalar','raw',"
            "'mul_q','mul_scalar','div_q','mul_pos_vel_dt','vel_from_delta','dist','normalize',"
            "'sincos','atan2','atan2_q','lerp','sat_add','sat_sub','imin','imax','iabs','iclamp',"
            "'str','rng_below','rng_q'}\n"
            "for _, n in ipairs(want) do\n"
            "  assert(type(fx[n]) == 'function', n .. ' missing from the fx table')\n"
            "end\n"
            "for _, n in ipairs({'H','INV_H','G_SUBSTEP','TEXEL','V_MAX_WORLD','POS','OMEGA'}) do\n"
            "  assert(type(fx[n]) == 'number', n .. ' missing from the fx constants')\n"
            "end\n"));
        script_fixture_down(&f);
    }

    // `to_f64` is the ONE kind-gated entry, and it is gated the way round: the UI VM draws and
    // prints, and neither feeds state; a scaled double is exactly what must never exist in the
    // sim VM, and the data VM's output is hashed so it must not be there either.
    ScriptFixture ui, sim, data;
    TL_ASSERT_TRUE(script_fixture_up(&ui, SCRIPT_VM_UI));
    TL_EXPECT_TRUE(script_ok(ui.vm, "assert(type(fx.to_f64) == 'function')"));
    TL_EXPECT_TRUE(script_ok(ui.vm, "assert(fx.to_f64(fx.pos(12.5), fx.POS) == 12.5)"));
    script_fixture_down(&ui);
    TL_ASSERT_TRUE(script_fixture_up(&sim, SCRIPT_VM_SIM));
    TL_EXPECT_TRUE(script_ok(sim.vm, "assert(fx.to_f64 == nil)"));
    script_fixture_down(&sim);
    TL_ASSERT_TRUE(script_fixture_up(&data, SCRIPT_VM_DATA));
    TL_EXPECT_TRUE(script_ok(data.vm, "assert(fx.to_f64 == nil)"));
    script_fixture_down(&data);
}
