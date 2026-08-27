// console_cvar.test.cpp - the console's built-in `set <name> <value>` command (Review C's D1(b),
// docs/TOOLING.md §9.3.5's Cvars clause): SIM routing through the sealed CMD_SET_CVAR door,
// READONLY refusal, lockstep refusal (command-level, per CONSOLE_SIM_AFFECTING), parse errors,
// unknown cvars, and the shared cvar_parse_raw parser (no second one written for this command).
// Spec: docs/TOOLING.md §9.3.5. Rubric: docs/TESTING.md §7.
//
// TL_TEST's generated signature is `(TestCtx* t)`.
//
// editor/console.cpp is compiled ONLY on the debug/dev tiers (src/editor/CMakeLists.txt links an
// empty INTERFACE tl_editor on netcode/ship), so every call site here is behind `#if TL_DEV`,
// matching console.test.cpp's own shape.
#include "runner/tl_test.h"

#if TL_DEV
#include "core/world_test_util.h"
#include "editor/console.h"
#include "core/cvar.h"

#include <string.h>

namespace {
TL_CVAR(i32, cc_plain, 7, 0, "a non-sim cvar - the ordinary cvar_set_raw door");
TL_CVAR(bool, cc_sim, false, CVAR_SIM, "a sim-fingerprinted cvar - the CMD_SET_CVAR door");
TL_CVAR(u32, cc_ro, 42u, CVAR_READONLY, "a readonly cvar - must refuse both doors");

// A dedicated CvarTable per fixture slot, never on the stack (world_test_util.h's own
// WorldFixture precedent - World alone is ~256 KB and a Windows child gets a 1 MB stack).
CvarTable g_test_cvars[4];

struct SetFixture {
    WorldFixture* wf;
    ConsoleState cs;
};

SetFixture make_set_fixture(u32 slot) {
    WorldFixture& f = *wt_fixture(slot);
    TL_CHECK(world_fixture_init(&f, 40u + slot));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);
    cvar_table_init(&g_test_cvars[slot]);
    cvar_register(&g_test_cvars[slot], &CVAR_cc_plain);
    cvar_register(&g_test_cvars[slot], &CVAR_cc_sim);
    cvar_register(&g_test_cvars[slot], &CVAR_cc_ro);
    f.w.cvars = &g_test_cvars[slot];

    SetFixture sf;
    sf.wf = &f;
    console_init(&sf.cs);
    console_register_cvar_set(&sf.cs);
    return sf;
}
}  // namespace
#endif  // TL_DEV

#define TL_CONSOLE_CVAR_SKIP TL_SKIP("editor/console.cpp is dev-only (TOOLING.md section 9.1); no symbol in this tier")

TL_TEST(console_set_registers_findable_command, "editor,console,cvar,fast") {
#if TL_DEV
    SetFixture sf = make_set_fixture(0u);
    const ConsoleCmd* cmd = console_find(&sf.cs, sv_lit("set"));
    TL_ASSERT_NOT_NULL(cmd);
    TL_EXPECT_EQ(strcmp(cmd->usage, "set <name> <value>"), 0);
    TL_EXPECT_EQ(cmd->argc_min, (u8)2u);
    TL_EXPECT_EQ(cmd->argc_max, (u8)2u);
    TL_EXPECT_EQ(cmd->flags, (u8)CONSOLE_SIM_AFFECTING);
#else
    TL_CONSOLE_CVAR_SKIP;
#endif
}

TL_TEST(console_set_non_sim_cvar_writes_immediately, "editor,console,cvar,fast") {
#if TL_DEV
    SetFixture sf = make_set_fixture(1u);
    char reply[64];
    const Result<u32> r = console_exec(&sf.cs, &sf.wf->w, /*lockstep=*/false, "set cc_plain 99",
                                        Span<char>{ reply, sizeof(reply) });
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_EXPECT_EQ(cvar_get_i32(sf.wf->w.cvars, "cc_plain"_id), 99);
    // Echoed back through cvar_format - the reused formatter, not a duplicate one.
    TL_ASSERT_TRUE(r.value > 0u);
    TL_EXPECT_EQ(strcmp(reply, "99"), 0);
#else
    TL_CONSOLE_CVAR_SKIP;
#endif
}

TL_TEST(console_set_sim_cvar_routes_through_cmd_set_cvar, "editor,console,cvar,fast") {
#if TL_DEV
    SetFixture sf = make_set_fixture(2u);
    TL_ASSERT_EQ(cvar_get_bool(sf.wf->w.cvars, "cc_sim"_id), false);

    char reply[64];
    const Result<u32> r = console_exec(&sf.cs, &sf.wf->w, /*lockstep=*/false, "set cc_sim 1",
                                        Span<char>{ reply, sizeof(reply) });
    TL_ASSERT_EQ(r.err, ERR_OK);
    // NOT applied yet - a SIM cvar's write is a sealed command, only visible after the barrier
    // (world_set_cvar_cmd's own contract, core/world.h) - never a direct write.
    TL_EXPECT_EQ(cvar_get_bool(sf.wf->w.cvars, "cc_sim"_id), false);

    world_flush(&sf.wf->w);
    TL_EXPECT_EQ(cvar_get_bool(sf.wf->w.cvars, "cc_sim"_id), true);
#else
    TL_CONSOLE_CVAR_SKIP;
#endif
}

TL_TEST(console_set_refused_under_lockstep_even_for_a_non_sim_cvar, "editor,console,cvar,fast") {
#if TL_DEV
    // CONSOLE_SIM_AFFECTING is a command-level flag (console_exec checks it before the target
    // cvar's own name is even resolved) - the whole `set` command is refused under lockstep, not
    // only the calls that turn out to target a SIM cvar. This is the coarse, existing mechanism
    // the dispatch named, not a defect: a per-argument lockstep check does not exist anywhere in
    // console_exec, and inventing one for this one command would be new policy, not a fix.
    SetFixture sf = make_set_fixture(3u);
    char reply[64];
    const Result<u32> r = console_exec(&sf.cs, &sf.wf->w, /*lockstep=*/true, "set cc_plain 5",
                                        Span<char>{ reply, sizeof(reply) });
    TL_EXPECT_EQ(r.err, ERR_CONSOLE_LOCKSTEP_REFUSED);
    TL_EXPECT_EQ(cvar_get_i32(sf.wf->w.cvars, "cc_plain"_id), 7);   // default, unchanged
#else
    TL_CONSOLE_CVAR_SKIP;
#endif
}

TL_TEST(console_set_readonly_cvar_refused_both_doors, "editor,console,cvar,fast") {
#if TL_DEV
    SetFixture sf = make_set_fixture(0u);   // slot reuse across tests: safe, see dotpath.test.cpp's note
    char reply[64];
    const Result<u32> r = console_exec(&sf.cs, &sf.wf->w, /*lockstep=*/false, "set cc_ro 1",
                                        Span<char>{ reply, sizeof(reply) });
    TL_EXPECT_EQ(r.err, ERR_CVAR_READONLY);
    TL_EXPECT_EQ(cvar_get_u32(sf.wf->w.cvars, "cc_ro"_id), 42u);   // default, unchanged
#else
    TL_CONSOLE_CVAR_SKIP;
#endif
}

TL_TEST(console_set_unknown_cvar_is_not_found, "editor,console,cvar,fast") {
#if TL_DEV
    SetFixture sf = make_set_fixture(1u);
    char reply[64];
    const Result<u32> r = console_exec(&sf.cs, &sf.wf->w, /*lockstep=*/false, "set cc_nope 1",
                                        Span<char>{ reply, sizeof(reply) });
    TL_EXPECT_EQ(r.err, ERR_CVAR_NOT_FOUND);
#else
    TL_CONSOLE_CVAR_SKIP;
#endif
}

TL_TEST(console_set_malformed_value_is_parse_error, "editor,console,cvar,fast") {
#if TL_DEV
    // Reuses core/cvar.cpp's shared cvar_parse_raw - not a second parser written for this
    // command - so this is exercising the same malformed-input battery cvar.test.cpp already
    // covers, through the console door.
    SetFixture sf = make_set_fixture(2u);
    char reply[64];
    const Result<u32> r = console_exec(&sf.cs, &sf.wf->w, /*lockstep=*/false, "set cc_plain abc",
                                        Span<char>{ reply, sizeof(reply) });
    TL_EXPECT_EQ(r.err, ERR_CVAR_PARSE);
    TL_EXPECT_EQ(cvar_get_i32(sf.wf->w.cvars, "cc_plain"_id), 7);   // default, unchanged
#else
    TL_CONSOLE_CVAR_SKIP;
#endif
}
