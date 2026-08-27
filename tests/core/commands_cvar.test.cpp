// commands_cvar.test.cpp - CMD_SET_CVAR: world_set_cvar_cmd records, apply_commands applies via
// cvar_apply_sim_raw. Added under the same ruled exception as commands.h's CMD_SET_CVAR
// (docs/ROADMAP.md section 0 rule 2, RR-33/RR-35, TODO.md).
// Spec: docs/TOOLING.md §3, docs/ECS.md §4/§10.5. Rubric: docs/TESTING.md §7.
#include "world_test_util.h"
#include "core/cvar.h"

namespace {
TL_CVAR(bool, cc_sim, false, CVAR_SIM, "a sim cvar for the commands integration test");
TL_CVAR(u32, cc_plain, 7u, 0, "a non-sim cvar - CMD_SET_CVAR must refuse this one");

// A dedicated CvarTable per fixture slot, never on the stack (world_test_util.h's own
// WorldFixture precedent: World alone is ~256 KB and a Windows child gets a 1 MB stack).
CvarTable g_test_cvars[4];

WorldFixture& cvar_fixture(u32 slot) {
    WorldFixture& f = *wt_fixture(slot);
    TL_CHECK(world_fixture_init(&f, 5u));
    world_fixture_register_std(&f);
    world_build_schedule(&f.w);
    cvar_table_init(&g_test_cvars[slot]);
    cvar_register(&g_test_cvars[slot], &CVAR_cc_sim);
    cvar_register(&g_test_cvars[slot], &CVAR_cc_plain);
    f.w.cvars = &g_test_cvars[slot];
    return f;
}
}  // namespace

TL_TEST(cmd_set_cvar_records_and_applies, "core,ecs,commands,cvar,fast") {
    WorldFixture& f = cvar_fixture(0u);
    TL_EXPECT_EQ(cvar_get_bool(f.w.cvars, "cc_sim"_id), false);

    world_set_cvar_cmd(&f.w, "cc_sim"_id, 1u);
    TL_EXPECT_EQ(cvar_get_bool(f.w.cvars, "cc_sim"_id), false);   // not applied until the barrier

    world_flush(&f.w);
    TL_EXPECT_EQ(cvar_get_bool(f.w.cvars, "cc_sim"_id), true);
}

TL_TEST(cmd_set_cvar_last_record_wins_within_one_window, "core,ecs,commands,cvar,fast") {
    WorldFixture& f = cvar_fixture(1u);
    world_set_cvar_cmd(&f.w, "cc_sim"_id, 1u);
    world_set_cvar_cmd(&f.w, "cc_sim"_id, 0u);
    world_flush(&f.w);
    TL_EXPECT_EQ(cvar_get_bool(f.w.cvars, "cc_sim"_id), false);
}

TL_TEST_EXPECT_FATAL(cmd_set_cvar_non_sim_cvar_is_fatal, "core,ecs,commands,cvar,fatal") {
    // TL_CHECK (not TL_ASSERT) - compiled in on every tier, no tier-gating needed here.
    ++t->checks;   // the child never returns normally; this just touches `t` (avoids -Wunused-parameter)
    WorldFixture& f = cvar_fixture(2u);
    world_set_cvar_cmd(&f.w, "cc_plain"_id, 99u);   // CVAR_SIM not set - TL_CHECK must fire
}

TL_TEST_EXPECT_FATAL(cmd_set_cvar_unregistered_key_is_fatal, "core,ecs,commands,cvar,fatal") {
    ++t->checks;
    WorldFixture& f = cvar_fixture(3u);
    world_set_cvar_cmd(&f.w, "cc_nope"_id, 0u);   // not registered - TL_CHECK must fire
}
