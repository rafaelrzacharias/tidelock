// luacomp.cpp - the runtime packer against C++ offsetof mirrors, pad emission, the default
// row, fingerprint parity with a C++ twin, end-to-end column use, and every reject code.
// Spec: docs/ECS.md §6.1/§10.7, §10.8 (luacomp.test.cpp line); docs/LUAU-LAYER.md §10.6.
#include "world_test_util.h"

// The C++ mirror of the docs/ECS.md §6.1 example declaration.
#define TL_FIELDS_LMirrorA(X, XA, XH) \
    X(i32, hp) X(i32, hp_max) XH(Entity, last_attacker) XA(u8, flags, 4)
TL_COMPONENT(LMirrorA)

// A mirror whose layout needs one interior and one tail pad, spelled explicitly per the rules.
#define TL_FIELDS_LMirrorB(X, XA, XH) \
    X(u8, a) XA(u8, _pad0, 3) X(u32, b) X(u16, c) XA(u8, _pad1, 2)
TL_COMPONENT(LMirrorB)

namespace {

// The Luau-side declaration of LMirrorA's fields (what ecs.component would hand the packer).
const LuauFieldDecl LA_DECL[4] = {
    { sv("hp"), K_i32, 0, 1, 0, 0u },
    { sv("hp_max"), K_i32, 0, 1, 0, 0u },
    { sv("last_attacker"), K_Entity, 0, 1, 0, 0u },
    { sv("flags"), K_u8, 0, 4, 0, 0u },
};

// LMirrorB WITHOUT its pads - the packer must synthesize _pad0/_pad1 at the same offsets.
const LuauFieldDecl LB_DECL[3] = {
    { sv("a"), K_u8, 0, 1, 0, 0u },
    { sv("b"), K_u32, 0, 1, 0, 0u },
    { sv("c"), K_u16, 0, 1, 0, 0u },
};

// Field-for-field table comparison (offset/size/kind/count/name_hash).
bool lc_tables_equal(TestCtx* t, const ComponentInfo* lua, const ComponentInfo* cpp) {
    TL_EXPECT_EQ(lua->size, cpp->size);
    TL_EXPECT_EQ(lua->align, cpp->align);
    TL_EXPECT_EQ(lua->field_count, cpp->field_count);
    if (lua->field_count != cpp->field_count) { return false; }
    bool ok = true;
    for (u32 i = 0; i < cpp->field_count; ++i) {
        TL_EXPECT_EQ(lua->fields[i].offset, cpp->fields[i].offset);
        TL_EXPECT_EQ(lua->fields[i].size, cpp->fields[i].size);
        TL_EXPECT_EQ(lua->fields[i].kind, cpp->fields[i].kind);
        TL_EXPECT_EQ(lua->fields[i].count, cpp->fields[i].count);
        TL_EXPECT_EQ(lua->fields[i].name_hash, cpp->fields[i].name_hash);
        ok = ok && lua->fields[i].offset == cpp->fields[i].offset
                && lua->fields[i].name_hash == cpp->fields[i].name_hash;
    }
    return ok;
}

}  // namespace

TL_TEST(luacomp_packer_layout_matches_cpp_offsetof, "core,ecs,luacomp,smoke,fast") {
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 1u));
    // Register under fresh names so the C++ infos stay comparison references, not collisions.
    Result<ComponentId> ra = world_register_component_luau(&f.w, sv("LuaA"), LA_DECL, 4u, 0u);
    TL_ASSERT_EQ(ra.err, ERR_OK);
    Result<ComponentId> rb = world_register_component_luau(&f.w, sv("LuaB"), LB_DECL, 3u, 0u);
    TL_ASSERT_EQ(rb.err, ERR_OK);
    const ComponentInfo* la = f.w.comps[ra.value].info;
    const ComponentInfo* lb = f.w.comps[rb.value].info;

    // Field names differ from the mirror only in the COMPONENT name; every field row must
    // agree with the compiler's own offsetof/sizeof through the C++ table.
    TL_EXPECT_TRUE(lc_tables_equal(t, la, &LMirrorA_info));
    TL_EXPECT_TRUE(lc_tables_equal(t, lb, &LMirrorB_info));
    // The synthesized pads carry the canonical names.
    TL_EXPECT_EQ(lb->fields[1].name_hash, "_pad0"_id);
    TL_EXPECT_EQ(lb->fields[4].name_hash, "_pad1"_id);
    TL_EXPECT_TRUE(tl_field_is_pad(&lb->fields[1]));
}

TL_TEST(luacomp_fingerprint_parity_with_a_cpp_twin, "core,ecs,luacomp,determinism,fast") {
    // docs/ECS.md §6.1: "same fingerprint contribution". A world that registers the C++
    // LMirrorA and one that declares the SAME name + fields from Luau must produce identical
    // reflection hashes.
    WorldFixture& cpp_w = *wt_fixture(0u);
    WorldFixture& lua_w = *wt_fixture(1u);
    TL_ASSERT_TRUE(world_fixture_init(&cpp_w, 1u));
    TL_ASSERT_TRUE(world_fixture_init(&lua_w, 2u));
    world_register_component(&cpp_w.w, &LMirrorA_info);
    Result<ComponentId> r = world_register_component_luau(&lua_w.w, sv("LMirrorA"), LA_DECL, 4u, 0u);
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_EXPECT_EQ(world_reflection_hash(&cpp_w.w), world_reflection_hash(&lua_w.w));
}

TL_TEST(luacomp_default_row_and_live_column, "core,ecs,luacomp,fast") {
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 1u));
    const LuauFieldDecl decl[3] = {
        { sv("hp"), K_i32, 0, 1, 0, 100u },            // default = raw bits (docs/LUAU-LAYER.md §10.6)
        { sv("tag"), K_u16, 0, 1, 0, 0x1234u },
        { sv("mask"), K_u8, 0, 4, 0, 0xABu },          // broadcast to every array element
    };
    Result<ComponentId> r = world_register_component_luau(&f.w, sv("LuaHealth"), decl, 3u, 0u);
    TL_ASSERT_EQ(r.err, ERR_OK);
    const ComponentInfo* info = f.w.comps[r.value].info;
    TL_ASSERT_NOT_NULL(info->default_row);
    const u8* def = (const u8*)info->default_row;
    i32 hp;
    memcpy(&hp, def + info->fields[0].offset, 4u);
    TL_EXPECT_EQ(hp, 100);
    u16 tag;
    memcpy(&tag, def + info->fields[1].offset, 2u);
    TL_EXPECT_EQ(tag, 0x1234u);
    for (u32 e = 0; e < 4u; ++e) { TL_EXPECT_EQ(def[info->fields[2].offset + e], 0xABu); }

    // Layout: i32(0) + u16(4) + u8[4](6) -> offset 10, tail pad 2 -> size 12, align 4.
    TL_ASSERT_EQ(info->size, 12u);
    TL_EXPECT_EQ(info->align, 4u);
    TL_EXPECT_EQ(info->field_count, 4u);   // three fields + the synthesized tail pad
    TL_EXPECT_TRUE(tl_field_is_pad(&info->fields[3]));

    // The registered column is a first-class citizen: spawn, add a raw row, read it back.
    world_build_schedule(&f.w);
    Entity ent = world_spawn(&f.w);
    u8 row[12];
    memcpy(row, info->default_row, info->size);
    world_add_raw(&f.w, ent, r.value, row);
    world_flush(&f.w);
    const void* got = column_get(&f.w.comps[r.value], ent);
    TL_ASSERT_NOT_NULL(got);
    TL_EXPECT_TRUE(memcmp(got, row, info->size) == 0);
}

TL_TEST(luacomp_every_reject_code_and_no_meta_residue, "core,ecs,luacomp,edge,fast") {
    WorldFixture& f = *wt_fixture(0u);
    TL_ASSERT_TRUE(world_fixture_init(&f, 1u));
    world_register_component(&f.w, &LMirrorA_info);
    const u64 mark = arena_mark(&f.w.meta);

    // Duplicate component name (against the C++ registration).
    Result<ComponentId> r = world_register_component_luau(&f.w, sv("LMirrorA"), LA_DECL, 4u, 0u);
    TL_EXPECT_EQ(r.err, ERR_ECS_DUPLICATE_NAME);

    // Bad kind / bad count / zero fields / duplicate field names.
    LuauFieldDecl bad = { sv("x"), K_COUNT, 0, 1, 0, 0u };
    r = world_register_component_luau(&f.w, sv("L1"), &bad, 1u, 0u);
    TL_EXPECT_EQ(r.err, ERR_ECS_BAD_KIND);
    bad = LuauFieldDecl{ sv("x"), K_i32, 0, 0, 0, 0u };
    r = world_register_component_luau(&f.w, sv("L2"), &bad, 1u, 0u);
    TL_EXPECT_EQ(r.err, ERR_ECS_BAD_COUNT);
    bad = LuauFieldDecl{ sv("x"), K_i32, 0, 256, 0, 0u };
    r = world_register_component_luau(&f.w, sv("L3"), &bad, 1u, 0u);
    TL_EXPECT_EQ(r.err, ERR_ECS_BAD_COUNT);
    r = world_register_component_luau(&f.w, sv("L4"), LA_DECL, 0u, 0u);
    TL_EXPECT_EQ(r.err, ERR_ECS_TABLE_FULL);
    const LuauFieldDecl dup2[2] = {
        { sv("same"), K_i32, 0, 1, 0, 0u },
        { sv("same"), K_u32, 0, 1, 0, 0u },
    };
    r = world_register_component_luau(&f.w, sv("L5"), dup2, 2u, 0u);
    TL_EXPECT_EQ(r.err, ERR_ECS_DUPLICATE_NAME);

    // A rejected declaration leaves no meta residue (the arena mark is restored).
    TL_EXPECT_EQ(arena_mark(&f.w.meta), mark);

    // The event twin: register once, then every duplicate/seal reject.
    Result<EventTypeId> ev = world_register_event_luau(&f.w, sv("LuaEv"), LA_DECL, 4u, 8u);
    TL_ASSERT_EQ(ev.err, ERR_OK);
    TL_EXPECT_EQ(ev.value, "LuaEv"_id);
    TL_EXPECT_NE(world_find_event(&f.w, "LuaEv"_id), (u32)MAX_EVENT_TYPES);
    ev = world_register_event_luau(&f.w, sv("LuaEv"), LA_DECL, 4u, 8u);
    TL_EXPECT_EQ(ev.err, ERR_ECS_DUPLICATE_NAME);

    world_build_schedule(&f.w);
    r = world_register_component_luau(&f.w, sv("L6"), LA_DECL, 4u, 0u);
    TL_EXPECT_EQ(r.err, ERR_ECS_SEALED);
    ev = world_register_event_luau(&f.w, sv("LuaEv2"), LA_DECL, 4u, 8u);
    TL_EXPECT_EQ(ev.err, ERR_ECS_SEALED);
}
