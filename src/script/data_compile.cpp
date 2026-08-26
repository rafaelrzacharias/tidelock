// ---------------------------------------------------------------------------------------------
// data_compile.cpp - the data-table compiler's body. Declared in core/data_tables.h (the
// assets+data lane's module); lives here because the module DAG only lets a module downstream of
// BOTH core and script drive the data VM the compiler needs (docs/ARCHITECTURE.md §1: "script":
// (script, core, ...) - core itself cannot include script/).
//
// RR-21 (TODO.md, ruled 2026-08-26, Rafael, relayed by the steward): script.h now exposes a
// generic C++-side Luau table reader (script_eval/script_table_get/script_table_geti/
// script_table_len/script_table_next). This compiler walks SCHEMA-ORDERED, by name
// (script_table_get) and by array position (script_table_geti/script_table_len) - never through
// script_table_next's own unordered walk, which is the ruling's binding determinism condition
// (docs/LUAU-LAYER.md §1: the data VM's output is hashed, so raw Luau iteration order must never
// reach it).
//
// Data-script SHAPE this pass builds against: a bare table-literal EXPRESSION (`{ materials =
// {...}, ... }`, no leading `return`) - script_eval's own expression-only shape
// ("return (<expr>)"); a "return {...}" statement chunk would need a separate exec-and-capture
// primitive nothing currently needs (script_run_source discards its result by design), so this
// pass refines docs/ASSETS-AND-DATA.md §3's "each returns a table" phrasing into the mechanism
// actually built (recorded in TODO.md's W3 assets+data lane notes).
//
// SCOPE THIS PASS SHIPS (recorded in TODO.md): integer/bool field kinds, `default_row` as the
// missing-field fallback. fx-literal fields (§7 R-2), StrId fields, handle/reference fields
// (§8.3 pass 2) and cross-table validators TL_FATAL, named - no Alloy schema exists anywhere in
// the tree yet (alloy-substrate is still queued, docs/ROADMAP.md §2) to compile a real one
// against.
// ---------------------------------------------------------------------------------------------
#include "core/data_tables.h"
#include "script/script.h"
#include "foundation/hash.h"
#include <string.h>

namespace {

bool kind_is_scoped_integer(FieldKind k) {
    return k == K_i8 || k == K_u8 || k == K_i16 || k == K_u16 || k == K_i32 || k == K_u32 ||
           k == K_i64 || k == K_u64 || k == K_bool;
}

// The [lo, hi] an integer FieldKind's stored width admits (docs/ASSETS-AND-DATA.md §8.3 "integer
// kinds require x == floor(x) and range"). K_bool is [0,1]; K_i64/K_u64 have no narrower range
// than a Luau number can carry exactly (+-2^53, already enforced by script_table_get's own
// ScriptValue conversion), so nothing here re-checks past kind matching for those two.
void integer_range(FieldKind k, i64* lo, i64* hi) {
    switch (k) {
        case K_i8:   *lo = -128;              *hi = 127;               return;
        case K_u8:   *lo = 0;                 *hi = 255;               return;
        case K_i16:  *lo = -32768;            *hi = 32767;             return;
        case K_u16:  *lo = 0;                 *hi = 65535;             return;
        case K_i32:  *lo = -2147483648LL;     *hi = 2147483647LL;      return;
        case K_u32:  *lo = 0;                 *hi = 4294967295LL;      return;
        case K_bool: *lo = 0;                 *hi = 1;                 return;
        default:     *lo = -9007199254740992LL; *hi = 9007199254740992LL; return;   // K_i64/K_u64
    }
}

void store_scalar(u8* dst, FieldKind k, i64 v) {
    switch (kind_scalar_size(k)) {
        case 1: { u8 x = (u8)v; memcpy(dst, &x, 1); return; }
        case 2: { u16 x = (u16)v; memcpy(dst, &x, 2); return; }
        case 4: { u32 x = (u32)v; memcpy(dst, &x, 4); return; }
        default: { u64 x = (u64)v; memcpy(dst, &x, 8); return; }
    }
}

// One field of one row: looks up row_ref[field->name] BY NAME, validates and writes field->size
// bytes at row_base + field->offset. Missing -> schema->default_row's bytes at the same offset,
// or ERR_DATA_MISSING_FIELD if the schema declares no default row.
ErrCode compile_field(ScriptVm* vm, ScriptTableRef row_ref, const ComponentInfo* schema,
                      const FieldInfo* f, u8* row_base) {
    u8* dst = row_base + f->offset;
    StrView field_name{ f->name, (u32)strlen(f->name) };
    Result<ScriptValue> v = script_table_get(vm, row_ref, field_name);
    if (v.err != ERR_OK) { return ERR_DATA_SCRIPT; }

    if (v.value.kind == SCRIPT_VAL_NIL) {
        if (schema->default_row == nullptr) { return ERR_DATA_MISSING_FIELD; }
        memcpy(dst, (const u8*)schema->default_row + f->offset, f->size);
        return ERR_OK;
    }

    if (kind_is_scoped_integer(f->kind)) {
        if (f->kind == K_bool) {
            if (v.value.kind != SCRIPT_VAL_BOOL) { return ERR_DATA_BAD_INT; }
        } else if (v.value.kind != SCRIPT_VAL_INT) {
            return ERR_DATA_BAD_INT;
        }
        i64 lo = 0, hi = 0;
        integer_range(f->kind, &lo, &hi);
        if (v.value.i < lo || v.value.i > hi) { return ERR_DATA_BAD_INT; }
        for (u16 e = 0; e < f->count; ++e) {
            store_scalar(dst + (u64)e * kind_scalar_size(f->kind), f->kind, v.value.i);
        }
        return ERR_OK;
    }
    // fx-literal rows (K_pos/K_vel/...), K_StrId, and handle/reference kinds: no consumer yet
    // (this file's top-of-file scope note) - TL_FATAL rather than silently write a wrong value.
    TL_FATAL("data_compile: this field kind is not yet supported - no Alloy schema exists yet to "
             "compile against (docs/ROADMAP.md §2)");
}

// Fetches `schema->table_name` from the first script root that has it (script_sources order).
struct TableLookup { ScriptTableRef ref; bool found; };

TableLookup find_table(ScriptVm* vm, Span<const ScriptTableRef> roots, StrView table_name) {
    for (u32 i = 0; i < roots.count; ++i) {
        Result<ScriptValue> v = script_table_get(vm, roots.data[i], table_name);
        if (v.err == ERR_OK && v.value.kind == SCRIPT_VAL_TABLE) {
            return TableLookup{ v.value.table, true };
        }
    }
    return TableLookup{ ScriptTableRef{}, false };
}

// Compiles every row of one table (array order - script_table_len/script_table_geti, never
// script_table_next, RR-21's binding condition) into `out`, backed by `arena`.
ErrCode compile_table(ScriptVm* vm, ScriptTableRef rows_ref, const TableSchema* schema, DataTable* out,
                      VMemArena* arena) {
    const u32 count = script_table_len(vm, rows_ref);
    if (count > schema->max_rows) { return ERR_DATA_TOO_MANY_ROWS; }

    out->schema = schema;
    out->rows = (u8*)arena_push(arena, (u64)schema->max_rows * schema->row->size, schema->row->align);
    out->count = count;
    sorted_map_init(&out->by_name, arena, schema->max_rows);

    for (u32 i = 0; i < count; ++i) {
        Result<ScriptValue> row_v = script_table_geti(vm, rows_ref, i + 1u);   // Luau arrays are 1-based
        if (row_v.err != ERR_OK) { return ERR_DATA_SCRIPT; }
        if (row_v.value.kind != SCRIPT_VAL_TABLE) { return ERR_DATA_MISSING_FIELD; }
        ScriptTableRef row_ref = row_v.value.table;

        Result<ScriptValue> name_v = script_table_get(vm, row_ref, sv("name"));
        if (name_v.err != ERR_OK) { return ERR_DATA_SCRIPT; }
        if (name_v.value.kind != SCRIPT_VAL_STRING) { return ERR_DATA_MISSING_FIELD; }

        u8* row_base = out->rows + (u64)i * schema->row->size;
        memset(row_base, 0, schema->row->size);
        for (u32 f = 0; f < schema->row->field_count; ++f) {
            const ErrCode fe = compile_field(vm, row_ref, schema->row, &schema->row->fields[f], row_base);
            if (fe != ERR_OK) { return fe; }
        }

        NameHash name_hash = fnv1a64(name_v.value.str, name_v.value.str_len);
        if (sorted_map_get(&out->by_name, name_hash) != nullptr) { return ERR_DATA_DUPLICATE_NAME; }
        sorted_map_put(&out->by_name, name_hash, (u16)i);
    }
    return ERR_OK;
}

}  // namespace

Result<DataTables*> data_compile(Span<const TableSchema> schemas, Span<const StrView> script_sources,
                                 VMemArena* perm, NameHash perm_id, const VMemApi* os,
                                 MemPool* compile_pool) {
    if (schemas.count > MAX_TABLES) { return Result<DataTables*>{ nullptr, ERR_DATA_TABLE_LIMIT }; }

    DataTables* out = (DataTables*)arena_push(perm, sizeof(DataTables), alignof(DataTables));
    memset(out, 0, sizeof(DataTables));
    const ErrCode ae = vmem_arena_init(&out->arena, perm_id, 16u * 1024u * 1024u, ARENA_ZERO_ON_PUSH, os);
    if (ae != ERR_OK) { return Result<DataTables*>{ nullptr, ae }; }

    ScriptVmDesc desc{};
    desc.pool_id = perm_id;
    desc.pool_reserve_bytes = 16u * 1024u * 1024u;
    desc.pool_budget_bytes = 8u * 1024u * 1024u;
    desc.budget_safepoints = 1000000u;
    desc.gc_step_kb = 0u;
    desc.interner = nullptr;    // §7 R-1: a throwaway VM per compile; data-script names are never
                                // process-interned
    desc.perm = perm;
    desc.os = os;
    desc.compile_pool = compile_pool;

    Result<ScriptVm*> vm_r = script_create_data(&desc);
    if (vm_r.err != ERR_OK) { return Result<DataTables*>{ nullptr, ERR_DATA_SCRIPT }; }
    ScriptVm* vm = vm_r.value;

    // Pass 1: evaluate every script source to its top-level table (§3 step 1).
    ScriptTableRef roots[MAX_TABLES];   // one script source per schema at most is a generous bound
    if (script_sources.count > MAX_TABLES) { script_destroy(vm); return Result<DataTables*>{ nullptr, ERR_DATA_SCRIPT }; }
    for (u32 i = 0; i < script_sources.count; ++i) {
        Result<ScriptValue> r = script_eval(vm, script_sources.data[i]);
        if (r.err != ERR_OK || r.value.kind != SCRIPT_VAL_TABLE) {
            script_destroy(vm);
            return Result<DataTables*>{ nullptr, ERR_DATA_SCRIPT };
        }
        roots[i] = r.value.table;
    }
    Span<const ScriptTableRef> roots_span{ roots, script_sources.count };

    // Pass 2: for each schema, in registration order, find its table and compile every row.
    for (u32 s = 0; s < schemas.count; ++s) {
        const TableSchema* schema = &schemas.data[s];
        TableLookup tl = find_table(vm, roots_span, schema->table_name);
        if (!tl.found) { script_destroy(vm); return Result<DataTables*>{ nullptr, ERR_DATA_UNKNOWN_TABLE }; }
        const ErrCode ce = compile_table(vm, tl.ref, schema, &out->t[s], &out->arena);
        if (ce != ERR_OK) { script_destroy(vm); return Result<DataTables*>{ nullptr, ce }; }
    }
    out->count = schemas.count;

    // hash = tl_hash64 over every table's rows, in registration order (§3 step 6). Names are
    // never in the hash - only the compiled POD bytes.
    u64 h = TL_HASH_SEED;
    for (u32 s = 0; s < out->count; ++s) {
        const DataTable* t = &out->t[s];
        h = tl_hash64(t->rows, (u64)t->count * t->schema->row->size, h);
    }
    out->hash = h;

    script_destroy(vm);
    return Result<DataTables*>{ out, ERR_OK };
}
