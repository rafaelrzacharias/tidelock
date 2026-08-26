#pragma once
// ---------------------------------------------------------------------------------------------
// save.h - the durable save file: header + name table + per-arena blocks over core/encoder.h.
//
// Spec: docs/ASSETS-AND-DATA.md §5 (design, DECIDED - M2), §8.1 (file layout), §8.4 (this header,
//   the byte layout), §8.5 (tests); docs/CPP-SUBSET.md §9 R-2 (TL_WIRE_STRUCT); docs/MEMORY.md §5
//   (contrast: the raw memcpy snapshot, a different mechanism for rollback within one build).
// Purpose: a save survives a rebuild and a schema edit. `core/encoder.h` (W2 ecs, merged) already
//   implements the per-payload engine - the name-keyed field match, ALIAS resolution, declared
//   defaults, and the kind/count-change refusal; this file owns the FILE: header, interned name
//   table, per-arena block framing, the alias/migration registries, the crc32 trailer, and
//   write-temp/fsync/rename via platform->file.write_atomic (docs/PLATFORM.md §9.3).
// Invariants: encode writes are ATOMIC at the file level (os_write_atomic's contract: on any
//   failure the temp file is deleted and the target is untouched) but not at the field level -
//   an in-memory encode failure (a caller bug: exceeding ENC_MAX_FIELDS, mismatched arena_descs)
//   is TL_FATAL, never a partial file. Fail-loud decode: every reject is a named LoadError; no
//   silent partial load (docs/ASSETS-AND-DATA.md §5).
// Determinism: not part of the deterministic sim path (a save is disk io); pure functions of the
//   registered arenas' bytes and the caller-supplied SaveArenaDesc table, no floats, no clock
//   read except by the caller (this header never stamps a wall-clock time).
// Threading: one save/load call at a time; the arenas it reads/writes must not be mutated
//   concurrently (the same single-writer contract every registered arena already carries).
// Includes: core/encoder.h (the per-payload engine), core/world.h (Entity, the ECS-column re-add
//   door for load), foundation/{arena_registry,crc32,bytes,interner,strview,array}.h, platform.h
//   (FileApi).
//
// SCOPE THIS LANE SHIPS (recorded in TODO.md, W3 assets+data lane notes): SAVE_ENC_REFLECTED and
// SAVE_ENC_ECS_COLUMN are implemented and tested - the only two encoder kinds with a real,
// buildable consumer today (every registered arena in the tree right now is either an ECS column
// or a plain reflected singleton). SAVE_ENC_RAW_POOL (Alloy pool rows) and SAVE_ENC_CHUNK_STORE
// (Alloy terrain) TL_FATAL("not yet built - no pool/chunk-store consumer exists yet") - Alloy has
// not landed a single pool (docs/ROADMAP.md §2, alloy-substrate is still queued); building their
// save path against a guessed layout would be the Layr trap (CLAUDE.md, speculative breadth).
// Data-table save/reload (§5's "the save records the data-script names + their hash") is
// signature-complete here (`SaveDesc::data_script_names`/`data_hash`) but its round-trip test is
// blocked on RR-21 (data_compile has no working body yet, so there is nothing to recompile
// against on load) - the header carries the field, save_write/save_read pass it through
// unconditionally, and the recompile-and-check step is a TL_FATAL stub pending that ruling.
// The NameTable (§8.4: "every interned name referenced by a StrId field") is written empty
// (name_table_len 0) at this cut: no shipped component has a StrId field yet to exercise it
// against, and the real mechanism needs a decode-side StrId REMAP (the writer's interner and the
// reader's interner can assign the same string different ids), which is more than a table to
// write - a half-built version (scan and write, no remap) would silently ship a save that reads
// back the wrong string on a different process. save_write TL_FATALs if it ever meets a K_StrId
// field, rather than encode one it cannot correctly decode.
// ---------------------------------------------------------------------------------------------
#include "core/encoder.h"
#include "core/world.h"
#include "foundation/arena_registry.h"
#include "foundation/crc32.h"
#include "foundation/bytes.h"
#include "foundation/interner.h"
#include "foundation/strview.h"
#include "foundation/array.h"
#include "platform/platform.h"

// The save module's ErrCode range is 0x035x (docs/CANON.md "Types": per-module ranges).
constexpr ErrCode ERR_SAVE_BAD_MAGIC     = (ErrCode)0x0350;  // header magic mismatch - not a tidelock save file
constexpr ErrCode ERR_SAVE_VERSION       = (ErrCode)0x0351;  // format_version newer than this build knows
constexpr ErrCode ERR_SAVE_TRUNCATED     = (ErrCode)0x0352;  // file shorter than its own header/block lengths claim
constexpr ErrCode ERR_SAVE_CRC_MISMATCH  = (ErrCode)0x0353;  // trailer crc32 does not match the decoded bytes
constexpr ErrCode ERR_SAVE_ARENA_MISSING = (ErrCode)0x0354;  // a stored ArenaBlock names an id with no SaveArenaDesc entry
constexpr ErrCode ERR_SAVE_FIELD_KIND    = (ErrCode)0x0355;  // a stored field's kind/count changed with no migration fn registered (wraps ERR_ENC_FIELD_KIND)
constexpr ErrCode ERR_SAVE_DATA_MISMATCH = (ErrCode)0x0356;  // recompiled data-table hash disagrees with the stored one
constexpr ErrCode ERR_SAVE_IO            = (ErrCode)0x0357;  // wraps a platform FileApi failure (write_atomic/read_all)
constexpr ErrCode ERR_SAVE_TOO_MANY_ARENAS = (ErrCode)0x0358; // more arena_descs entries than MAX_ARENAS

// Log-side name for a save ErrCode; "ERR_?" outside this header's codes.
constexpr const char* err_save_name(ErrCode e) {
    return e == ERR_OK ? "ERR_OK"
         : e == ERR_SAVE_BAD_MAGIC ? "ERR_SAVE_BAD_MAGIC"
         : e == ERR_SAVE_VERSION ? "ERR_SAVE_VERSION"
         : e == ERR_SAVE_TRUNCATED ? "ERR_SAVE_TRUNCATED"
         : e == ERR_SAVE_CRC_MISMATCH ? "ERR_SAVE_CRC_MISMATCH"
         : e == ERR_SAVE_ARENA_MISSING ? "ERR_SAVE_ARENA_MISSING"
         : e == ERR_SAVE_FIELD_KIND ? "ERR_SAVE_FIELD_KIND"
         : e == ERR_SAVE_DATA_MISMATCH ? "ERR_SAVE_DATA_MISMATCH"
         : e == ERR_SAVE_IO ? "ERR_SAVE_IO"
         : e == ERR_SAVE_TOO_MANY_ARENAS ? "ERR_SAVE_TOO_MANY_ARENAS" : "ERR_?";
}

constexpr u32 SAVE_FORMAT_VERSION = 1;
constexpr u8 SAVE_MAGIC[4] = { 'T', 'L', 'S', 'V' };

// docs/ASSETS-AND-DATA.md §8.4's four encoder kinds. RAW_POOL/CHUNK_STORE are declared (the byte
// layout names them) but not yet implemented - see this header's top-of-file scope note.
enum SaveEncoderKind : u8 { SAVE_ENC_RAW_POOL = 0, SAVE_ENC_REFLECTED = 1, SAVE_ENC_ECS_COLUMN = 2, SAVE_ENC_CHUNK_STORE = 3 };

// SaveHeader (docs/ASSETS-AND-DATA.md §8.4, "SaveHeader (WIRE_STRUCT, 160 B)"). TL_WIRE_STRUCT
// prepends `format_version` as field 0 (docs/CPP-SUBSET.md §9 R-2) - the doc's own header field
// list already opens with it, so TL_FIELDS_SaveHeader does not repeat it. `_pad0` is reserved
// headroom to round the header to the doc's stated 160 B, written zeroed and refused nonzero on
// read (the wire door's own pad rule, core/reflect.h's tl_wire_get_row).
#define TL_FIELDS_SaveHeader(X, XA, XH) \
    XA(u8, magic, 4) \
    XA(u8, build_id, 32) \
    XA(u8, session_fingerprint, 32) \
    X(u64, seed) \
    X(u64, tick) \
    X(u32, session_model) \
    X(u32, origin) \
    X(u32, name_table_len) \
    X(u32, arena_count) \
    X(u32, flags) \
    XA(u8, _pad0, 52)
#define TL_OFFSETS_SaveHeader(XO) \
    XO(magic, 4) \
    XO(build_id, 8) \
    XO(session_fingerprint, 40) \
    XO(seed, 72) \
    XO(tick, 80) \
    XO(session_model, 88) \
    XO(origin, 92) \
    XO(name_table_len, 96) \
    XO(arena_count, 100) \
    XO(flags, 104) \
    XO(_pad0, 108)
TL_WIRE_STRUCT(SaveHeader)
static_assert(sizeof(SaveHeader) == 160, "docs/ASSETS-AND-DATA.md section 8.4 pins this struct's size");

// SaveHeader.origin: the loaded-across-builds distinction the reader's header check names
// (docs/ASSETS-AND-DATA.md §8.4: "build_id differences allowed, session_fingerprint differences
// allowed - this is the cross-build path"). Not yet a closed enum with more than one member; a
// second origin lands with the netcode checkpoint-vs-save split (docs/NETCODE.md §11) consumer.
enum : u32 { SAVE_ORIGIN_LOCAL = 0 };

// A rename entry (docs/ASSETS-AND-DATA.md §5: "alias entries for renames" - the same FieldAlias
// shape encoder.h already defines; save.h owns WHERE the table lives (registered per component,
// by the caller, before load) and HOW it is looked up, not the shape.
struct SaveComponentAliases {
    NameHash                component_name_hash;
    Span<const FieldAlias>  aliases;
};

// A versioned migration function (docs/ASSETS-AND-DATA.md §5/§8.4: "a kind change is an explicit,
// versioned migration function in C++, keyed by format_version - no generic migrate(T) magic").
// Runs INSTEAD of encoder_read_rows/encoder_read_column for the named component when the stored
// SaveHeader.format_version matches `from_version`; converts the raw stored bytes (still in the
// stored field order/kinds - the fn gets the ByteReader positioned at the row payload's start and
// the stored field table encoder_read_rows would have used) into `max_rows` live-shaped rows.
// Returns the row count decoded, same contract as encoder_read_rows.
typedef Result<u32> (*SaveMigrateFn)(ByteReader* r, const ComponentInfo* live_info,
                                     void* out_rows, u32 max_rows);

struct SaveComponentMigration {
    NameHash       component_name_hash;
    u32            from_version;
    SaveMigrateFn  fn;
};

// One registered arena's save shape (this lane's own construction - the doc gives the file
// FORMAT, not how a caller maps its registered arenas to it; recorded in TODO.md as a "signature
// added over spec", the slotmap_init/world_init precedent). `info`/`max_rows` are required for
// REFLECTED and ECS_COLUMN, ignored for RAW_POOL/CHUNK_STORE (not yet implemented - TL_FATAL if
// selected). `world`/`comp` are required only for ECS_COLUMN (the load-side re-add door).
struct SaveArenaDesc {
    NameHash              arena_id;      // must match the ArenaRegistry entry's id
    SaveEncoderKind        kind;
    u8                     _pad0[3];
    const ComponentInfo*   info;         // REFLECTED / ECS_COLUMN only
    u32                    max_rows;     // decode buffer sizing, REFLECTED / ECS_COLUMN only
    ComponentId             comp;         // ECS_COLUMN only: world_add_raw's target component
};

// Everything one save call needs beyond the registry/header (this lane's own construction, same
// reasoning as SaveArenaDesc). `world` is required when any `arena_descs` entry is SAVE_ENC_
// ECS_COLUMN (load re-adds rows through world_add_raw, docs/ECS.md §4); may be null otherwise.
// `data_script_names`/`data_hash`: docs/ASSETS-AND-DATA.md §5 "data tables are not stored: the
// save records the data-script names + their hash; on load the tables are recompiled and the
// hash checked" - carried here, recompile-and-check is a TL_FATAL stub pending RR-21.
struct SaveDesc {
    const ArenaRegistry*          registry;
    Span<const SaveArenaDesc>     arena_descs;
    Span<const SaveComponentAliases>   aliases;      // load only; may be empty
    Span<const SaveComponentMigration> migrations;   // load only; may be empty
    World*                        world;             // required iff any arena_desc is ECS_COLUMN
    Interner*                     interner;          // names the NameTable interns through
    Span<const StrView>           data_script_names; // §5; RR-21 blocks the recompile-and-check step
    u64                           data_hash;
    u64                           seed;
    u64                           tick;
    u8                            build_id[32];             // docs/BUILD.md §5; zeroed if the caller has none yet
    u8                            session_fingerprint[32];   // docs/BUILD.md §5; zeroed if the caller has none yet
};

// Encodes header + name table + one ArenaBlock per SAVE-flagged registry entry `desc` covers,
// crc32 trailer, then writes the whole buffer via platform->file.write_atomic (tmp-write ->
// fsync -> rename, docs/PLATFORM.md §9.3 - the target is either the old content or the new
// content, never torn). `scratch` backs the in-memory encode (sized by the caller; TL_FATAL on
// overflow, a caller bug, never a partial file - the write only happens once encoding fully
// succeeds). One block per `arena_descs` ENTRY (not per underlying registered arena - an ECS
// column is three registry entries, docs/ECS.md §10.3, but one `encoder_write_column` call and
// one block; `arena_descs` is the save's actual membership list, not "everything SNAPSHOT-
// flagged"). ERR_SAVE_TOO_MANY_ARENAS if `arena_descs.count > MAX_ARENAS`; ERR_SAVE_IO wraps a
// write_atomic failure.
ErrCode save_write(const SaveDesc* desc, const PlatformApi* platform, StrView path,
                   VMemArena* scratch);

// Reads `path` via platform->file.read_all into `scratch`, checks magic/version (newer than
// SAVE_FORMAT_VERSION -> ERR_SAVE_VERSION), verifies the crc32 trailer over everything after the
// header (ERR_SAVE_CRC_MISMATCH), decodes the name table, then per ArenaBlock: finds the matching
// `arena_descs` entry (ERR_SAVE_ARENA_MISSING if none), and dispatches to a registered
// SaveComponentMigration (by component name hash + the FILE's format_version) if one matches,
// else encoder_read_rows/encoder_read_column with the component's `aliases` entry (empty span if
// none registered) - ERR_SAVE_FIELD_KIND wraps ERR_ENC_FIELD_KIND. ECS_COLUMN rows re-add through
// world_add_raw, which TL_CHECKs the target entity live and the component absent (docs/ECS.md
// §4) - v1's ECS_COLUMN restore therefore assumes the caller's entities already exist with the
// right identity (an in-session save/reload: add the component, save, remove it, reload). Cross-
// session entity identity (spawning fresh entities for rows whose original Entity no longer
// exists) has no consumer yet and is not this version's job - a save whose row count does not
// match live, component-absent entities is the caller's TL_CHECK to hit, not a silent skip.
// `world_add_raw` only RECORDS the command (docs/ECS.md §4) - the caller must call
// `world_flush(desc->world)` (or run a phase barrier) after `save_read` returns before the
// restored rows are visible to `world_get`/`world_column`.
// Writes the decoded seed/tick
// into `out_seed`/`out_tick`. Nothing is applied to `desc->world` until every block has decoded
// successfully - a truncated or malformed file is refused with the World untouched
// (docs/ASSETS-AND-DATA.md §5 "no silent partial loads").
ErrCode save_read(const SaveDesc* desc, const PlatformApi* platform, StrView path,
                  VMemArena* scratch, u64* out_seed, u64* out_tick);
