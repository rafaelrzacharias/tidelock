// vm.cpp - lua_State lifecycle per VM kind: the pool, the allocator, the callbacks, the error
// path, the GC step. Contract: script/vm.h and script/script.h.
// Spec: docs/LUAU-LAYER.md §10.2 (construction, steps 1-11), §10.7 (the per-tick bracket).
#include <lua.h>
#include <lualib.h>
#include <luacode.h>

#include <string.h>

#include "foundation/tl_assert.h"
#include "foundation/tl_log.h"
#include "script/compile_opts.h"
#include "script/handles.h"
#include "script/vm.h"
#include "vendor_glue/luau_alloc.h"

// The adaptor is declared without a Luau header on purpose (vendor_glue/luau_alloc.h says why);
// this is the TU that may see lua_Alloc, so this is where the two are proved compatible.
static_assert(__is_same(lua_Alloc, decltype(&tl_luau_alloc)),
              "tl_luau_alloc must match lua_Alloc exactly (docs/LUAU-LAYER.md section 10.2 step 2)");

namespace {

// The VM behind a lua_State. lua_Callbacks::userdata is documented by Luau as "never overwritten
// by Luau", which is what lets the context-free callbacks below find their VM without the
// namespace-scope mutable pointer docs/CPP-SUBSET.md section 1 bans.
ScriptVm* vm_of(lua_State* L) {
    ScriptVm* vm = (ScriptVm*)lua_callbacks(L)->userdata;
    TL_ASSERT(vm != nullptr);
    return vm;
}

// docs/LUAU-LAYER.md section 10.2 step 8. Luau calls this at SAFEPOINTS (function entry, loop
// back edges) with gc < 0, and from the collector with gc >= 0; only the former is budgeted, so
// the count is a pure function of the bytecode executed and every peer trips on the same one.
void script_interrupt(lua_State* L, int gc) {
    if (gc >= 0) return;                       // a GC callback, not a safepoint: never budgeted
    ScriptVm* vm = vm_of(L);
    if (vm->budget_left < 0) return;           // already tripped this tick; do not re-raise
    vm->budget_left -= 1;
    if (vm->budget_left >= 0) return;
    if (vm->kind == SCRIPT_VM_UI) {
        // The UI VM's budget only logs (docs/LUAU-LAYER.md section 10.2 step 8): a slow panel is
        // a frame-rate problem, not a desync. Once per frame, not once per safepoint.
        if (!vm->budget_warned) {
            vm->budget_warned = 1;
            TL_LOG_WARN("script: ui VM over its per-frame safepoint budget (%u)", vm->budget_safepoints);
        }
        return;
    }
    luaL_error(L, "script budget exceeded after %u safepoints", vm->budget_safepoints);
}

// Reached only when an error is raised outside every protected call, which is a bug in THIS
// module: every entry point into Luau here goes through lua_pcall. Fatal on purpose - a silent
// return would leave the VM in an unwound, unusable state.
void script_panic(lua_State* L, int errcode) {
    (void)errcode;
    const char* msg = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(no message)";
    TL_LOG_ERR("script: unprotected Luau error: %s", msg);
    TL_FATAL("unprotected Luau error - every call into a VM must be protected "
             "(docs/LUAU-LAYER.md section 10.2 step 8)");
}

// The traceback errfunc of docs/LUAU-LAYER.md section 10.6: appends lua_debugtrace's stack to
// the message so a script failure names the line, not just the reason. Pushed once per
// protected call; it runs on the erroring stack, before it is unwound.
int script_traceback(lua_State* L) {
    const char* msg = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(non-string error)";
    const char* trace = lua_debugtrace(L);
    lua_pushfstring(L, "%s\n%s", msg, trace != nullptr ? trace : "(no traceback)");
    return 1;
}

// Copies the string at the top of the stack into the VM's error buffer and pops it.
ErrCode take_error(ScriptVm* vm, ErrCode code) {
    const char* msg = lua_type(vm->L, -1) == LUA_TSTRING ? lua_tostring(vm->L, -1) : "(non-string error)";
    const ErrCode e = script_set_error(vm, code, msg);
    lua_pop(vm->L, 1);
    return e;
}

// Steps 1-9 of docs/LUAU-LAYER.md section 10.2, shared by all three constructors. On any failure
// nothing survives: the pool is released and the caller gets an error, never a half-built VM.
Result<ScriptVm*> create(ScriptVmKind kind, const ScriptVmDesc* d) {
    if (d == nullptr || d->perm == nullptr || d->os == nullptr ||
        d->pool_reserve_bytes == 0 || d->pool_budget_bytes == 0 ||
        d->pool_budget_bytes > d->pool_reserve_bytes) {
        return Result<ScriptVm*>{ nullptr, ERR_SCRIPT_BAD_ARG };
    }

    ScriptVm* vm = (ScriptVm*)arena_push(d->perm, (u64)sizeof(ScriptVm), 16u);
    if (vm == nullptr) return Result<ScriptVm*>{ nullptr, ERR_SCRIPT_OOM };
    memset(vm, 0, sizeof(ScriptVm));
    vm->kind = kind;
    vm->interner = d->interner;
    vm->budget_safepoints = d->budget_safepoints;
    vm->budget_left = (i64)d->budget_safepoints;
    vm->gc_step_kb = d->gc_step_kb;
    vm->init_open = 1;

    // Step 1: the pool. One VMemArena reserve per VM, budgeted from app/'s reserve table.
    const ErrCode pe = pool_init(&vm->pool, d->pool_id, d->pool_reserve_bytes,
                                 d->pool_budget_bytes, d->os);
    if (pe != ERR_OK) return Result<ScriptVm*>{ nullptr, ERR_SCRIPT_OOM };

    // Step 2: the state, allocating only through the pool from its very first byte.
    vm->L = lua_newstate(&tl_luau_alloc, &vm->pool);
    if (vm->L == nullptr) {
        d->os->release(d->os->ctx, vm->pool.arena.base, vm->pool.arena.reserved);
        return Result<ScriptVm*>{ nullptr, ERR_SCRIPT_OOM };
    }

    // Step 8, first half: the back-pointer every other callback needs, before any Luau code can
    // run. Set before the libraries open so even an allocation failure during luaopen_* lands in
    // a state whose callbacks can find their VM.
    lua_Callbacks* cb = lua_callbacks(vm->L);
    cb->userdata = vm;
    cb->interrupt = &script_interrupt;
    cb->panic = &script_panic;
    cb->userthread = nullptr;     // the sim VM has no coroutines (docs/LUAU-LAYER.md section 9 R-1)
    // cb->useratom stays null - see script_useratom_installed() and the ruling request in TODO.md.

    script_register_tag_names(vm->L);

    // Steps 3-5: libraries, removals, replacements.
    const ErrCode se = script_sandbox_open(vm);
    if (se != ERR_OK) {
        lua_close(vm->L);
        vm->L = nullptr;
        d->os->release(d->os->ctx, vm->pool.arena.base, vm->pool.arena.reserved);
        return Result<ScriptVm*>{ nullptr, se };
    }

    // Step 6: the binding tables this lane owns. ecs/events/input/alloy/data/log are W3's.
    script_bind_fx(vm);

    // Step 9: native codegen. Sim and data VMs never consider it (docs/LUAU-LAYER.md section 1);
    // the UI VM would, but the CodeGen library is not vendored in rev 1 and
    // script_codegen_available() says so out loud rather than falling back in silence.

    return Result<ScriptVm*>{ vm, ERR_OK };
}

// Compiles `source` and leaves the loaded chunk on the stack. The bytecode buffer luau_compile
// returns is malloc'd by contract and freed here on every path.
ErrCode load_chunk(ScriptVm* vm, const char* chunkname, StrView source) {
    if (!script_can_compile_in_process()) {
        return script_set_error(vm, ERR_SCRIPT_NO_COMPILER,
                                "this tier cannot compile Luau source in-process: the Luau "
                                "compiler allocates with global operator new, which the "
                                "alloc-shim tripwire makes fatal in dev and netcode "
                                "(TODO.md RR-18)");
    }
    lua_CompileOptions opts = script_compile_options();
    size_t bc_size = 0;
    char* bc = luau_compile(source.ptr, (size_t)source.len, &opts, &bc_size);
    if (bc == nullptr) {
        return script_set_error(vm, ERR_SCRIPT_COMPILE, "luau_compile returned no bytecode");
    }
    // A compile error is encoded IN the bytecode: a leading 0 byte, then the message
    // (docs/LUAU-LAYER.md section 10.9). It is not a null return, which is why the check above
    // is not enough on its own.
    if (bc_size == 0 || bc[0] == 0) {
        const char* msg = bc_size > 1 ? bc + 1 : "(empty compile error)";
        const ErrCode e = script_set_error(vm, ERR_SCRIPT_COMPILE, msg);
        tl_luau_compile_free(bc);
        return e;
    }
    char named[256];
    named[0] = '=';                        // makes Luau print the chunkname verbatim in traces
    u32 n = 0;
    while (chunkname != nullptr && chunkname[n] != 0 && n + 2u < (u32)sizeof(named)) {
        named[n + 1u] = chunkname[n];
        ++n;
    }
    named[n + 1u] = 0;
    const int rc = luau_load(vm->L, named, bc, bc_size, 0);
    tl_luau_compile_free(bc);
    if (rc != 0) return take_error(vm, ERR_SCRIPT_LOAD);
    return ERR_OK;
}

}  // namespace

ErrCode script_set_error(ScriptVm* vm, ErrCode code, const char* msg) {
    TL_ASSERT(vm != nullptr);
    u32 n = 0;
    if (msg != nullptr) {
        while (msg[n] != 0 && n + 1u < SCRIPT_ERR_MAX) {
            vm->err[n] = msg[n];
            ++n;
        }
    }
    vm->err[n] = 0;
    vm->err_len = n;
    return code;
}

void script_clear_error(ScriptVm* vm) {
    TL_ASSERT(vm != nullptr);
    vm->err[0] = 0;
    vm->err_len = 0;
}

void script_freeze_table(lua_State* L, int idx) {
    lua_setreadonly(L, idx, 1);
}

Result<ScriptVm*> script_create_sim(const ScriptVmDesc* d) { return create(SCRIPT_VM_SIM, d); }
Result<ScriptVm*> script_create_ui(const ScriptVmDesc* d) { return create(SCRIPT_VM_UI, d); }
Result<ScriptVm*> script_create_data(const ScriptVmDesc* d) { return create(SCRIPT_VM_DATA, d); }

void script_destroy(ScriptVm* vm) {
    if (vm == nullptr) return;
    if (vm->L != nullptr) {
        lua_close(vm->L);                  // every byte goes back to the pool through the adaptor
        vm->L = nullptr;
        // docs/LUAU-LAYER.md section 10.11 (memory_exhaustion): "the pool counter returns to
        // baseline after lua_close". Asserted HERE rather than only in a test, because it is an
        // invariant of the allocator adaptor - a byte Luau handed back that the pool did not
        // account for is a leak in tl_luau_alloc, and the counter is the only witness.
        TL_ASSERT(pool_stats(&vm->pool)->live_bytes == 0);
    }
    // The pool's whole reserve returns to the OS. The ScriptVm struct itself belongs to the
    // caller's permanent arena and is deliberately not freed (docs/MEMORY.md section 1.2).
    VMemArena* a = &vm->pool.arena;
    if (a->base != nullptr && a->os != nullptr) {
        a->os->release(a->os->ctx, a->base, a->reserved);
        a->base = nullptr;
    }
}

ScriptVmKind script_kind(const ScriptVm* vm) {
    TL_ASSERT(vm != nullptr);
    return vm->kind;
}

ErrCode script_seal(ScriptVm* vm) {
    TL_ASSERT(vm != nullptr && vm->L != nullptr);
    script_clear_error(vm);
    if (!vm->init_open) {
        return script_set_error(vm, ERR_SCRIPT_SEALED, "script_seal called twice on one VM");
    }
    script_sandbox_freeze(vm);
    vm->init_open = 0;
    return ERR_OK;
}

bool script_init_open(const ScriptVm* vm) {
    TL_ASSERT(vm != nullptr);
    return vm->init_open != 0;
}

ErrCode script_run_source(ScriptVm* vm, const char* chunkname, StrView source) {
    TL_ASSERT(vm != nullptr && vm->L != nullptr);
    script_clear_error(vm);
    if (source.ptr == nullptr) {
        return script_set_error(vm, ERR_SCRIPT_BAD_ARG, "script_run_source: null source");
    }
    lua_State* L = vm->L;
    lua_pushcfunction(L, &script_traceback, "traceback");
    const int errfunc = lua_gettop(L);
    const ErrCode le = load_chunk(vm, chunkname, source);
    if (le != ERR_OK) {
        lua_remove(L, errfunc);
        return le;
    }
    const int rc = lua_pcall(L, 0, 0, errfunc);
    lua_remove(L, errfunc);
    if (rc != 0) return take_error(vm, ERR_SCRIPT_RUNTIME);
    return ERR_OK;
}

Result<i64> script_eval_int(ScriptVm* vm, StrView expr) {
    TL_ASSERT(vm != nullptr && vm->L != nullptr);
    script_clear_error(vm);
    if (expr.ptr == nullptr || expr.len == 0) {
        return Result<i64>{ 0, script_set_error(vm, ERR_SCRIPT_BAD_ARG, "script_eval_int: empty expression") };
    }
    // "return (<expr>)" is the whole trick: a Luau chunk is a function body, so a returned
    // expression comes back as one result with no eval() and no loadstring in the sandbox.
    char buf[SCRIPT_ERR_MAX];
    const char* prefix = "return (";
    u32 n = 0;
    while (prefix[n] != 0) { buf[n] = prefix[n]; ++n; }
    if (expr.len + n + 2u >= (u32)sizeof(buf)) {
        return Result<i64>{ 0, script_set_error(vm, ERR_SCRIPT_BAD_ARG, "script_eval_int: expression too long") };
    }
    memcpy(buf + n, expr.ptr, (size_t)expr.len);
    n += expr.len;
    buf[n] = ')';
    buf[n + 1u] = 0;

    lua_State* L = vm->L;
    lua_pushcfunction(L, &script_traceback, "traceback");
    const int errfunc = lua_gettop(L);
    const ErrCode le = load_chunk(vm, "eval", StrView{ buf, n + 1u });
    if (le != ERR_OK) {
        lua_remove(L, errfunc);
        return Result<i64>{ 0, le };
    }
    const int rc = lua_pcall(L, 0, 1, errfunc);
    lua_remove(L, errfunc);
    if (rc != 0) return Result<i64>{ 0, take_error(vm, ERR_SCRIPT_RUNTIME) };

    if (lua_type(L, -1) != LUA_TNUMBER) {
        const char* what = luaL_typename(L, -1);
        lua_pop(L, 1);
        return Result<i64>{ 0, script_set_error(vm, ERR_SCRIPT_RUNTIME, what) };
    }
    const double x = lua_tonumber(L, -1);
    lua_pop(L, 1);
    // Exactness, not truncation: a non-integer or an out-of-range value is a caller bug, and
    // silently flooring it is exactly the class of rounding the palette exists to forbid.
    if (!(x >= -9007199254740992.0 && x <= 9007199254740992.0) || x != (double)(i64)x) {
        return Result<i64>{ 0, script_set_error(vm, ERR_SCRIPT_RUNTIME,
                                                "value is not an exact integer within +-2^53") };
    }
    return Result<i64>{ (i64)x, ERR_OK };
}

const char* script_last_error(const ScriptVm* vm) {
    TL_ASSERT(vm != nullptr);
    return vm->err;
}

void script_tick_begin(ScriptVm* vm) {
    TL_ASSERT(vm != nullptr && vm->L != nullptr);
    vm->budget_left = (i64)vm->budget_safepoints;
    vm->budget_warned = 0;
    // The GC step runs INSIDE the tick so its cost is measured with the tick, not amortised
    // invisibly between them (docs/LUAU-LAYER.md section 10.7 step 1).
    if (vm->gc_step_kb != 0) {
        (void)lua_gc(vm->L, LUA_GCSTEP, (int)vm->gc_step_kb);
    }
}

void script_tick_end(ScriptVm* vm) {
    TL_ASSERT(vm != nullptr && vm->L != nullptr);
#if TL_DEV
    // docs/LUAU-LAYER.md section 10.7 step 3: a full collect, then "did reachable bytes grow
    // again this tick?". State is not in the Luau heap, so sustained growth means a script is
    // keeping something across ticks - the section 0 rule, caught by a heuristic rather than
    // trusted to review. The cvar that turns this off is the W3 tooling lane's; the counter
    // itself is here because the VM owns it.
    (void)lua_gc(vm->L, LUA_GCCOLLECT, 0);
    const u64 reachable = (u64)lua_gc(vm->L, LUA_GCCOUNT, 0) * 1024u + (u64)lua_gc(vm->L, LUA_GCCOUNTB, 0);
    vm->growth_ticks = reachable > vm->last_reachable ? vm->growth_ticks + 1u : 0u;
    vm->last_reachable = reachable;
#endif
}

i64 script_budget_left(const ScriptVm* vm) {
    TL_ASSERT(vm != nullptr);
    return vm->budget_left;
}

const MemPoolStats* script_pool_stats(const ScriptVm* vm) {
    TL_ASSERT(vm != nullptr);
    return pool_stats(&vm->pool);
}

bool script_codegen_available(void) {
    // vendor/luau/CMakeLists.txt vendors Common/VM/Ast/Compiler and NOT CodeGen; there is no
    // luau_codegen_supported() in this binary to ask. Reported as a fact, never guessed.
    return false;
}

bool script_can_compile_in_process(void) {
    // The one home for this fact; script.h's contract block carries the measurement and names
    // RR-18. Mirroring the tier macro at a call site would put the same claim in two places, and
    // a test that mirrors a header's #if cannot fail on the branch it does not take (LESSONS.md).
#if TL_TIER_DEV || TL_TIER_NETCODE
    return false;
#else
    return true;
#endif
}

bool script_useratom_installed(void) {
    // Luau's useratom callback is `int16_t (*)(const char*, size_t)` - no context parameter and
    // no lua_State - so reaching the process Interner from it needs namespace-scope mutable
    // state, banned by docs/CPP-SUBSET.md section 1 and enforced by tools/audit/symbols.py's
    // .data/.bss check. Ruling request in TODO.md; until it is answered the callback is not
    // installed and lua_tostringatom yields -1 for every string.
    return false;
}
