#pragma once
// ---------------------------------------------------------------------------------------------
// compile_opts.h - the ONE pinned lua_CompileOptions, shared by every compiler in the program.
//
// Spec: docs/LUAU-LAYER.md §10.9 ("the struct is one constexpr shared by luauc and loader.cpp
//   through src/script/compile_opts.h"), §6 (bytecode and the fingerprint).
// Purpose: dev compiles on load, tools/luauc compiles at build, and the two outputs must be
//   BYTE-IDENTICAL or the fingerprint is a lie. Identical output needs the same compiler AND the
//   same options; the compiler is pinned by vendor/VERSIONS, and this header is the options half.
// Invariants: -O2, debug level 1 (line info for tracebacks, no local/upvalue names), no type
//   info, no coverage, no vector library, no mutable globals, no userdata types. Every pointer
//   field is null on purpose: a non-null one changes the bytecode.
// Determinism: these values hash into build_id through the bytecode they produce
//   (docs/BUILD.md §5). Changing one is a ruling, not a tweak - every peer's fingerprint moves.
// Threading: a pure constexpr initializer; luau_compile takes a non-const pointer, so callers
//   copy it into a local (script_compile_options()) rather than sharing one mutable object.
// Includes: <luacode.h> (the vendored Luau compiler's public header - src/script only).
// ---------------------------------------------------------------------------------------------
#include <luacode.h>

// A fresh copy of the pinned options. Returned by value because luau_compile takes a mutable
// pointer: one shared mutable instance would be namespace-scope writable state, which
// docs/CPP-SUBSET.md §1 bans, and would let a caller silently change everyone's bytecode.
inline lua_CompileOptions script_compile_options(void) {
    lua_CompileOptions o = {};
    o.optimizationLevel = 2;              // docs/LUAU-LAYER.md §6: -O2
    o.debugLevel = 1;                     // line info + function names; enough for a traceback
    o.typeInfoLevel = 0;                  // native codegen is not used (docs/LUAU-LAYER.md §1)
    o.coverageLevel = 0;
    o.vectorLib = nullptr;                // no vector builtin: the palette is fixed point
    o.vectorCtor = nullptr;
    o.vectorType = nullptr;
    o.mutableGlobals = nullptr;           // globals are frozen at seal, so none are mutable
    o.userdataTypes = nullptr;
    o.librariesWithKnownMembers = nullptr;
    o.libraryMemberTypeCb = nullptr;
    o.libraryMemberConstantCb = nullptr;
    o.disabledBuiltins = nullptr;
    return o;
}
