#pragma once
// ---------------------------------------------------------------------------------------------
// encoder.h - the name-keyed save encoder/decoder over reflection field tables.
//
// Spec: docs/ASSETS-AND-DATA.md §5 (design), §8.4 (the byte layout - this file implements the
//   "reflected payload" and "ECS column payload" shapes); docs/ECS.md §6 consumer 2 (M2 durable
//   saves), §10.1 (this file). The save FILE (header, name table, arena blocks, crc, alias
//   registration, versioned migration functions) is save.h's - the W3 assets+data lane; this is
//   the per-payload engine it calls.
// Purpose: a save must survive a rebuild and a schema edit (unlike the raw memcpy snapshot).
//   Rows encode name-keyed: a stored field list (name_hash, kind, count, size per field), then
//   rows packed per that list, little-endian. The decoder matches stored fields to live fields
//   by name hash, applies ALIASES for renames and the live component's default_row for added
//   fields, skips stored fields the live schema dropped, and REJECTS a field whose kind or
//   element count changed (that is a versioned migration function in save.h, never generic
//   coercion - docs/ASSETS-AND-DATA.md §5).
// Invariants: encode writes fields in table order (explicit _padN fields included - they are
//   ordinary rows of zeros), so a row's payload is exactly info->size bytes and two encodes of
//   one state are byte-identical. Decode writes only matched live fields over a default-
//   initialized row - a decoded row NEVER carries stale caller memory. Fail-loud: every reject
//   is a named ErrCode (0x032x); truncation is the byte pair's sticky code.
// Determinism: pure functions of the input bytes and tables; no allocation (the stored-field
//   map lives on the caller's frame, bounded by ENC_MAX_FIELDS).
// Threading: none.
// Includes: core/reflect.h, core/column.h (the column payload's source).
// ---------------------------------------------------------------------------------------------
#include "core/reflect.h"
#include "core/column.h"
#include "foundation/array.h"   // Span

constexpr ErrCode ERR_ENC_MALFORMED  = (ErrCode)0x0320;  // bad kind byte, size/kind mismatch, field count over ENC_MAX_FIELDS
constexpr ErrCode ERR_ENC_FIELD_KIND = (ErrCode)0x0321;  // a stored field's kind or count changed vs the live schema
constexpr ErrCode ERR_ENC_OVERFLOW   = (ErrCode)0x0322;  // stored row/entity count exceeds the caller's buffer

// Log-side name for an encoder ErrCode; "ERR_?" outside this header's codes.
constexpr const char* err_enc_name(ErrCode e) {
    return e == ERR_OK ? "ERR_OK"
         : e == ERR_BYTES_TRUNCATED ? "ERR_BYTES_TRUNCATED"
         : e == ERR_ENC_MALFORMED ? "ERR_ENC_MALFORMED"
         : e == ERR_ENC_FIELD_KIND ? "ERR_ENC_FIELD_KIND"
         : e == ERR_ENC_OVERFLOW ? "ERR_ENC_OVERFLOW" : "ERR_?";
}

// A rename entry (docs/ASSETS-AND-DATA.md §8.4): a stored field named old_hash decodes into the
// live field named new_hash. save.h owns registration; the decoder takes the span. Resolution
// is per stored field, first matching alias wins, no chaining; when two stored fields resolve
// to one live field (a duplicate stored name, or an alias landing on an also-stored name) the
// decode is deterministic last-write-wins in stored order - stored streams are the encoder's
// own output, where neither shape exists (review 1 note, pinned here rather than rejected).
struct FieldAlias {
    NameHash old_hash;
    NameHash new_hash;
};

// The decoder's stored-field bound - far above any real component (docs/ECS.md §6 components
// are tens of fields); past it the payload is treated as malformed, never truncated silently.
enum : u32 { ENC_MAX_FIELDS = 256 };

// Writes the reflected payload (docs/ASSETS-AND-DATA.md §8.4): u32 field_count; per field
// { name_hash u64, kind u8, 0 u8, count u16, size u32 }; u32 row_count; rows packed per the
// field list, little-endian. rows = row_count contiguous instances of info's struct.
void encoder_write_rows(ByteWriter* w, const ComponentInfo* info, const void* rows, u32 row_count);

// Writes the ECS column payload: the reflected payload over the column's dense rows, then the
// entity list (u32 handle bits per row) - docs/ASSETS-AND-DATA.md §8.4.
void encoder_write_column(ByteWriter* w, const ComponentTable* t);

// Decodes a reflected payload against the LIVE schema `info`: each out row starts as
// info->default_row (or zeros), matched fields overlay it, dropped stored fields are skipped,
// kind/count changes reject with ERR_ENC_FIELD_KIND. Returns the decoded row count (<=
// max_rows, else ERR_ENC_OVERFLOW with nothing written). out_rows holds max_rows instances.
Result<u32> encoder_read_rows(ByteReader* r, const ComponentInfo* info,
                              Span<const FieldAlias> aliases, void* out_rows, u32 max_rows);

// Decodes an ECS column payload: rows as encoder_read_rows, then the parallel entity list into
// out_entities (same count). The caller (save.h's world rebuild) re-adds rows through the
// command/column door; this function only decodes.
Result<u32> encoder_read_column(ByteReader* r, const ComponentInfo* info,
                                Span<const FieldAlias> aliases, void* out_rows,
                                Entity* out_entities, u32 max_rows);
