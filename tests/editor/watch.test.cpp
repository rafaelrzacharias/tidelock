// watch.test.cpp - bind once, read through the cached tuple, re-resolve only on a stale handle.
// Spec: docs/TOOLING.md §3, §9.3.6. Rubric: docs/TESTING.md §7.
//
// TL_TEST's generated signature is `(TestCtx* t)` - every WorldFixture local here is `f`, every
// Watch local is `w`.
//
// editor/watch.cpp (and dotpath.cpp underneath it) is compiled ONLY on the debug/dev tiers
// (src/editor/CMakeLists.txt links an empty INTERFACE tl_editor on netcode/ship), so - matching
// this session's console.test.cpp/editor.test.cpp/dotpath.test.cpp precedent - every watch_*
// call site is behind `#if TL_DEV`.
#include "core/world_test_util.h"
#include "editor/watch.h"
#include "foundation/interner.h"

#include <string.h>

#define TL_FIELDS_Name(X, XA, XH) \
    X(StrId, sid)
TL_COMPONENT(Name)

#define TL_FIELDS_WHealth(X, XA, XH) \
    X(i32, hp)
TL_COMPONENT(WHealth)

#if TL_DEV

namespace {

WorldFixture& w_fixture(u32 slot) {
    WorldFixture& f = *wt_fixture(slot);
    TL_CHECK(world_fixture_init(&f, 11u));
    world_register_component(&f.w, &Name_info);
    world_register_component(&f.w, &WHealth_info);
    world_build_schedule(&f.w);

    static VMemApi api;
    static VMemArena chars_arena, meta_arena;
    static Interner in;
    static u8 done = 0;
    if (!done) {
        api = test_vmem_api();
        TL_CHECK(vmem_arena_init(&chars_arena, "wt.chars"_id, 1024u * 1024u, 0u, &api) == ERR_OK);
        TL_CHECK(vmem_arena_init(&meta_arena, "wt.meta"_id, 1024u * 1024u, 0u, &api) == ERR_OK);
        interner_init(&in, &chars_arena, &meta_arena, 256u);
        done = 1;
    }
    f.w.interner = &in;
    return f;
}

Entity spawn_named(World* w, const char* name, i32 hp) {
    Entity e = world_spawn(w);
    WHealth h{ hp };
    world_add<WHealth>(w, e, h);
    Name nm{ intern(w->interner, sv(name)) };
    world_add<Name>(w, e, nm);
    world_flush(w);
    return e;
}

}  // namespace

#endif  // TL_DEV

#define TL_WATCH_SKIP TL_SKIP("editor/watch.cpp is dev-only (TOOLING.md section 9.1); no symbol in this tier")

TL_TEST(watch_init_and_refresh_reads_current_value, "editor,watch,fast") {
#if TL_DEV
    WorldFixture& f = w_fixture(2u);
    spawn_named(&f.w, "player", 10);

    Watch w;
    TL_ASSERT_EQ(watch_init(&w, &f.w, sv_lit("player.WHealth.hp")), ERR_OK);
    TL_EXPECT_EQ(w.resolved, (u8)1);

    watch_refresh(&w, &f.w);
    TL_ASSERT_EQ(w.ok, (u8)1);
    i32 hp;
    memcpy(&hp, w.last_value, sizeof(hp));
    TL_EXPECT_EQ(hp, 10);
#else
    TL_WATCH_SKIP;
#endif
}

TL_TEST(watch_init_bad_path_reports_error, "editor,watch,fast") {
#if TL_DEV
    WorldFixture& f = w_fixture(3u);
    Watch w;
    TL_EXPECT_EQ(watch_init(&w, &f.w, sv_lit("nope.WHealth.hp")), ERR_PATH_NO_ENTITY);
    TL_EXPECT_EQ(w.resolved, (u8)0);
    watch_refresh(&w, &f.w);
    TL_EXPECT_EQ(w.ok, (u8)0);
#else
    TL_WATCH_SKIP;
#endif
}

TL_TEST(watch_refresh_re_resolves_after_destroy_and_respawn, "editor,watch,fast") {
#if TL_DEV
    WorldFixture& f = w_fixture(0u);
    Entity e1 = spawn_named(&f.w, "goblin", 5);

    Watch w;
    TL_ASSERT_EQ(watch_init(&w, &f.w, sv_lit("goblin.WHealth.hp")), ERR_OK);
    watch_refresh(&w, &f.w);
    TL_ASSERT_EQ(w.ok, (u8)1);
    i32 hp;
    memcpy(&hp, w.last_value, sizeof(hp));
    TL_EXPECT_EQ(hp, 5);

    // Destroy and respawn a DIFFERENT entity under the same name - the old PathRef.e now names a
    // dead/stale slot (or a resurrected one with a different generation), so a refresh must fail
    // to read through the STALE tuple and re-resolve via the path string instead.
    world_destroy(&f.w, e1);
    world_flush(&f.w);
    spawn_named(&f.w, "goblin", 99);

    watch_refresh(&w, &f.w);
    TL_ASSERT_EQ(w.ok, (u8)1);
    memcpy(&hp, w.last_value, sizeof(hp));
    TL_EXPECT_EQ(hp, 99);
#else
    TL_WATCH_SKIP;
#endif
}
