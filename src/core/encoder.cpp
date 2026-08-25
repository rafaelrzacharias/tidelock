// encoder.cpp - the name-keyed payload engine. Spec: docs/ASSETS-AND-DATA.md §8.4; contracts
// in encoder.h.
#include "core/encoder.h"
#include <string.h>

namespace {

// One stored field's meta + its resolved live target (ENC_NO_MATCH = skip its bytes).
struct StoredField {
    NameHash name_hash;
    FieldKind kind;
    u8  _pad0;
    u16 count;
    u32 size;
    u32 live_index;
};
enum : u32 { ENC_NO_MATCH = 0xFFFFFFFFu };

// Resolves a stored name against the live fields, directly then through the alias table
// (stored old_hash -> live new_hash). ENC_NO_MATCH when the live schema dropped the field.
u32 enc_resolve(const ComponentInfo* info, Span<const FieldAlias> aliases, NameHash stored) {
    NameHash wanted = stored;
    for (u32 a = 0; a < aliases.count; ++a) {
        if (aliases.data[a].old_hash == stored) { wanted = aliases.data[a].new_hash; break; }
    }
    for (u32 i = 0; i < info->field_count; ++i) {
        if (info->fields[i].name_hash == wanted) { return i; }
    }
    return ENC_NO_MATCH;
}

}  // namespace

void encoder_write_rows(ByteWriter* w, const ComponentInfo* info, const void* rows, u32 row_count) {
    TL_CHECK(info != nullptr && (rows != nullptr || row_count == 0u));
    bw_put_u32(w, info->field_count);
    for (u32 i = 0; i < info->field_count; ++i) {
        const FieldInfo* f = &info->fields[i];
        bw_put_u64(w, f->name_hash);
        bw_put_u8(w, (u8)f->kind);
        bw_put_u8(w, 0u);
        bw_put_u16(w, f->count);
        bw_put_u32(w, f->size);
    }
    bw_put_u32(w, row_count);
    const u8* base = (const u8*)rows;
    for (u32 r = 0; r < row_count; ++r) {
        tl_wire_put_row(w, info->fields, info->field_count, base + (u64)r * info->size);
    }
}

void encoder_write_column(ByteWriter* w, const ComponentTable* t) {
    TL_CHECK(t != nullptr && t->info != nullptr);
    encoder_write_rows(w, t->info, t->dense, t->count);
    for (u32 i = 0; i < t->count; ++i) { bw_put_u32(w, t->entities[i].bits); }
}

Result<u32> encoder_read_rows(ByteReader* r, const ComponentInfo* info,
                              Span<const FieldAlias> aliases, void* out_rows, u32 max_rows) {
    Result<u32> res;
    res.value = 0;
    res.err = ERR_OK;
    TL_CHECK(info != nullptr && (out_rows != nullptr || max_rows == 0u));

    const u32 stored_count = br_get_u32(r);
    if (!br_ok(r)) { res.err = r->err; return res; }
    if (stored_count == 0u || stored_count > ENC_MAX_FIELDS) { res.err = ERR_ENC_MALFORMED; return res; }

    StoredField stored[ENC_MAX_FIELDS];
    u64 stored_row_bytes = 0;
    for (u32 i = 0; i < stored_count; ++i) {
        stored[i].name_hash = br_get_u64(r);
        const u8 kind_byte = br_get_u8(r);
        (void)br_get_u8(r);
        stored[i].count = br_get_u16(r);
        stored[i].size = br_get_u32(r);
        if (!br_ok(r)) { res.err = r->err; return res; }
        if (kind_byte >= (u8)K_COUNT || stored[i].count == 0u) { res.err = ERR_ENC_MALFORMED; return res; }
        stored[i].kind = (FieldKind)kind_byte;
        stored[i]._pad0 = 0;
        if (stored[i].size != kind_scalar_size(stored[i].kind) * stored[i].count) {
            res.err = ERR_ENC_MALFORMED;
            return res;
        }
        stored_row_bytes += stored[i].size;
        stored[i].live_index = enc_resolve(info, aliases, stored[i].name_hash);
        if (stored[i].live_index != ENC_NO_MATCH) {
            const FieldInfo* live = &info->fields[stored[i].live_index];
            // A kind or element-count change is an explicit versioned migration in save.h,
            // never a coercion here (docs/ASSETS-AND-DATA.md §5).
            if (live->kind != stored[i].kind || live->count != stored[i].count) {
                res.err = ERR_ENC_FIELD_KIND;
                return res;
            }
        }
    }
    (void)stored_row_bytes;

    const u32 row_count = br_get_u32(r);
    if (!br_ok(r)) { res.err = r->err; return res; }
    if (row_count > max_rows) { res.err = ERR_ENC_OVERFLOW; return res; }

    u8* out = (u8*)out_rows;
    for (u32 row = 0; row < row_count; ++row) {
        u8* dst = out + (u64)row * info->size;
        // Missing stored fields land on the default: the live default_row, else zeros
        // (docs/ASSETS-AND-DATA.md §5 "declared defaults for added fields").
        if (info->default_row != nullptr) {
            memcpy(dst, info->default_row, info->size);
        } else {
            memset(dst, 0, info->size);
        }
        for (u32 i = 0; i < stored_count; ++i) {
            if (stored[i].live_index == ENC_NO_MATCH) {
                br_skip(r, stored[i].size);   // the live schema dropped this field
                continue;
            }
            const FieldInfo* live = &info->fields[stored[i].live_index];
            const u32 scalar = kind_scalar_size(stored[i].kind);
            for (u32 e = 0; e < stored[i].count; ++e) {
                tl_wire_get_scalar(r, stored[i].kind, dst + live->offset + (u64)e * scalar);
            }
        }
        if (!br_ok(r)) { res.err = r->err; return res; }
    }
    res.value = row_count;
    return res;
}

Result<u32> encoder_read_column(ByteReader* r, const ComponentInfo* info,
                                Span<const FieldAlias> aliases, void* out_rows,
                                Entity* out_entities, u32 max_rows) {
    Result<u32> res = encoder_read_rows(r, info, aliases, out_rows, max_rows);
    if (res.err != ERR_OK) { return res; }
    TL_CHECK(out_entities != nullptr || res.value == 0u);
    for (u32 i = 0; i < res.value; ++i) { out_entities[i].bits = br_get_u32(r); }
    if (!br_ok(r)) {
        Result<u32> bad;
        bad.value = 0;
        bad.err = r->err;
        return bad;
    }
    return res;
}
