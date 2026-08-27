// dotpath.test.cpp - the three token forms (name, #index, @singleton), array [k], and the named
// errors (unknown entity/component/field, syntax, ambiguous name).
// Spec: docs/TOOLING.md §9.3.6. Rubric: docs/TESTING.md §7.
//
// TL_TEST's generated signature is `(TestCtx* t)` - every WorldFixture local here is `f`, every
// resolved PathRef is `r`.
//
// editor/dotpath.cpp is compiled ONLY on the debug/dev tiers (src/editor/CMakeLists.txt links an
// empty INTERFACE tl_editor on netcode/ship - docs/TOOLING.md §9.1's file layout table), so -
// matching console.test.cpp/editor.test.cpp's precedent this session - every dotpath_* call
// site, including the shared `dp_fixture()`/`spawn_named()` helpers, is behind `#if TL_DEV`. The
// component declarations (`TL_COMPONENT`) are reflect.h machinery, not editor's, and stay
// unguarded - they compile identically on every tier.
#include "core/world_test_util.h"
#include "editor/dotpath.h"
#include "foundation/interner.h"

#include <stdio.h>   // snprintf - tests/ carries the printf-class io exemption (docs/TESTING.md section 8 R-2)
#include <string.h>

#define TL_FIELDS_Name(X, XA, XH) \
    X(StrId, sid)
TL_COMPONENT(Name)

#define TL_FIELDS_DpHealth(X, XA, XH) \
    X(i32, hp)
TL_COMPONENT(DpHealth)

#define TL_FIELDS_DpFlags(X, XA, XH) \
    XA(i32, bits, 4)
TL_COMPONENT(DpFlags)

#define TL_FIELDS_DpPeerSlots(X, XA, XH) \
    X(i32, local_slot) XA(u8, _pad0, 4)
TL_COMPONENT_FLAGS(DpPeerSlots, COMP_SINGLETON)

#if TL_DEV

namespace {

WorldFixture& dp_fixture(u32 slot) {
    WorldFixture& f = *wt_fixture(slot);
    TL_CHECK(world_fixture_init(&f, 9u));
    world_register_component(&f.w, &Name_info);
    world_register_component(&f.w, &DpHealth_info);
    world_register_component(&f.w, &DpFlags_info);
    world_register_component(&f.w, &DpPeerSlots_info);
    world_build_schedule(&f.w);

    // `api` MUST be static too: VMemArena stores a pointer to it, and chars_arena/meta_arena are
    // static (persist across calls) - a stack-local api would dangle the moment dp_fixture
    // returns, and the next intern() call (from a LATER call, a different stack frame) would
    // read garbage through it. Found as a real segfault (gdb backtrace: arena_push -> intern ->
    // spawn_named), not a hypothetical.
    static VMemApi api;
    static VMemArena chars_arena, meta_arena;
    static Interner in;
    static u8 interner_init_done = 0;
    if (!interner_init_done) {
        api = test_vmem_api();
        TL_CHECK(vmem_arena_init(&chars_arena, "dp.chars"_id, 1024u * 1024u, 0u, &api) == ERR_OK);
        TL_CHECK(vmem_arena_init(&meta_arena, "dp.meta"_id, 1024u * 1024u, 0u, &api) == ERR_OK);
        interner_init(&in, &chars_arena, &meta_arena, 256u);
        interner_init_done = 1;
    }
    f.w.interner = &in;
    return f;
}

Entity spawn_named(World* w, const char* name, i32 hp) {
    Entity e = world_spawn(w);
    DpHealth h{ hp };
    world_add<DpHealth>(w, e, h);
    Name nm{ intern(w->interner, sv(name)) };
    world_add<Name>(w, e, nm);
    DpFlags fl{ { 10, 20, 30, 40 } };
    world_add<DpFlags>(w, e, fl);
    world_flush(w);
    return e;
}

}  // namespace

#endif  // TL_DEV

#define TL_DOTPATH_SKIP TL_SKIP("editor/dotpath.cpp is dev-only (TOOLING.md section 9.1); no symbol in this tier")

TL_TEST(dotpath_resolve_by_name, "editor,dotpath,fast") {
#if TL_DEV
    WorldFixture& f = dp_fixture(0u);
    Entity e = spawn_named(&f.w, "player", 42);

    Result<PathRef> r = dotpath_resolve(&f.w, sv_lit("player.DpHealth.hp"));
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_EXPECT_EQ(r.value.e.bits, e.bits);
    TL_EXPECT_EQ(r.value.elem, (u16)0);

    i32 hp = 0;
    Result<u32> got = dotpath_get_raw(&f.w, r.value, &hp, sizeof(hp));
    TL_ASSERT_EQ(got.err, ERR_OK);
    TL_EXPECT_EQ(got.value, (u32)sizeof(i32));
    TL_EXPECT_EQ(hp, 42);
#else
    TL_DOTPATH_SKIP;
#endif
}

TL_TEST(dotpath_resolve_by_index_current_generation, "editor,dotpath,fast") {
#if TL_DEV
    WorldFixture& f = dp_fixture(1u);
    Entity e = spawn_named(&f.w, "goblin", 7);
    char path[32];
    snprintf(path, sizeof(path), "#%u.DpHealth.hp", handle_index(e));

    Result<PathRef> r = dotpath_resolve(&f.w, sv(path));
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_EXPECT_EQ(r.value.e.bits, e.bits);
#else
    TL_DOTPATH_SKIP;
#endif
}

TL_TEST(dotpath_resolve_singleton, "editor,dotpath,fast") {
#if TL_DEV
    WorldFixture& f = dp_fixture(2u);
    DpPeerSlots ps{ 3, { 0, 0, 0, 0 } };
    world_singleton_set_cmd(&f.w, world_component_id<DpPeerSlots>(&f.w), &ps);
    world_flush(&f.w);

    Result<PathRef> r = dotpath_resolve(&f.w, sv_lit("@DpPeerSlots.local_slot"));
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_EXPECT_TRUE(handle_is_null(r.value.e));

    i32 slot = 0;
    Result<u32> got = dotpath_get_raw(&f.w, r.value, &slot, sizeof(slot));
    TL_ASSERT_EQ(got.err, ERR_OK);
    TL_EXPECT_EQ(slot, 3);
#else
    TL_DOTPATH_SKIP;
#endif
}

TL_TEST(dotpath_resolve_array_element, "editor,dotpath,fast") {
#if TL_DEV
    WorldFixture& f = dp_fixture(3u);
    spawn_named(&f.w, "a", 1);

    Result<PathRef> r = dotpath_resolve(&f.w, sv_lit("a.DpFlags.bits[3]"));
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_EXPECT_EQ(r.value.elem, (u16)3);

    i32 v = 0;
    Result<u32> got = dotpath_get_raw(&f.w, r.value, &v, sizeof(v));
    TL_ASSERT_EQ(got.err, ERR_OK);
    TL_EXPECT_EQ(v, 40);   // bits = {10,20,30,40}, index 3
#else
    TL_DOTPATH_SKIP;
#endif
}

TL_TEST(dotpath_resolve_named_errors, "editor,dotpath,fast") {
#if TL_DEV
    // wt_fixture only carries 4 slots (world_test_util.h); slots are safely reused across
    // DIFFERENT tests because a non-isolated run executes tests sequentially in one process
    // (world_fixture_init re-zeroes the slot each call) and an --isolate run gives every test
    // its own process/address space either way.
    WorldFixture& f = dp_fixture(0u);
    spawn_named(&f.w, "a", 1);
    spawn_named(&f.w, "a", 2);   // duplicate name -> ambiguous
    spawn_named(&f.w, "solo", 3);   // uniquely-named, for the component/field error rows below

    TL_EXPECT_EQ(dotpath_resolve(&f.w, sv_lit("nope.DpHealth.hp")).err, ERR_PATH_NO_ENTITY);
    TL_EXPECT_EQ(dotpath_resolve(&f.w, sv_lit("a.DpHealth.hp")).err, ERR_PATH_NO_ENTITY);   // ambiguous
    TL_EXPECT_EQ(dotpath_resolve(&f.w, sv_lit("#99999.DpHealth.hp")).err, ERR_PATH_NO_ENTITY);
    TL_EXPECT_EQ(dotpath_resolve(&f.w, sv_lit("#x.DpHealth.hp")).err, ERR_PATH_SYNTAX);
    TL_EXPECT_EQ(dotpath_resolve(&f.w, sv_lit("solo.Nope.hp")).err, ERR_PATH_NO_COMPONENT);
    TL_EXPECT_EQ(dotpath_resolve(&f.w, sv_lit("solo.DpHealth.nope")).err, ERR_PATH_NO_FIELD);
    TL_EXPECT_EQ(dotpath_resolve(&f.w, sv_lit("solo.DpFlags.bits[99]")).err, ERR_PATH_NO_FIELD);
    TL_EXPECT_EQ(dotpath_resolve(&f.w, sv_lit("just.two")).err, ERR_PATH_SYNTAX);
    TL_EXPECT_EQ(dotpath_resolve(&f.w, sv_lit("@DpPeerSlots.local_slot.extra")).err, ERR_PATH_SYNTAX);
    TL_EXPECT_EQ(dotpath_resolve(&f.w, sv_lit("@Nope.field")).err, ERR_PATH_NO_COMPONENT);
    TL_EXPECT_EQ(dotpath_resolve(&f.w, sv_lit("@DpHealth.hp")).err, ERR_PATH_NO_COMPONENT);   // not a singleton
#else
    TL_DOTPATH_SKIP;
#endif
}

TL_TEST(dotpath_set_raw_records_command_and_refuses_lockstep, "editor,dotpath,fast") {
#if TL_DEV
    WorldFixture& f = dp_fixture(1u);
    Entity e = spawn_named(&f.w, "a", 1);
    Result<PathRef> r = dotpath_resolve(&f.w, sv_lit("a.DpHealth.hp"));
    TL_ASSERT_EQ(r.err, ERR_OK);

    i32 nv = 55;
    TL_EXPECT_EQ(dotpath_set_raw(&f.w, r.value, true, &nv, sizeof(nv)), ERR_PATH_LOCKSTEP);
    TL_ASSERT_EQ(dotpath_set_raw(&f.w, r.value, false, &nv, sizeof(nv)), ERR_OK);
    world_flush(&f.w);

    i32 hp = 0;
    Result<u32> got = dotpath_get_raw(&f.w, r.value, &hp, sizeof(hp));
    TL_ASSERT_EQ(got.err, ERR_OK);
    TL_EXPECT_EQ(hp, 55);
    (void)e;
#else
    TL_DOTPATH_SKIP;
#endif
}
