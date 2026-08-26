#pragma once
// ---------------------------------------------------------------------------------------------
// script.h - the Luau boundary: ScriptVm and nothing Luau-shaped.
//
// Spec: docs/LUAU-LAYER.md §10.1 (file layout), §10.2 (the VM construction sequence), §10.7
//   (the per-tick bracket), §10.11 (tests). docs/CANON.md "Luau VMs" is the one-line summary.
// Purpose: app/, editor/ and the tests see `ScriptVm*` and our own types. A `lua_State*` or a
//   Luau header may appear ONLY under src/script (docs/ARCHITECTURE.md §1); tools/audit's
//   BACKEND_HEADERS entry is the gate, and this header is what makes obeying it possible.
// Invariants: one MemPool per VM, budgeted (docs/MEMORY.md §6); the sim VM is interpreter-only
//   with frozen globals and a per-tick safepoint budget; authoritative state NEVER lives in the
//   Luau heap (docs/LUAU-LAYER.md §0), so no call here can move engine state into a VM.
// Determinism: a sim VM's observable behaviour is a pure function of its bytecode and the world
//   it is handed. The safepoint budget, the removal list and the pool budget are all
//   fingerprinted inputs (docs/BUILD.md §5), so every peer trips on the same safepoint and
//   refuses the same allocation. Nothing here reads a clock, an address or an iteration order.
// Threading: a ScriptVm has one owning thread - the sim VM the tick thread, the UI VM the frame
//   thread. There is no internal locking and none is wanted; the two VMs never share a state.
// Includes: foundation/tl_types.h, foundation/strview.h, foundation/interner.h,
//   foundation/mem_pool.h, foundation/vmem_arena.h.
//
// NOT here yet, by lane split (docs/ROADMAP.md §2): script_load_manifest/require (§10.9),
// script_reload (§10.8), the ecs./alloy./events./input./data. binding tables and the system
// trampoline (§10.6) are the W3 luau-bindings lane's. They are named rather than stubbed: a
// guessed signature for a spec section this lane did not build is drift, not a contract.
// ---------------------------------------------------------------------------------------------
#include "foundation/interner.h"
#include "foundation/mem_pool.h"
#include "foundation/strview.h"
#include "foundation/tl_types.h"
#include "foundation/vmem_arena.h"

// The script module's ErrCode range, 0x07xx (docs/CANON.md "Types": per-module ranges).
constexpr ErrCode ERR_SCRIPT_BAD_ARG  = (ErrCode)0x0701;  // null/zero field in ScriptVmDesc
constexpr ErrCode ERR_SCRIPT_OOM      = (ErrCode)0x0702;  // pool reserve or lua_newstate refused
constexpr ErrCode ERR_SCRIPT_SANDBOX  = (ErrCode)0x0703;  // a removal/replacement step did not take
constexpr ErrCode ERR_SCRIPT_COMPILE  = (ErrCode)0x0704;  // luau_compile rejected the source
constexpr ErrCode ERR_SCRIPT_LOAD     = (ErrCode)0x0705;  // luau_load rejected the bytecode
constexpr ErrCode ERR_SCRIPT_RUNTIME  = (ErrCode)0x0706;  // the chunk raised; script_last_error has it
constexpr ErrCode ERR_SCRIPT_SEALED   = (ErrCode)0x0707;  // an init-only call after script_seal

// The longest error message kept per VM. Luau's own messages are chunkname + line + reason; the
// traceback is appended by the errfunc (docs/LUAU-LAYER.md §10.6) and truncated to fit.
enum : u32 { SCRIPT_ERR_MAX = 1024u };

// The three VMs of docs/LUAU-LAYER.md §1. The kind is fixed at creation and decides the library
// set, the removals, the freeze and whether native codegen is even considered.
enum ScriptVmKind : u8 { SCRIPT_VM_SIM = 0, SCRIPT_VM_UI = 1, SCRIPT_VM_DATA = 2 };

// Opaque: the definition is src/script/vm.h, which is the only place that may name a lua_State.
struct ScriptVm;

// Everything a VM needs to exist. `pool_reserve_bytes`/`pool_budget_bytes` come from app/'s
// reserve table and are therefore fingerprinted (docs/MEMORY.md §7 R-2); `budget_safepoints` and
// `gc_step_kb` are the `script.*` SIM cvars of docs/CANON.md "Cvars". `perm` owns the ScriptVm
// itself and never shrinks; `interner` is the process interner the atom callback will consult
// (see script_useratom_installed). Every field is required.
struct ScriptVmDesc {
    NameHash       pool_id;              // arena/pool id for the registry and the profiler
    u64            pool_reserve_bytes;   // address space reserved for the VM's heap
    u64            pool_budget_bytes;    // <= reserve; over it, Luau raises "not enough memory"
    u32            budget_safepoints;    // `script.budget_safepoints` (sim VM, per tick)
    u32            gc_step_kb;           // `script.gc_step_kb` (the per-tick GC step)
    Interner*      interner;             // process interner; may be null for the data VM
    VMemArena*     perm;                 // where the ScriptVm struct lives
    const VMemApi* os;                   // the platform vmem table the pool reserves through
    // The SHARED vendor pool (`PLATFORM.md` §9.5's `pool_vendor`) the Luau compiler allocates
    // from, NOT this VM's own pool. Ruled 2026-08-26 (Rafael, review round 1 D2): binding the
    // compile window to the VM pool made an over-budget COMPILE process-fatal on the same pool
    // where an over-budget EXECUTION is survivable by contract, and made the trip point a
    // function of runtime heap occupancy rather than of the source. Required; a null one is
    // ERR_SCRIPT_BAD_ARG at creation, never a silent fall back to the VM pool.
    MemPool*       compile_pool;
};

// The headroom `compile_pool` must show before a compile is attempted, so the refusal is an
// ErrCode and the fatal in vendor_new.cpp is never reached. DERIVED, not guessed: the Luau
// compiler's pool peak was measured at 90.66x the source size for a 1 KB source, falling to
// 49.83x at 64 KB, with an ~88 KB floor for even a tiny one (Luau 0.696, x86-64). The constants
// below carry ~3x margin on the floor and ~1.4x on the worst ratio.
enum : u64 { SCRIPT_COMPILE_HEADROOM_MIN = 256u * 1024u };
enum : u64 { SCRIPT_COMPILE_BYTES_PER_SRC_BYTE = 128u };

// Builds a sim VM: base/table/string only, the §10.2 step 4 removals, the deterministic
// tostring/string.format replacements, sortedpairs, the fx table, the safepoint budget, and NO
// native codegen. Globals stay writable until script_seal. Fails with ERR_SCRIPT_BAD_ARG on a
// malformed desc and ERR_SCRIPT_OOM if the reserve or lua_newstate is refused; no partial VM
// survives a failure (docs/LUAU-LAYER.md §10.2).
Result<ScriptVm*> script_create_sim(const ScriptVmDesc* d);

// Builds a UI/editor VM: the stock library set, nothing removed, `pairs` and coroutines present,
// read-only world access to come from the W3 binding tables. Same failure contract as
// script_create_sim. Native codegen is the only VM that may use it (docs/LUAU-LAYER.md §1);
// see script_codegen_available for what this build actually offers.
Result<ScriptVm*> script_create_ui(const ScriptVmDesc* d);

// Builds a throwaway data VM for one table compile (docs/ASSETS-AND-DATA.md §3): the stock set
// minus os/io/loadstring/getfenv/setfenv, the fx literal constructors, no engine bindings. Its
// OUTPUT is hashed, not the VM, so it is destroyed after the compile. Same failure contract.
Result<ScriptVm*> script_create_data(const ScriptVmDesc* d);

// Closes the lua_State and releases the VM's pool reserve back to the OS. The ScriptVm's own
// bytes belong to `perm` and are not returned. Null is a no-op; a destroyed VM must not be used.
void script_destroy(ScriptVm* vm);

// The kind this VM was created as. Cheap; used by the bindings to refuse a sim-only entry point
// in the UI VM and vice versa.
ScriptVmKind script_kind(const ScriptVm* vm);

// docs/LUAU-LAYER.md §10.2 step 11: freezes the VM. lua_setsafeenv + lua_setreadonly on _G, on
// every binding table and on string/table, after which a global assignment raises "attempt to
// modify a readonly table". Init-only bindings refuse with ERR_SCRIPT_SEALED afterwards. Calling
// it twice is an error, not a no-op, so a double-seal in the wiring is visible.
ErrCode script_seal(ScriptVm* vm);

// True while init-only registrations (components, systems, actions, require) are still legal -
// that is, between creation and script_seal (docs/LUAU-LAYER.md §10.2 step 10).
bool script_init_open(const ScriptVm* vm);

// Compiles `source` with the pinned options (docs/LUAU-LAYER.md §10.9 - the same constexpr
// tools/luauc will use) and runs it under lua_pcall with the traceback errfunc. `chunkname` is
// spelled without the leading '=' and is prefixed here. Returns ERR_SCRIPT_COMPILE,
// ERR_SCRIPT_LOAD or ERR_SCRIPT_RUNTIME; the message is script_last_error(vm) until the next
// call. Results are discarded - script_eval_int is the form that returns one.
ErrCode script_run_source(ScriptVm* vm, const char* chunkname, StrView source);

// Runs `expr` as a Luau expression and returns its value as an exact integer. The value must be
// a number equal to its own floor and within +-2^53, else ERR_SCRIPT_RUNTIME with a message
// naming what came back. The dev console and the tests read fx raw bits out of a VM through
// this; nothing on a sim path calls it.
Result<i64> script_eval_int(ScriptVm* vm, StrView expr);

// The message from the last failing call on this VM, NUL-terminated, empty after a success.
// Valid until the next call that can fail; never null.
const char* script_last_error(const ScriptVm* vm);

// docs/LUAU-LAYER.md §10.7 step 1, the VM half: reloads the safepoint budget and runs one
// incremental GC step of `gc_step_kb`. The World-facing `sys_script_begin` system that calls
// this is the W3 trampoline lane's; nothing here can change state.
void script_tick_begin(ScriptVm* vm);

// docs/LUAU-LAYER.md §10.7 step 3, the VM half: the dev-tier leak heuristic (a full collect,
// then a growth counter over reachable bytes). Compiled to nothing but the counter reset in
// netcode/ship. Nothing here can change state.
void script_tick_end(ScriptVm* vm);

// Safepoints left in this tick's budget; negative once the budget has been tripped. Sim VM only
// - the UI VM's budget only logs and the data VM's fails a compile (docs/LUAU-LAYER.md §10.2).
i64 script_budget_left(const ScriptVm* vm);

// The VM pool's live/peak/per-class counters (docs/MEMORY.md §8.6), for the profiler and for the
// tests that assert the pool returns to baseline after script_destroy. Never null.
const MemPoolStats* script_pool_stats(const ScriptVm* vm);

// Whether this build can hand the UI VM native codegen. FALSE in every build of rev 1: the Luau
// CodeGen library is deliberately not vendored (vendor/luau/CMakeLists.txt, and the ruling
// request in TODO.md carries the measured build cost). Stated as a queryable fact rather than a
// silent interpreter fallback - docs/CLAUDE.md's "fail loudly", applied to a missing capability.
bool script_codegen_available(void);

// The atom Luau assigns to `s` inside this VM (docs/LUAU-LAYER.md §10.2 step 8): the interner's
// StrId for a name registered BEFORE the string was created, and -1 for anything else. A read,
// never a registration - a script that builds a string at runtime must not be able to grow the
// interner - and a NameHash match with different bytes reads as -1, because a hash is not an
// identity (intern() fatals on that collision; a lookup simply says no). Returns -1 in a VM
// created with no interner.
i32 script_atom_of(ScriptVm* vm, StrView s);

// What the last compile drew from the shared vendor pool - the compile WINDOW's own peak delta,
// not the enclosing call's. This is the number that discriminates "the compiler allocated from
// the pool" from "the CRT served it": script_run_source also loads and runs, and both of those
// allocate through the VM's own hook, so a figure measured across the whole call is dominated by
// bytes that have nothing to do with RR-18. Zero before the first compile.
u64 script_last_compile_bytes(const ScriptVm* vm);

// Whether the string-atom callback of docs/LUAU-LAYER.md §10.2 step 8 is installed - true once
// any VM has been created with an interner (RR-19, ruled 2026-08-26). Luau's callback takes
// (const char*, size_t) and no context pointer, so the process Interner is reached through the
// one pointer named in tools/audit/static_allow.txt. A VM created with a NULL interner leaves it
// uninstalled, and script_atom_of then yields -1 for every string; that is a program with no
// name registry, not a fallback.
bool script_useratom_installed(void);
