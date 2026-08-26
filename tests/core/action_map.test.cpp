// action_map.test.cpp - registration/lookup, and the fingerprint invariant (docs/INPUT.md §2:
// "names + kinds + classes... hashes into the build fingerprint"; bindings do not).
// Spec: docs/INPUT.md §2, §7 ("fingerprint changes when the action list changes").
#include "runner/tl_test.h"
#include "core/action_map.h"
#include "foundation/vmem_test_api.h"

namespace {
struct AmFixture { VMemApi api; VMemArena arena; ActionMap map; };
bool am_fixture_init(AmFixture* f, u32 max_bindings) {
    f->api = test_vmem_api();
    if (vmem_arena_init(&f->arena, "am.test"_id, 1024u * 1024u, 0u, &f->api) != ERR_OK) { return false; }
    action_map_init(&f->map, &f->arena, max_bindings);
    return true;
}
}  // namespace

TL_TEST(action_register_and_find, "core,input,action_map,fast") {
    AmFixture f;
    TL_ASSERT_TRUE(am_fixture_init(&f, 16u));
    const ActionId jump = action_register(&f.map, "jump"_id, 0u, ACT_DIGITAL, CLS_EDGE);
    const ActionId move_x = action_register(&f.map, "move_x"_id, 0u, ACT_ANALOG, CLS_AXIS);
    TL_EXPECT_EQ(jump, (ActionId)0);
    TL_EXPECT_EQ(move_x, (ActionId)1);
    TL_EXPECT_EQ(action_find(&f.map, "jump"_id), jump);
    TL_EXPECT_EQ(action_find(&f.map, "move_x"_id), move_x);
    TL_EXPECT_EQ(action_find(&f.map, "nope"_id), (ActionId)ACTION_ID_NONE);
}

TL_TEST(action_bind_stores_binding, "core,input,action_map,fast") {
    AmFixture f;
    TL_ASSERT_TRUE(am_fixture_init(&f, 16u));
    const ActionId jump = action_register(&f.map, "jump"_id, 0u, ACT_DIGITAL, CLS_EDGE);
    Binding b{};
    b.action = jump; b.dev = DEV_KEY; b.code_pos = 44u; b.context = CONTEXT_DEFAULT;
    action_bind(&f.map, b);
    TL_ASSERT_EQ(f.map.bindings.count, 1u);
    TL_EXPECT_EQ(array_at(&f.map.bindings, 0u).action, jump);
}

TL_TEST(fingerprint_changes_when_actions_change, "core,input,action_map,fingerprint,fast") {
    AmFixture a, b;
    TL_ASSERT_TRUE(am_fixture_init(&a, 4u));
    TL_ASSERT_TRUE(am_fixture_init(&b, 4u));
    action_register(&a.map, "jump"_id, 0u, ACT_DIGITAL, CLS_EDGE);
    action_register(&b.map, "jump"_id, 0u, ACT_DIGITAL, CLS_EDGE);
    TL_EXPECT_EQ(action_map_fingerprint(&a.map), action_map_fingerprint(&b.map));

    action_register(&b.map, "crouch"_id, 0u, ACT_DIGITAL, CLS_EDGE);
    TL_EXPECT_NE(action_map_fingerprint(&a.map), action_map_fingerprint(&b.map));
}

TL_TEST(fingerprint_ignores_bindings, "core,input,action_map,fingerprint,fast") {
    AmFixture a, b;
    TL_ASSERT_TRUE(am_fixture_init(&a, 4u));
    TL_ASSERT_TRUE(am_fixture_init(&b, 4u));
    const ActionId ja = action_register(&a.map, "jump"_id, 0u, ACT_DIGITAL, CLS_EDGE);
    const ActionId jb = action_register(&b.map, "jump"_id, 0u, ACT_DIGITAL, CLS_EDGE);
    const u64 before = action_map_fingerprint(&b.map);

    Binding bind{}; bind.action = jb; bind.dev = DEV_KEY; bind.code_pos = 44u;
    action_bind(&b.map, bind);
    (void)ja;
    TL_EXPECT_EQ(action_map_fingerprint(&b.map), before);
    TL_EXPECT_EQ(action_map_fingerprint(&a.map), action_map_fingerprint(&b.map));
}

TL_TEST(context_default_and_set, "core,input,action_map,fast") {
    AmFixture f;
    TL_ASSERT_TRUE(am_fixture_init(&f, 4u));
    TL_EXPECT_EQ(f.map.active_context, (u8)CONTEXT_DEFAULT);
    action_map_set_context(&f.map, 3u);
    TL_EXPECT_EQ(f.map.active_context, (u8)3u);
}
