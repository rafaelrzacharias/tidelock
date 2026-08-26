// sandbox.cpp - the library whitelist, the removal list, the deterministic replacements,
// sortedpairs, and the freeze. Contract: script/vm.h.
// Spec: docs/LUAU-LAYER.md §10.2 steps 3-5 and 11, §10.2.1 (sortedpairs), §1.1 (why pairs is
//   gone); docs/CANON.md "Luau sim VM - the exact removal list" is the HOME of that list and
//   this file is written from it.
#include <lua.h>
#include <lualib.h>

#include <string.h>

#include "foundation/tl_assert.h"
#include "script/vm.h"

namespace {

// docs/CANON.md "Luau sim VM": the closed removal list, in the order docs/LUAU-LAYER.md §10.2
// step 4 spells it, followed by the three raw-access functions CANON names and §10.2 step 4 did
// not (CANON is the sheet every doc must agree with, so the code follows CANON and §10.2 is
// corrected in the same commit). A dotted name means "the field of that table".
//   - `io` and `debug` are never opened by the sim VM's library set at all; they are removed
//     anyway, and the sandbox test asserts them absent, so a future library-set change cannot
//     let one in silently. (Luau has no luaopen_io whatsoever - measured against the 0.696 pin.)
//   - `require` is not here: it is installed for the init phase and removed at seal (§10.9), and
//     this lane does not install it.
const char* const SIM_REMOVE[] = {
    "math", "os", "io", "debug", "pairs", "next", "coroutine", "loadstring", "collectgarbage",
    "gcinfo", "getfenv", "setfenv", "newproxy", "print",
    "rawequal", "rawget", "rawset",                        // docs/CANON.md: the proxies forbid raw access
    "string.rep",                                          // an unbounded allocation bomb (§9 R-2)
    "table.foreach", "table.foreachi",                     // they call next
    nullptr,
};

// docs/LUAU-LAYER.md §10.2 step 4, last sentence. `os` is additionally never opened (see
// open_libraries); it is listed so the assertion covers it either way.
//
// `math.random`/`math.randomseed` join the list by RULING (2026-08-26, Rafael - review round 1's
// D4). Luau seeds its PCG from `uintptr_t(L) ^ time(NULL) ^ clock()` (lmathlib.cpp), and the data
// VM's OUTPUT IS HASHED (docs/LUAU-LAYER.md §1) - so a data script that draws once produces a
// peer-divergent table, and the divergence surfaces as a fingerprint mismatch on handshake rather
// than as an error where the mistake is. A data table wanting randomness is a bug, and it should
// fail at the call. The rest of `math` stays: it is pure and its output is a function of its
// input. The `time()`/`clock()` reads inside luaopen_math's seeding are inert once `random` is
// unreachable - they touch only `rngstate`, which nothing can then read.
const char* const DATA_REMOVE[] = {
    "os", "io", "loadstring", "getfenv", "setfenv",
    "math.random", "math.randomseed",
    nullptr,
};

// Sets `path` to nil, where `path` is either a global name or `table.field`. Returns false if a
// dotted path names a global that is not a table - which would mean the removal silently did
// nothing, the exact failure mode docs/LESSONS.md calls a phantom gate.
bool remove_name(lua_State* L, const char* path) {
    const char* dot = strchr(path, '.');
    if (dot == nullptr) {
        lua_pushnil(L);
        lua_setglobal(L, path);
        return true;
    }
    char head[32];
    const u32 n = (u32)(dot - path);
    if (n + 1u >= (u32)sizeof(head)) return false;
    memcpy(head, path, (size_t)n);
    head[n] = 0;
    lua_getglobal(L, head);
    if (lua_isnil(L, -1)) {                    // the table itself is gone: nothing to remove
        lua_pop(L, 1);
        return true;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    lua_pushnil(L);
    lua_setfield(L, -2, dot + 1);
    lua_pop(L, 1);
    return true;
}

// True iff `path` currently reads as nil. The verification half of the removal: docs/LUAU-LAYER.md
// §10.2 step 4 says "each asserted absent afterwards", and this is that assertion, in the module
// rather than only in the test, so a mistake fails VM creation instead of shipping.
bool name_is_absent(lua_State* L, const char* path) {
    const char* dot = strchr(path, '.');
    if (dot == nullptr) {
        lua_getglobal(L, path);
        const bool absent = lua_isnil(L, -1);
        lua_pop(L, 1);
        return absent;
    }
    char head[32];
    const u32 n = (u32)(dot - path);
    if (n + 1u >= (u32)sizeof(head)) return false;
    memcpy(head, path, (size_t)n);
    head[n] = 0;
    lua_getglobal(L, head);
    if (!lua_istable(L, -1)) {
        const bool absent = lua_isnil(L, -1);
        lua_pop(L, 1);
        return absent;
    }
    lua_getfield(L, -1, dot + 1);
    const bool absent = lua_isnil(L, -1);
    lua_pop(L, 2);
    return absent;
}

// One luaopen_* call, the way luaL_openlibs makes it: the function is called with the library's
// global name as its argument, which is what registers the table under that name.
void open_lib(lua_State* L, lua_CFunction fn, const char* name) {
    lua_pushcfunction(L, fn, name);
    lua_pushstring(L, name);
    lua_call(L, 1, 0);
}

// docs/LUAU-LAYER.md §10.2 step 3. The sets are spelled out per kind rather than reached through
// luaL_openlibs-minus-something, so the opened set is readable here and cannot drift when
// upstream adds a library.
//
// One correction to the doc, measured against the 0.696 pin: §10.2 step 3 claims "luaL_openlibs
// opens neither os nor io" - Luau's linit.cpp DOES open `os` (there is no io library at all), so
// the data VM opens its list explicitly and simply never opens os. §10.2 is corrected in the
// same commit.
void open_libraries(lua_State* L, ScriptVmKind kind) {
    open_lib(L, luaopen_base, "");
    if (kind == SCRIPT_VM_SIM) {
        open_lib(L, luaopen_table, LUA_TABLIBNAME);
        open_lib(L, luaopen_string, LUA_STRLIBNAME);
        return;
    }
    open_lib(L, luaopen_coroutine, LUA_COLIBNAME);
    open_lib(L, luaopen_table, LUA_TABLIBNAME);
    open_lib(L, luaopen_string, LUA_STRLIBNAME);
    open_lib(L, luaopen_math, LUA_MATHLIBNAME);
    open_lib(L, luaopen_debug, LUA_DBLIBNAME);
    open_lib(L, luaopen_utf8, LUA_UTF8LIBNAME);
    open_lib(L, luaopen_bit32, LUA_BITLIBNAME);
    open_lib(L, luaopen_buffer, LUA_BUFFERLIBNAME);
    open_lib(L, luaopen_vector, LUA_VECLIBNAME);
    if (kind == SCRIPT_VM_UI) {
        open_lib(L, luaopen_os, LUA_OSLIBNAME);   // the UI VM removes nothing (§10.2 step 4)
    }
}

// --- the deterministic tostring (docs/LUAU-LAYER.md §10.2 step 5) ---------------------------

// Pushes the sandbox's string form of the value at `idx`. Reference types become their TYPE
// NAME: the stock form prints an address, which differs per run and per peer, and a script that
// branched on one would desync with no float and no unkeyed RNG anywhere. Value types keep the
// stock conversion, which is a pure function of the value.
void push_tostring(lua_State* L, int idx) {
    switch (lua_type(L, idx)) {
        case LUA_TTABLE:    lua_pushstring(L, "table"); return;
        case LUA_TFUNCTION: lua_pushstring(L, "function"); return;
        case LUA_TUSERDATA: lua_pushstring(L, "userdata"); return;
        case LUA_TLIGHTUSERDATA: {
            // A handle: the DOMAIN name, never the bits. The bits are state; printing them into
            // a log line is fine, but printing them into a string a script can compare is a
            // pointer value by another route.
            lua_pushstring(L, "handle");
            return;
        }
        case LUA_TTHREAD:   lua_pushstring(L, "thread"); return;
        case LUA_TBUFFER:   lua_pushstring(L, "buffer"); return;
        case LUA_TVECTOR:   lua_pushstring(L, "vector"); return;
        default: break;
    }
    size_t len = 0;
    const char* s = luaL_tolstring(L, idx, &len);   // numbers, strings, booleans, nil
    lua_pushlstring(L, s, len);
    lua_remove(L, -2);                              // drop luaL_tolstring's own copy
}

// _G.tostring. One argument, one result; see push_tostring for why references lose their address.
int sandbox_tostring(lua_State* L) {
    luaL_checkany(L, 1);
    push_tostring(L, 1);
    return 1;
}

// True for the flag characters a printf conversion may carry before its width.
bool is_format_flag(char c) {
    return c == '-' || c == '+' || c == ' ' || c == '#' || c == '0';
}

// True for an ASCII decimal digit. Spelled out rather than reached through <ctype.h>, which is
// locale-sensitive and is not on the system-include allowlist (docs/CPP-SUBSET.md §1).
bool is_digit(char c) { return c >= '0' && c <= '9'; }

// _G.string.format, wrapped. Upvalue 1 is the original. Every argument consumed by a `%s` or a
// `%*` conversion is replaced with push_tostring's form first, then the original does the real
// formatting.
//
// `%*` is the one that matters and the doc did not name it: stock `%s` calls luaL_checklstring,
// so a table argument is a TYPE ERROR there, never an address - but Luau's `%*` extension goes
// through luaL_tolstring and prints `table: 0x...` (measured against the 0.696 pin). `%p` needs
// no handling at all: Luau's format rejects it as an invalid option.
int sandbox_string_format(lua_State* L) {
    const int top = lua_gettop(L);
    size_t fl = 0;
    const char* f = luaL_checklstring(L, 1, &fl);
    const char* end = f + fl;
    int arg = 1;
    for (const char* p = f; p < end;) {
        if (*p != '%') { ++p; continue; }
        ++p;
        if (p >= end) break;
        if (*p == '%') { ++p; continue; }                 // %% is a literal, consumes no argument
        if (*p == '*') {
            ++p;
            ++arg;
            if (arg <= top) { push_tostring(L, arg); lua_replace(L, arg); }
            continue;
        }
        while (p < end && is_format_flag(*p)) ++p;
        while (p < end && is_digit(*p)) ++p;
        if (p < end && *p == '.') { ++p; while (p < end && is_digit(*p)) ++p; }
        if (p >= end) break;
        const char conv = *p++;
        ++arg;
        if (conv == 's' && arg <= top) { push_tostring(L, arg); lua_replace(L, arg); }
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    for (int i = 1; i <= top; ++i) lua_pushvalue(L, i);
    lua_call(L, top, 1);
    return 1;
}

// --- sortedpairs (docs/LUAU-LAYER.md §10.2.1) ------------------------------------------------

// One collected key. `slot` is its position in the collection order, which the sort preserves on
// a tie so the walk is stable as well as ordered.
struct SortKey {
    double      num;      // valid when kind == 0
    const char* str;      // valid when kind == 1; points into the Luau string, rooted by the table
    u32         len;
    u32         slot;
    u8          kind;     // 0 = number, 1 = string
    u8          _pad[7];
};

// Total order of docs/LUAU-LAYER.md §10.2.1: numbers before strings; numbers ascending by value;
// strings bytewise, shorter first on a common prefix. True iff a sorts strictly before b.
bool key_less(const SortKey& a, const SortKey& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.kind == 0) return a.num < b.num;
    const u32 n = a.len < b.len ? a.len : b.len;
    const int c = n != 0 ? memcmp(a.str, b.str, (size_t)n) : 0;
    if (c != 0) return c < 0;
    return a.len < b.len;
}

// Stable bottom-up merge sort over `n` records, using `tmp` (also n records) as the scratch half.
// A merge, not the foundation radix: docs/CONTAINERS.md §4's sort takes an INTEGER key and no
// comparator, and the runtime has no generic comparison sort by rule, so the one place that
// needs a total order over mixed number/string keys carries it (docs/LUAU-LAYER.md §10.2.1 is
// corrected in the same commit - it named the foundation sort, which cannot express this).
void sort_keys(SortKey* a, SortKey* tmp, u32 n) {
    for (u32 width = 1; width < n; width *= 2u) {
        for (u32 lo = 0; lo < n; lo += 2u * width) {
            const u32 mid = lo + width < n ? lo + width : n;
            const u32 hi = lo + 2u * width < n ? lo + 2u * width : n;
            u32 i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = key_less(a[j], a[i]) ? a[j++] : a[i++];
            while (i < mid) tmp[k++] = a[i++];
            while (j < hi) tmp[k++] = a[j++];
        }
        for (u32 i = 0; i < n; ++i) a[i] = tmp[i];
    }
}

// The iterator returned by sortedpairs. Generic-for calls it as iter(state, control); the cursor
// lives in the state table's array part (state[3]) rather than in the control variable, so the
// walk is O(1) per step and does not have to find the previous key's position.
//
// A value set to nil mid-walk is SKIPPED, never re-ordered: the key array was fixed when the
// walk began, so removing an entry cannot change the order of the ones still to come.
int sortedpairs_iter(lua_State* L) {
    lua_rawgeti(L, 1, 1);                     // the table being walked
    lua_rawgeti(L, 1, 2);                     // the sorted key array
    lua_rawgeti(L, 1, 3);                     // the cursor
    const int t = lua_gettop(L) - 2;
    const int keys = t + 1;
    int i = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    const int n = lua_objlen(L, keys);
    while (i < n) {
        ++i;
        lua_pushinteger(L, i);
        lua_rawseti(L, 1, 3);                 // publish the cursor before returning
        lua_rawgeti(L, keys, i);              // key
        lua_pushvalue(L, -1);                 // key, key
        lua_rawget(L, t);                     // key, value
        if (!lua_isnil(L, -1)) return 2;
        lua_pop(L, 2);                        // deleted mid-walk: skip it
    }
    lua_pushnil(L);
    return 1;
}

// _G.sortedpairs(t). Same code in all three VMs (docs/LUAU-LAYER.md §10.2.1); it replaces `pairs`
// in the sim VM, where iteration order must be a pure function of the KEY SET and not of the
// insertion history (docs/LUAU-LAYER.md §1.1).
int sandbox_sortedpairs(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_newtable(L);                          // [2] = collected keys, in encounter order
    const int keys = lua_gettop(L);
    u32 n = 0;
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        lua_pop(L, 1);                        // drop the value; the key is what we collect
        const int kt = lua_type(L, -1);
        if (kt == LUA_TNUMBER) {
            const double x = lua_tonumber(L, -1);
            // "numbers ascending by value (NaN is impossible - integers only; a non-integer key
            // is a Luau error)". x != x catches NaN, which is not ordered by `<` and would make
            // the sort's result depend on the comparison order.
            if (x != x || x != (double)(long long)x) {
                luaL_error(L, "sortedpairs: number key is not an integer");
            }
        } else if (kt != LUA_TSTRING) {
            luaL_error(L, "sortedpairs: unsupported key type '%s'", luaL_typename(L, -1));
        }
        lua_pushvalue(L, -1);
        lua_rawseti(L, keys, (int)(n + 1u));
        ++n;
    }

    if (n > 1u) {
        // The record array is a Luau userdata so its bytes come from the VM's own pool and are
        // collected with the walk (docs/MEMORY.md §8.6); no engine arena is reachable from here,
        // and pool_alloc is not callable outside vendor_glue by gate.
        SortKey* recs = (SortKey*)lua_newuserdata(L, sizeof(SortKey) * 2u * (size_t)n);
        SortKey* tmp = recs + n;
        for (u32 i = 0; i < n; ++i) {
            lua_rawgeti(L, keys, (int)(i + 1u));
            recs[i].slot = i;
            recs[i].num = 0.0;
            recs[i].str = nullptr;
            recs[i].len = 0;
            for (u32 p = 0; p < 7u; ++p) recs[i]._pad[p] = 0;
            if (lua_type(L, -1) == LUA_TNUMBER) {
                recs[i].kind = 0;
                recs[i].num = lua_tonumber(L, -1);
            } else {
                size_t sl = 0;
                recs[i].kind = 1;
                recs[i].str = lua_tolstring(L, -1, &sl);
                recs[i].len = (u32)sl;
            }
            lua_pop(L, 1);
        }
        sort_keys(recs, tmp, n);

        lua_newtable(L);                      // the sorted key array
        const int sorted = lua_gettop(L);
        for (u32 i = 0; i < n; ++i) {
            lua_rawgeti(L, keys, (int)(recs[i].slot + 1u));
            lua_rawseti(L, sorted, (int)(i + 1u));
        }
        lua_replace(L, keys);                 // the sorted array takes the collected array's slot
        lua_pop(L, 1);                        // drop the userdata
    }

    lua_pushcfunction(L, &sortedpairs_iter, "sortedpairs_iter");
    lua_newtable(L);                          // the state: { t, keys, cursor }
    lua_pushvalue(L, 1);
    lua_rawseti(L, -2, 1);
    lua_pushvalue(L, keys);
    lua_rawseti(L, -2, 2);
    lua_pushinteger(L, 0);
    lua_rawseti(L, -2, 3);
    lua_pushnil(L);
    return 3;                                 // iter, state, control
}

}  // namespace

ErrCode script_sandbox_open(ScriptVm* vm) {
    lua_State* L = vm->L;
    open_libraries(L, vm->kind);

    const char* const* removals = nullptr;
    if (vm->kind == SCRIPT_VM_SIM) removals = SIM_REMOVE;
    else if (vm->kind == SCRIPT_VM_DATA) removals = DATA_REMOVE;

    if (removals != nullptr) {
        for (const char* const* r = removals; *r != nullptr; ++r) {
            if (!remove_name(L, *r)) {
                return script_set_error(vm, ERR_SCRIPT_SANDBOX, *r);
            }
        }
        // Verified here, not only in the test: a removal that silently did nothing is a hole in
        // the sandbox, and a hole discovered by a test that someone forgot to run is not a gate.
        for (const char* const* r = removals; *r != nullptr; ++r) {
            if (!name_is_absent(L, *r)) {
                return script_set_error(vm, ERR_SCRIPT_SANDBOX, *r);
            }
        }
    }

    // Step 5: the replacements. The sim VM is the one that must not see an address; the other two
    // get sortedpairs (same code everywhere, §10.2.1) but keep their stock tostring/format, since
    // neither feeds sim state and the UI's inspector genuinely wants the address.
    if (vm->kind == SCRIPT_VM_SIM) {
        lua_pushcfunction(L, &sandbox_tostring, "tostring");
        lua_setglobal(L, "tostring");

        lua_getglobal(L, LUA_STRLIBNAME);
        TL_ASSERT(lua_istable(L, -1));
        lua_getfield(L, -1, "format");                       // the original, as the upvalue
        lua_pushcclosurek(L, &sandbox_string_format, "format", 1, nullptr);
        lua_setfield(L, -2, "format");
        lua_pop(L, 1);
    }

    lua_pushcfunction(L, &sandbox_sortedpairs, "sortedpairs");
    lua_setglobal(L, "sortedpairs");
    return ERR_OK;
}

void script_sandbox_freeze(ScriptVm* vm) {
    lua_State* L = vm->L;
    // docs/LUAU-LAYER.md §10.2 step 11, through the vendored implementation rather than around it.
    //
    // This was a hand-kept two-name array (`_G`, `string`, `table`) plus setsafeenv, and it MISSED
    // the string metatable - a fourth table permanently rooted by the VM and reachable from a
    // sealed sim script through `getmetatable('')`, which is not on the removal list. Measured by
    // review round 1 (D3): a sealed sim VM accepted `getmetatable('').tl_hidden = 41`, a LATER
    // chunk in a LATER tick read it back, and it survived ten tick brackets including dev's full
    // LUA_GCCOLLECT. That is a live breach of §0 - "authoritative state never lives in the Luau
    // heap" - whose enforcement mechanism (1) is this very freeze; mechanism (2), the growth_ticks
    // heuristic, is structurally blind to it (one scalar write is a one-time delta, not sustained
    // growth), and mechanism (3), the dual-sim test, is a later lane's.
    //
    // luaL_sandbox does three things: readonly on EVERY table in _G (not a list that has to be
    // kept), readonly on the string metatable (the one that mattered), then readonly +
    // setsafeenv on _G. The hand-rolled version reimplemented steps 1 and 3, narrowed step 1 to
    // two names, and dropped step 2. A hand-kept list where a closure over the property was
    // available is a class docs/LESSONS.md already carries (entropy_carriers).
    //
    // Ordering is not load-bearing: setreadonly is order-independent, and luaL_sandbox sets _G
    // readonly LAST so its own iteration runs while _G is still writable.
    luaL_sandbox(L);
}
