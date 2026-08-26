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
#include "vendor_glue/vendor_new.h"

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
        d->pool_budget_bytes > d->pool_reserve_bytes || d->compile_pool == nullptr) {
        return Result<ScriptVm*>{ nullptr, ERR_SCRIPT_BAD_ARG };
    }

    ScriptVm* vm = (ScriptVm*)arena_push(d->perm, (u64)sizeof(ScriptVm), 16u);
    if (vm == nullptr) return Result<ScriptVm*>{ nullptr, ERR_SCRIPT_OOM };
    memset(vm, 0, sizeof(ScriptVm));
    vm->kind = kind;
    vm->interner = d->interner;
    vm->compile_pool = d->compile_pool;
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
    script_install_useratom(vm->L, d->interner);   // RR-19, ruled 2026-08-26 (atom.cpp)

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
    lua_CompileOptions opts = script_compile_options();
    size_t bc_size = 0;
    // RR-18, as amended by review round 1's D2 ruling (2026-08-26, Rafael): Luau's Compiler has
    // no allocator hook and allocates with global operator new, and the window below serves those
    // allocations from the SHARED VENDOR POOL - `PLATFORM.md` §9.5's pool_vendor - not from this
    // VM's own pool. Two reasons, both measured rather than argued:
    //   - the two allocators drawing on one pool had OPPOSITE failure semantics. tl_luau_alloc
    //     returns null over budget, which Luau turns into a recoverable ERR_SCRIPT_RUNTIME (the
    //     memory_exhaustion row pins that as the contract); vendor_alloc TL_FATALs. A sim VM with
    //     a 512 KB budget compiling a 200 KB source killed the process, in every tier including
    //     ship, from a path fed by files on disk.
    //   - drawing from the live VM's pool made the trip point a function of runtime heap
    //     occupancy: a chunk that compiled at startup could kill the process when reloaded after
    //     the heap had grown, with reproducibility depending on GC state.
    // The headroom check makes the refusal an ErrCode, so vendor_alloc's fatal - which is correct
    // for a genuine bug - is not the only outcome available for an ordinary large source.
    MemPool* cpool = vm->compile_pool;
    const MemPoolStats* cst = pool_stats(cpool);
    const u64 want = SCRIPT_COMPILE_HEADROOM_MIN +
                     (u64)source.len * SCRIPT_COMPILE_BYTES_PER_SRC_BYTE;
    // Against CARVED bytes, not live ones - that is what pool_alloc enforces
    // (`carved_bytes + bytes > budget_bytes`), and pool_free returns a small-class block to its
    // per-class freelist WITHOUT decrementing carved_bytes. Freelists never split across classes,
    // so carved-but-free bytes in one class cannot serve a request in another: `budget - live`
    // overstates the room a compile can take, without bound, as a shared pool ages. Review round
    // 2 reproduced the original TL_FATAL with this check reporting 33.6 MB of headroom against
    // 40 KB of real carve room. Ignoring freelist reuse is conservative, which is the correct
    // direction for a refusal gate: it can refuse a compile that might have fit, never admit one
    // that cannot.
    const u64 have = cpool->budget_bytes > cpool->carved_bytes
                   ? cpool->budget_bytes - cpool->carved_bytes : 0;
    if (have < want) {
        return script_set_error(vm, ERR_SCRIPT_COMPILE,
                                "the shared vendor pool has too little headroom to compile this "
                                "source (docs/MEMORY.md section 1.5)");
    }
    const u64 live_before = cst->live_bytes;
    vendor_heap_install(cpool);
    char* bc = luau_compile(source.ptr, (size_t)source.len, &opts, &bc_size);
    vendor_heap_install(nullptr);
    // What the window itself cost, for the test that guards RR-18's headline mechanism. Measured
    // HERE and not across script_run_source because that call also loads and runs, and both of
    // those go through tl_luau_alloc - review round 1 (D1) showed the confound is larger than the
    // signal, so a floor measured against the whole call cannot discriminate. And taken from the
    // WINDOW's own high-water counter rather than the pool's peak_bytes, which is a LIFETIME mark
    // a returning window can never raise twice: review round 2 measured 32,992 / 0 / 0 across
    // three compiles on one pool, which on a long-lived pool_vendor is 0 essentially always.
    vm->last_compile_bytes = vendor_heap_window_peak();
    TL_ASSERT(pool_stats(cpool)->live_bytes == live_before);
    (void)live_before;
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

namespace {

// Converts the Luau value at stack index `idx` into a tagged ScriptValue (RR-21). A table is
// PINNED via lua_ref (non-destructive of the stack - lua_ref reads by index, docs/LUAU-LAYER.md
// §10.6's own `lua_ref(L, fn_idx)` shape); every other case copies out and leaves the stack
// untouched too. The caller pops `idx` itself once done with it.
ErrCode to_script_value(ScriptVm* vm, int idx, ScriptValue* out) {
    lua_State* L = vm->L;
    memset(out, 0, sizeof(ScriptValue));
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            out->kind = SCRIPT_VAL_NIL;
            return ERR_OK;
        case LUA_TBOOLEAN:
            out->kind = SCRIPT_VAL_BOOL;
            out->i = lua_toboolean(L, idx) ? 1 : 0;
            return ERR_OK;
        case LUA_TNUMBER: {
            const double x = lua_tonumber(L, idx);
            // Exactness, not truncation - the same rule script_eval_int enforces.
            if (!(x >= -9007199254740992.0 && x <= 9007199254740992.0) || x != (double)(i64)x) {
                return script_set_error(vm, ERR_SCRIPT_RUNTIME,
                                        "value is not an exact integer within +-2^53");
            }
            out->kind = SCRIPT_VAL_INT;
            out->i = (i64)x;
            return ERR_OK;
        }
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            if (len >= (size_t)SCRIPT_VALUE_STR_MAX) {
                return script_set_error(vm, ERR_SCRIPT_RUNTIME,
                                        "string value exceeds SCRIPT_VALUE_STR_MAX");
            }
            memcpy(out->str, s, len);
            out->str[len] = 0;
            out->str_len = (u32)len;
            out->kind = SCRIPT_VAL_STRING;
            return ERR_OK;
        }
        case LUA_TTABLE:
            out->kind = SCRIPT_VAL_TABLE;
            out->table.ref = lua_ref(L, idx);
            return ERR_OK;
        default:
            return script_set_error(vm, ERR_SCRIPT_RUNTIME, luaL_typename(L, idx));
    }
}

}  // namespace

Result<ScriptValue> script_eval(ScriptVm* vm, StrView expr) {
    TL_ASSERT(vm != nullptr && vm->L != nullptr);
    script_clear_error(vm);
    ScriptValue zero{};
    if (expr.ptr == nullptr || expr.len == 0) {
        return Result<ScriptValue>{ zero, script_set_error(vm, ERR_SCRIPT_BAD_ARG, "script_eval: empty expression") };
    }
    // "return (<expr>)" - script_eval_int's own trick, duplicated rather than factored out of
    // already-shipped, tested code for a one-lane scoped exception (RR-21).
    char buf[SCRIPT_ERR_MAX];
    const char* prefix = "return (";
    u32 n = 0;
    while (prefix[n] != 0) { buf[n] = prefix[n]; ++n; }
    if (expr.len + n + 2u >= (u32)sizeof(buf)) {
        return Result<ScriptValue>{ zero, script_set_error(vm, ERR_SCRIPT_BAD_ARG, "script_eval: expression too long") };
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
        return Result<ScriptValue>{ zero, le };
    }
    const int rc = lua_pcall(L, 0, 1, errfunc);
    lua_remove(L, errfunc);
    if (rc != 0) return Result<ScriptValue>{ zero, take_error(vm, ERR_SCRIPT_RUNTIME) };

    ScriptValue out{};
    const ErrCode ce = to_script_value(vm, -1, &out);
    lua_pop(L, 1);
    if (ce != ERR_OK) return Result<ScriptValue>{ zero, ce };
    return Result<ScriptValue>{ out, ERR_OK };
}

Result<ScriptValue> script_table_get(ScriptVm* vm, ScriptTableRef t, StrView key) {
    TL_ASSERT(vm != nullptr && vm->L != nullptr);
    script_clear_error(vm);
    ScriptValue zero{};
    lua_State* L = vm->L;
    lua_getref(L, t.ref);
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        return Result<ScriptValue>{ zero, script_set_error(vm, ERR_SCRIPT_BAD_ARG, "script_table_get: not a table") };
    }
    lua_pushlstring(L, key.ptr, (size_t)key.len);
    lua_gettable(L, -2);
    ScriptValue out{};
    const ErrCode ce = to_script_value(vm, -1, &out);
    lua_pop(L, 2);   // the looked-up value, then the table itself
    if (ce != ERR_OK) return Result<ScriptValue>{ zero, ce };
    return Result<ScriptValue>{ out, ERR_OK };
}

Result<ScriptValue> script_table_geti(ScriptVm* vm, ScriptTableRef t, u32 index) {
    TL_ASSERT(vm != nullptr && vm->L != nullptr);
    script_clear_error(vm);
    ScriptValue zero{};
    lua_State* L = vm->L;
    lua_getref(L, t.ref);
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        return Result<ScriptValue>{ zero, script_set_error(vm, ERR_SCRIPT_BAD_ARG, "script_table_geti: not a table") };
    }
    lua_rawgeti(L, -1, (int)index);
    ScriptValue out{};
    const ErrCode ce = to_script_value(vm, -1, &out);
    lua_pop(L, 2);
    if (ce != ERR_OK) return Result<ScriptValue>{ zero, ce };
    return Result<ScriptValue>{ out, ERR_OK };
}

u32 script_table_len(ScriptVm* vm, ScriptTableRef t) {
    TL_ASSERT(vm != nullptr && vm->L != nullptr);
    lua_State* L = vm->L;
    lua_getref(L, t.ref);
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        return 0u;
    }
    const u32 n = (u32)lua_objlen(L, -1);
    lua_pop(L, 1);
    return n;
}

namespace {

// Pushes back a previously-yielded key (NIL/BOOL/INT/STRING only - script_table_next's own
// contract refuses a table key rather than risk desyncing lua_next's walk on a reconstructed
// copy). Returns false, pushing nothing, for an unsupported kind.
bool push_script_value_key(lua_State* L, const ScriptValue* key) {
    switch (key->kind) {
        case SCRIPT_VAL_NIL:    lua_pushnil(L); return true;
        case SCRIPT_VAL_BOOL:   lua_pushboolean(L, key->i != 0); return true;
        case SCRIPT_VAL_INT:    lua_pushnumber(L, (double)key->i); return true;
        case SCRIPT_VAL_STRING: lua_pushlstring(L, key->str, (size_t)key->str_len); return true;
        default: return false;
    }
}

}  // namespace

bool script_table_next(ScriptVm* vm, ScriptTableRef t, ScriptValue* key, ScriptValue* out_value) {
    TL_ASSERT(vm != nullptr && vm->L != nullptr && key != nullptr && out_value != nullptr);
    script_clear_error(vm);
    lua_State* L = vm->L;
    lua_getref(L, t.ref);
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        (void)script_set_error(vm, ERR_SCRIPT_BAD_ARG, "script_table_next: not a table");
        return false;
    }
    // lua_next's protocol: the key to continue from must be on the stack (nil to begin); it pops
    // that key and pushes the next key/value pair, or pushes nothing and returns 0 when done.
    // The cursor is carried as a VALUE (`*key`), not a live stack slot, because this VM may run
    // other Luau code between two separate calls into this function (nothing here assumes it does
    // not) - so the key is genuinely round-tripped through Luau every call, never left parked.
    if (!push_script_value_key(L, key)) {
        lua_pop(L, 1);   // the table
        (void)script_set_error(vm, ERR_SCRIPT_RUNTIME, "script_table_next: table keys are not supported");
        return false;
    }
    const int has_more = lua_next(L, -2);
    if (!has_more) {
        lua_pop(L, 1);   // the table
        return false;
    }
    ScriptValue new_key{};
    ErrCode key_err = to_script_value(vm, -2, &new_key);
    (void)to_script_value(vm, -1, out_value);   // a value error (e.g. a function) still returns
                                          // true - the caller sees the VM's last_error, matching
                                          // script_table_get's per-value error surface
    lua_pop(L, 3);   // value, key, table
    // to_script_value succeeds (tags SCRIPT_VAL_TABLE) for a table key, which push_script_value_key
    // cannot round-trip - reject it here explicitly rather than silently stalling next call, and
    // release the ref to_script_value just pinned so refusing does not leak it.
    if (new_key.kind == SCRIPT_VAL_TABLE) {
        script_table_unref(vm, new_key.table);
        key_err = script_set_error(vm, ERR_SCRIPT_RUNTIME, "script_table_next: table keys are not supported");
    }
    if (key_err != ERR_OK) {
        // A Luau table key, or a key outside NIL/BOOL/INT/STRING/TABLE (script_table_next's own
        // closed set): named here, at the point the walk would otherwise silently stall next call.
        return false;
    }
    *key = new_key;
    return true;
}

void script_table_unref(ScriptVm* vm, ScriptTableRef t) {
    TL_ASSERT(vm != nullptr && vm->L != nullptr);
    if (t.ref == LUA_NOREF || t.ref == LUA_REFNIL) return;
    lua_unref(vm->L, t.ref);
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
    // (void)vm OUTSIDE the #if: the whole body below is dev-only and TL_ASSERT compiles out in
    // netcode/ship, so `vm` is an unused parameter there and -Werror rejects the TU on exactly
    // the two tiers dev and debug cannot see (docs/LESSONS.md - the same class that turned 11 of
    // 23 CI legs red once already). The function doing nothing in those tiers is correct: the
    // leak heuristic is a dev tool and its cost is not paid where it cannot be read.
    (void)vm;
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

i32 script_atom_of(ScriptVm* vm, StrView s) {
    TL_ASSERT(vm != nullptr && vm->L != nullptr);
    if (s.ptr == nullptr) return -1;
    lua_State* L = vm->L;
    lua_pushlstring(L, s.ptr, (size_t)s.len);
    int atom = -1;
    (void)lua_tostringatom(L, -1, &atom);
    lua_pop(L, 1);
    return (i32)atom;
}

u64 script_last_compile_bytes(const ScriptVm* vm) {
    TL_ASSERT(vm != nullptr);
    return vm->last_compile_bytes;
}

bool script_useratom_installed(void) {
    // TRUE once any VM has been created with an interner (RR-19, ruled 2026-08-26): Luau's
    // callback carries no context, so the interner is reached through the one pointer named in
    // tools/audit/static_allow.txt. A VM created with a null interner leaves it uninstalled and
    // lua_tostringatom yields -1 - the honest state for a program with no name registry.
    return script_atom_interner() != nullptr;
}
