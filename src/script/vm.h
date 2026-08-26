#pragma once
// ---------------------------------------------------------------------------------------------
// vm.h - the ScriptVm definition and the module-internal seams. NOT a public header.
//
// Spec: docs/LUAU-LAYER.md §10.2 (the construction sequence this struct is the state of), §10.7
//   (the per-tick counters), §10.1 (which file owns which step).
// Purpose: script.h keeps `ScriptVm` opaque so no Luau type escapes the module; this is where it
//   is a real struct. Only src/script TUs include it.
// Invariants: `L` is non-null for a live VM and its lua_Callbacks::userdata points BACK at this
//   struct - that back-pointer is how the context-free Luau callbacks (interrupt, panic) find
//   their VM without a namespace-scope global, which docs/CPP-SUBSET.md §1 bans. `pool` owns the
//   VM's whole heap; nothing in the struct is registered, hashed or snapshotted.
// Determinism: `budget_left` counts SAFEPOINTS, which is a pure function of the bytecode
//   executed (docs/LUAU-LAYER.md §10.2 step 8), so peers trip together. `last_reachable` and
//   `growth_ticks` are dev-tier heuristics and are deliberately NOT state.
// Threading: one owning thread per VM; no locking.
// Includes: <lua.h>, script/script.h, foundation/mem_pool.h.
// ---------------------------------------------------------------------------------------------
#include <lua.h>

#include "foundation/mem_pool.h"
#include "script/script.h"

struct ScriptVm {
    lua_State*   L;                       // null only between allocation and a successful create
    MemPool      pool;                    // this VM's entire heap (docs/MEMORY.md §8.6)
    Interner*    interner;                // the process interner; see script_useratom_installed
    MemPool*     compile_pool;            // the SHARED vendor pool the compiler draws from (D2)
    u64          last_compile_bytes;      // the last compile window's pool peak delta (D1)
    ScriptVmKind kind;
    u8           init_open;               // 1 until script_seal
    u8           budget_warned;           // UI VM: the per-frame over-budget warning is once
    u8           _pad0;
    u32          budget_safepoints;       // reloaded into budget_left every tick_begin
    i64          budget_left;             // signed: negative means tripped, and stays tripped
    u32          gc_step_kb;
    u32          growth_ticks;            // dev leak heuristic (docs/LUAU-LAYER.md §10.7 step 3)
    u64          last_reachable;          // dev leak heuristic: bytes reachable after a collect
    u32          err_len;
    char         err[SCRIPT_ERR_MAX];     // script_last_error's buffer; never static storage
};

// Records `msg` (NUL-terminated, truncated to SCRIPT_ERR_MAX-1) as this VM's last error and
// returns `code`, so every failure site is one line. A null msg records the empty string.
ErrCode script_set_error(ScriptVm* vm, ErrCode code, const char* msg);

// Clears the last-error buffer. Called at the top of every call that can fail, so a stale
// message from three calls ago can never be mistaken for this call's.
void script_clear_error(ScriptVm* vm);

// docs/LUAU-LAYER.md §10.2 steps 3-5: opens the kind's library set, applies the removal list and
// installs the deterministic tostring/string.format replacements and sortedpairs. Implemented in
// sandbox.cpp. Returns ERR_SCRIPT_SANDBOX if any removal did not take, which is checked rather
// than assumed - a renamed upstream global would otherwise leave a hole silently.
ErrCode script_sandbox_open(ScriptVm* vm);

// docs/LUAU-LAYER.md §10.2 step 11, the sandbox half: lua_setsafeenv plus lua_setreadonly on
// _G, string and table. Implemented in sandbox.cpp; script_seal calls it after the binding
// tables have frozen themselves.
void script_sandbox_freeze(ScriptVm* vm);

// docs/LUAU-LAYER.md §10.3: creates the `fx` global for this VM kind. Implemented in bind_fx.cpp.
// The sim VM gets the whole table; the UI VM additionally gets fx.to_f64; the data VM gets the
// literal constructors and the constants (a table compile writes fx literals and nothing else).
void script_bind_fx(ScriptVm* vm);

// docs/LUAU-LAYER.md §10.2 step 8: installs the string-atom callback on `L` against the process
// interner. A null interner installs nothing, and lua_tostringatom then yields -1 for every
// string - the honest state for a VM with no name registry, not a fallback. Implemented in
// atom.cpp, which is alone in its TU because it holds the one exempted pointer (RR-19).
void script_install_useratom(lua_State* L, Interner* interner);

// The installed interner, or null. For the tests and for script_useratom_installed().
const Interner* script_atom_interner(void);

// Marks `t` at the top of the stack read-only and pops nothing. Used by every binding table so
// a script cannot add a function to `fx` and have it look official.
void script_freeze_table(lua_State* L, int idx);
