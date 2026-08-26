#pragma once
// ---------------------------------------------------------------------------------------------
// handles.h - the closed tagged-lightuserdata table and its push/check helpers.
//
// Spec: docs/LUAU-LAYER.md §10.4 (the tag table, verbatim), §3 ("handles are tagged
//   lightuserdata"). docs/CANON.md "Types" owns the handle widths themselves.
// Purpose: a handle crosses into Luau as a POINTER-SIZED tagged value: zero allocation, typed
//   (an Entity is not a Body), `nil` for null, and `==` in Luau is handle equality because Luau
//   compares pointer AND tag. Plain integers were rejected (untyped) and full userdata rejected
//   (allocates) - docs/LUAU-LAYER.md §3.
// Invariants: the tag numbers below are THE closed list; a new domain is a doc change first.
//   bits == 0 is the null handle and always pushes nil, in every domain (docs/CANON.md: a
//   Handle's bits == 0 is null and generation 0 is never issued, so 0 can never be a live
//   handle). Every tag is < LUA_LUTAG_LIMIT, asserted at compile time.
// Determinism: a handle's BITS are state; its address is not (there is no address - the bits ARE
//   the pointer value). Nothing here hashes, allocates or orders.
// Threading: none - pure stack operations on the caller's lua_State.
// Includes: <lua.h>, <lualib.h> (src/script only), foundation/tl_types.h.
//
// The per-domain typed wrappers (push_entity, check_body, ...) arrive with the domains
// themselves in the W3 luau-bindings lane; the tag numbers and the generic helpers are here
// because docs/LUAU-LAYER.md §10.12 step 1 puts handles.h beside vm.cpp, and because the tag
// numbers are a CROSS-LANE contract - W3 must not be free to renumber them.
// ---------------------------------------------------------------------------------------------
#include <lua.h>
#include <lualib.h>

#include "foundation/tl_types.h"

// docs/LUAU-LAYER.md §10.4, the closed list. Gaps are intentional: 11..15 are reserved so the
// resource block (7..10) can grow without renumbering the domain block below it.
enum ScriptTag : i32 {
    TAG_ENTITY     = 1,    // Entity.bits (u32)
    TAG_BODY       = 2,    // Alloy handle bits (docs/ALLOY.md §1.1)
    TAG_CONSTRAINT = 3,
    TAG_PLANT      = 4,
    TAG_CAVITY     = 5,
    TAG_BASIN      = 6,
    TAG_TEX        = 7,    // Handle<_,12,4>.bits
    TAG_FONT       = 8,
    TAG_AUDIO      = 9,
    TAG_DATATABLE  = 10,
    TAG_COMPONENT  = 16,   // ComponentId + 1: id 0 is valid, so 0 must stay null
    TAG_EVENTTYPE  = 17,   // index + 1 into the world's event table
    TAG_ACTION     = 18,   // ActionId + 1
};

// The userdata tags of docs/LUAU-LAYER.md §10.5. Separate namespace from ScriptTag: Luau keys
// lightuserdata and full-userdata metatables on independent tag spaces. The proxies themselves
// are the W3 lane's; the numbers are pinned here for the same cross-lane reason.
enum ScriptUserdataTag : i32 {
    UTAG_PROXY = 1,        // component proxy (World*, ComponentId, dense, tick)
    UTAG_EVENT = 2,        // event read-buffer row
    UTAG_ROW   = 3,        // data-table row
    UTAG_INPUT = 4,        // InputFrame view
};

static_assert(TAG_ACTION < LUA_LUTAG_LIMIT, "docs/LUAU-LAYER.md section 10.4: every tag < LUA_LUTAG_LIMIT");
static_assert(UTAG_INPUT < LUA_UTAG_LIMIT, "docs/LUAU-LAYER.md section 10.5: every userdata tag < LUA_UTAG_LIMIT");

// The domain name registered with lua_setlightuserdataname, which is what luaL_typeerror prints
// when a script passes a Body where an Entity was wanted. One row per ScriptTag, terminated by a
// null name; iterated once at VM creation.
struct ScriptTagName {
    i32         tag;
    const char* name;
};

// The closed name table, in tag order. `inline constexpr` so every TU sees one object with no
// namespace-scope mutable storage (docs/CPP-SUBSET.md §1).
inline constexpr ScriptTagName SCRIPT_TAG_NAMES[] = {
    { TAG_ENTITY, "Entity" },      { TAG_BODY, "Body" },        { TAG_CONSTRAINT, "Constraint" },
    { TAG_PLANT, "Plant" },        { TAG_CAVITY, "Cavity" },    { TAG_BASIN, "Basin" },
    { TAG_TEX, "Tex" },            { TAG_FONT, "Font" },        { TAG_AUDIO, "Audio" },
    { TAG_DATATABLE, "DataTable" },{ TAG_COMPONENT, "Component" },
    { TAG_EVENTTYPE, "EventType" },{ TAG_ACTION, "Action" },    { 0, nullptr },
};

// Registers every domain name on `L`. Called once per VM, before any binding table is built, so
// a type error raised during init already prints the domain name.
inline void script_register_tag_names(lua_State* L) {
    for (const ScriptTagName* r = SCRIPT_TAG_NAMES; r->name != nullptr; ++r) {
        lua_setlightuserdataname(L, r->tag, r->name);
    }
}

// Pushes `bits` as tagged lightuserdata, or nil when bits == 0 (the null handle, docs/CANON.md).
// Never allocates. The caller names the tag; there is no deduction, on purpose.
inline void script_push_handle(lua_State* L, u64 bits, i32 tag) {
    if (bits == 0) {
        lua_pushnil(L);
        return;
    }
    lua_pushlightuserdatatagged(L, (void*)(uintptr_t)bits, tag);
}

// Reads a handle of `tag` at `idx`: nil reads as the null handle (0), a value of the right tag
// reads as its bits, and anything else raises luaL_typeerror naming the domain. `nullable` is
// the binding's own contract - when false, nil is refused with the same type error rather than
// silently becoming handle 0, which is how a null slipped into a spawn in the Ore program.
inline u64 script_check_handle(lua_State* L, int idx, i32 tag, bool nullable) {
    const char* name = "handle";
    for (const ScriptTagName* r = SCRIPT_TAG_NAMES; r->name != nullptr; ++r) {
        if (r->tag == tag) { name = r->name; break; }
    }
    if (lua_isnoneornil(L, idx)) {
        if (nullable) return 0;
        luaL_typeerror(L, idx, name);
    }
    void* p = lua_tolightuserdatatagged(L, idx, tag);
    if (p == nullptr) {
        luaL_typeerror(L, idx, name);
    }
    return (u64)(uintptr_t)p;
}
