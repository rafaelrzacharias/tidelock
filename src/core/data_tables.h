#pragma once
// ---------------------------------------------------------------------------------------------
// data_tables.h - DataTables: Luau-authored tuning tables, compiled to POD in a registered arena.
//
// Spec: docs/ASSETS-AND-DATA.md §3 (design, DECIDED), §7 (rulings R-1/R-2), §8.1 (file layout),
//   §8.3 (this header, the build contract), §8.5 (tests); docs/CANON.md "resource handles"
//   (DataHandle's shape); docs/LUAU-LAYER.md §1/§10.6 (the data VM this compiler drives, and the
//   `data.*` read-only Luau binding that reads the tables THIS header produces - a DIFFERENT,
//   later, W3 luau-bindings-lane direction, not this file's).
// Purpose: Alloy and a game declare row schemas in C++ (TL_COMPONENT/TL_POOL_ROW) or Luau
//   (world_register_component_luau's ComponentInfo); a data script returns a Luau table per
//   schema; the compiler validates every row against the schema's FieldInfo (defaults, integer
//   range, fx-literal exactness, name references) and writes POD rows into a registered,
//   id-indexed, hashed arena. Two peers with different data cannot handshake (§3 step 4: the
//   table hash joins the build fingerprint).
// Invariants: DataTables.arena is HASHED|SNAPSHOT (§8.3); every Map/SortedMap living inside it
//   is therefore FIXED-CAPACITY (map.h's "a Map on a hashed arena cannot grow" rule) and sized
//   at data_compile time from each schema's max_rows. Rows are written zeroed-then-overlaid
//   (reflect.h's field table already asserts sizeof == sum of field sizes, so every pad reads
//   zero by construction, never by a separate rule here). data_compile is fail-loud: the first
//   ErrCode named table/row/field aborts the whole compile, no partial DataTables survives
//   (docs/CPP-SUBSET.md §3).
// Determinism: `hash` folds every table's rows in registration order (tl_hash64); NAMES are
//   never in the hash, only their resolved dense ids (§3 step 6) - two builds whose data scripts
//   use different table/row NAME BYTES but resolve to the same ids and rows hash identically,
//   which is intended (the hash is over sim-visible content). The data VM (LUAU-LAYER.md §1) is
//   a throwaway per compile - created, run, walked, destroyed (§7 R-1); its OUTPUT is hashed,
//   never the VM.
// Threading: data_compile runs at init (or a dev reload's sealed-command apply), single-threaded;
//   the resulting DataTables is read-only for the rest of the run (or until the next reload).
// Includes: core/reflect.h (ComponentInfo/FieldInfo - a table's row schema), foundation/{sorted,
//   vmem_arena,array,strview,handle}.h.
//
// NOT YET IMPLEMENTED (RR-21, TODO.md, filed this lane): the compiler needs a C++-side reader of
// a Luau table returned by a data script, and script.h (W2 luau-vm, merged, closed out) exposes
// no such call - only script_run_source (discards the result) and script_eval_int (one
// expression to i64). data_compile's BODY is a TL_FATAL stub until RR-21 resolves; this header's
// public surface is complete and does not depend on the resolution (ROADMAP.md §0 rule 1,
// header-first). Signature refined over §8.3's abbreviated pseudocode: `data_compile` takes the
// caller's ordered schema list directly (`Span<const TableSchema>`) rather than a separate
// stateful pre-registration API on a struct that does not exist until compile returns it -
// recorded in TODO.md, the same "signature added over spec" shape as slotmap_init/world_init.
// ---------------------------------------------------------------------------------------------
#include "core/reflect.h"
#include "foundation/sorted.h"
#include "foundation/vmem_arena.h"
#include "foundation/array.h"
#include "foundation/strview.h"
#include "foundation/handle.h"

// The data_tables module's ErrCode range is 0x034x (docs/CANON.md "Types": per-module ranges).
constexpr ErrCode ERR_DATA_TOO_MANY_ROWS   = (ErrCode)0x0340;  // a table's row count exceeds its schema's max_rows
constexpr ErrCode ERR_DATA_MISSING_FIELD   = (ErrCode)0x0341;  // a row field has neither a value nor a declared default
constexpr ErrCode ERR_DATA_BAD_INT         = (ErrCode)0x0342;  // an integer field's value is non-integral or out of range
constexpr ErrCode ERR_DATA_BAD_FX_LITERAL  = (ErrCode)0x0343;  // an fx field's literal is not exactly representable at the row quantum (§7 R-2)
constexpr ErrCode ERR_DATA_DANGLING_REF    = (ErrCode)0x0344;  // a name reference resolves to no row in the target table
constexpr ErrCode ERR_DATA_UNKNOWN_TABLE   = (ErrCode)0x0345;  // a data script names a table with no registered schema
constexpr ErrCode ERR_DATA_VALIDATOR       = (ErrCode)0x0346;  // a cross-table validator (ALLOY.md §11.1 / a game's own) rejected
constexpr ErrCode ERR_DATA_SCRIPT          = (ErrCode)0x0347;  // the data VM failed to run the script (compile/runtime error)
constexpr ErrCode ERR_DATA_TABLE_LIMIT     = (ErrCode)0x0348;  // more schemas were passed than MAX_TABLES

// Log-side name for a data-table ErrCode; "ERR_?" outside this header's codes.
constexpr const char* err_data_name(ErrCode e) {
    return e == ERR_OK ? "ERR_OK"
         : e == ERR_DATA_TOO_MANY_ROWS ? "ERR_DATA_TOO_MANY_ROWS"
         : e == ERR_DATA_MISSING_FIELD ? "ERR_DATA_MISSING_FIELD"
         : e == ERR_DATA_BAD_INT ? "ERR_DATA_BAD_INT"
         : e == ERR_DATA_BAD_FX_LITERAL ? "ERR_DATA_BAD_FX_LITERAL"
         : e == ERR_DATA_DANGLING_REF ? "ERR_DATA_DANGLING_REF"
         : e == ERR_DATA_UNKNOWN_TABLE ? "ERR_DATA_UNKNOWN_TABLE"
         : e == ERR_DATA_VALIDATOR ? "ERR_DATA_VALIDATOR"
         : e == ERR_DATA_SCRIPT ? "ERR_DATA_SCRIPT"
         : e == ERR_DATA_TABLE_LIMIT ? "ERR_DATA_TABLE_LIMIT" : "ERR_?";
}

// DataHandle: the resource-handle shape (docs/CANON.md "Types": Handle<_, 12, 4>, u16, listed
// among "textures, fonts, audio clips, clips, data tables"). Addressing scheme (a single global
// row space vs. a per-table dense id widened into this shape) is a data_compile-body decision,
// left open until RR-21 unblocks it - the TYPE and its wire/reflection shape do not depend on it.
struct DataTag;
typedef Handle<struct DataTag, 12, 4> DataHandle;
static_assert(sizeof(DataHandle) == 2, "docs/CANON.md: resource handles are u16");
constexpr FieldKind tl_field_kind_DataHandle = K_Data;

enum : u32 { MAX_TABLES = 64 };   // docs/ASSETS-AND-DATA.md §8.3

// TableSchema.flags - reserved (0 at rev 1); a bit here gates future per-table compiler behaviour
// (e.g. "this table's rows may be empty at v0", "no cross-table validators run for it") without
// widening TableSchema's own shape when the first real flag lands.
enum : u32 { DATA_SCHEMA_FLAGS_NONE = 0 };

// One table's row schema (docs/ASSETS-AND-DATA.md §8.3, field for field): `row` is a normal
// ComponentInfo* from a C++ TL_COMPONENT/TL_POOL_ROW declaration (Alloy's SolidMaterial etc.,
// docs/ALLOY.md §11) or a Luau-declared one (world_register_component_luau's return, when a game
// wants its tuning schema declared in Luau too - docs/ECS.md §6.1). `table_name` is the Luau
// table key the compiler looks the array up by (`{ <table_name> = {...} }`, §3 step 1).
struct TableSchema {
    const ComponentInfo* row;
    NameHash              table_name;
    u32                   max_rows;
    u32                   flags;
};

// One compiled table (docs/ASSETS-AND-DATA.md §8.3, field for field): `rows` is `count` packed
// instances of `schema->row`'s struct (stride == schema->row->size), pushed from DataTables.arena;
// `by_name` resolves a Luau-facing row name to its dense id (§8.3 step 3's `data.id`/`data.row`
// consumer, docs/LUAU-LAYER.md §10.6).
struct DataTable {
    const TableSchema*     schema;
    u8*                    rows;
    u32                    count;
    u32                    _pad0;
    SortedMap<NameHash, u16> by_name;
};

// The compiled set (docs/ASSETS-AND-DATA.md §8.3, field for field). `arena` is registered
// HASHED|SNAPSHOT by the caller (data_compile does not register it itself - registration order
// is part of the lockstep contract, docs/MEMORY.md §1.2, and only the caller knows where in that
// order this DataTables belongs). `hash` is tl_hash64 over every table's rows in registration
// order (§3 step 6); it is one input of session_fingerprint (docs/BUILD.md §5).
struct DataTables {
    VMemArena  arena;
    DataTable  t[MAX_TABLES];
    u32        count;
    u32        _pad0;
    u64        hash;
};

// Creates the data VM (docs/LUAU-LAYER.md §1), runs each of `script_paths` in order, and for
// every schema in `schemas` (registration order): fetches the Luau array named `schema->table_name`
// from whichever script defined it (ERR_DATA_UNKNOWN_TABLE if none did and the schema has no
// declared-empty allowance); walks each row against `schema->row`'s FieldInfo (missing field ->
// its declared default or ERR_DATA_MISSING_FIELD; integer kinds range-checked, ERR_DATA_BAD_INT;
// fx kinds accept only an exactly-representable literal or `fx.raw(bits)`, ERR_DATA_BAD_FX_LITERAL
// - §7 R-2; handle/ref kinds resolve by name in pass 2, ERR_DATA_DANGLING_REF); runs every
// registered cross-table validator (ERR_DATA_VALIDATOR names table/row/field); computes `hash`;
// destroys the data VM. The first error anywhere aborts with nothing written - no partial
// DataTables survives a failed compile (docs/CPP-SUBSET.md §3). `perm` backs the returned
// DataTables* and its `arena`'s reserve (a permanent, caller-owned arena - e.g. World::meta);
// `perm_id` names `arena` for the caller's later registry_add call. ERR_DATA_TABLE_LIMIT if
// `schemas.count > MAX_TABLES`; ERR_DATA_SCRIPT if any script fails to run.
//
// NOT YET IMPLEMENTED (RR-21): the body is a TL_FATAL("unimplemented - RR-21, see TODO.md") stub
// until the data VM gains a C++-side table reader (script.h, a different lane's module).
Result<DataTables*> data_compile(Span<const TableSchema> schemas, Span<const StrView> script_paths,
                                 VMemArena* perm, NameHash perm_id, const VMemApi* os);

// The dense id for `name` in table `t`, or DataHandle{} (null) if absent. Pure, all tiers.
DataHandle data_find_row(const DataTable* t, NameHash name);

// The row pointer for a dense id, or null if `id`'s index is >= t->count (TL_CHECK bounds it -
// caller-input validation, since a dense id can arrive from a save file or a Luau reference).
// Cast to the schema's row type at the call site (`(const SolidMaterial*)data_row(t, id)`).
const void* data_row(const DataTable* t, DataHandle id);
