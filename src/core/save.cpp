// ---------------------------------------------------------------------------------------------
// save.cpp - the save file writer/reader: header + name table + per-arena block framing over
//   core/encoder.h's per-payload engine.
//
// See save.h for the contract. SAVE_ENC_REFLECTED/SAVE_ENC_ECS_COLUMN are implemented;
// SAVE_ENC_RAW_POOL/SAVE_ENC_CHUNK_STORE TL_FATAL (no Alloy pool/chunk store exists yet); the
// NameTable is written empty (no StrId-bearing component exists yet - save.h's top-of-file note).
// ---------------------------------------------------------------------------------------------
#include "core/save.h"

namespace {

// Patches 4 already-written bytes at `at` with `v`, low-byte-first - the same encoding
// bw_put_u32 uses, for the one field (a block's byte_len) whose value is only known after its
// payload has been written past it.
void patch_u32(u8* at, u32 v) {
    at[0] = (u8)(v & 0xFFu);
    at[1] = (u8)((v >> 8) & 0xFFu);
    at[2] = (u8)((v >> 16) & 0xFFu);
    at[3] = (u8)((v >> 24) & 0xFFu);
}

u32 read_u32_le(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

// True iff any field of `info` (recursively - no nested-struct fields exist in this schema
// system, so one level suffices) is a StrId - save.h's documented scope cut.
bool has_strid_field(const ComponentInfo* info) {
    for (u32 i = 0; i < info->field_count; ++i) {
        if (info->fields[i].kind == K_StrId) { return true; }
    }
    return false;
}

const SaveArenaDesc* find_arena_desc(Span<const SaveArenaDesc> descs, NameHash id) {
    for (u32 i = 0; i < descs.count; ++i) {
        if (descs.data[i].arena_id == id) { return &descs.data[i]; }
    }
    return nullptr;
}

const ArenaEntry* find_registry_entry(const ArenaRegistry* r, NameHash id) {
    for (u32 i = 0; i < r->count; ++i) {
        if (r->e[i].id == id) { return &r->e[i]; }
    }
    return nullptr;
}

const SaveComponentMigration* find_migration(Span<const SaveComponentMigration> migs,
                                             NameHash comp_name_hash, u32 from_version) {
    for (u32 i = 0; i < migs.count; ++i) {
        if (migs.data[i].component_name_hash == comp_name_hash && migs.data[i].from_version == from_version) {
            return &migs.data[i];
        }
    }
    return nullptr;
}

Span<const FieldAlias> find_aliases(Span<const SaveComponentAliases> table, NameHash comp_name_hash) {
    for (u32 i = 0; i < table.count; ++i) {
        if (table.data[i].component_name_hash == comp_name_hash) { return table.data[i].aliases; }
    }
    return Span<const FieldAlias>{ nullptr, 0u };
}

}  // namespace

ErrCode save_write(const SaveDesc* desc, const PlatformApi* platform, StrView path, VMemArena* scratch) {
    u64 mark = arena_mark(scratch);
    u64 cap = scratch->reserved - scratch->used;
    u8* buf = (u8*)arena_push(scratch, cap, 16u);

    if (desc->arena_descs.count > MAX_ARENAS) { arena_reset_to(scratch, mark); return ERR_SAVE_TOO_MANY_ARENAS; }

    ByteWriter w;
    bw_init(&w, buf, cap);
    w.len = 160u;   // the header is patched in at the end; reserve its bytes now.

    // One block per LOGICAL entry in the caller's own list, not per underlying registered arena:
    // an ECS column is three registry entries (dense/entity/pages, docs/ECS.md §10.3) but exactly
    // one encoder_write_column call, so a registry-driven loop would triple-encode it. The
    // caller's `arena_descs` is the save's actual membership list (not "everything SNAPSHOT-
    // flagged" - pages arenas are derived, not saved, docs/ECS.md §10.3).
    u32 arena_count = 0u;
    for (u32 i = 0; i < desc->arena_descs.count; ++i) {
        const SaveArenaDesc* ad = &desc->arena_descs.data[i];

        bw_put_u64(&w, ad->arena_id);
        bw_put_u8(&w, (u8)ad->kind);
        bw_put_u8(&w, 0u); bw_put_u8(&w, 0u); bw_put_u8(&w, 0u);
        u64 len_patch_at = w.len;
        bw_put_u32(&w, 0u);   // byte_len placeholder
        u64 payload_start = w.len;

        switch (ad->kind) {
            case SAVE_ENC_REFLECTED: {
                TL_CHECK(!has_strid_field(ad->info));   // save.h's documented scope cut
                const ArenaEntry* e = find_registry_entry(desc->registry, ad->arena_id);
                TL_CHECK(e != nullptr);
                encoder_write_rows(&w, ad->info, e->arena->base, 1u);
                break;
            }
            case SAVE_ENC_ECS_COLUMN: {
                TL_CHECK(!has_strid_field(ad->info));
                encoder_write_column(&w, &desc->world->comps[ad->comp]);
                break;
            }
            case SAVE_ENC_RAW_POOL:
            case SAVE_ENC_CHUNK_STORE:
            default:
                TL_FATAL("save_write: encoder kind not yet built - no Alloy pool/chunk-store consumer exists yet");
        }
        patch_u32(buf + len_patch_at, (u32)(w.len - payload_start));
        arena_count += 1u;
    }

    SaveHeader hdr{};
    hdr.magic[0] = SAVE_MAGIC[0]; hdr.magic[1] = SAVE_MAGIC[1];
    hdr.magic[2] = SAVE_MAGIC[2]; hdr.magic[3] = SAVE_MAGIC[3];
    // Self-found while adding round 1 review D9's migration coverage test, not one of the
    // reviewer's own items: `SaveHeader{}` zero-inits format_version, and nothing here ever set
    // it to SAVE_FORMAT_VERSION, so every save this lane has written so far is stamped version 0
    // regardless of the real format - save_read's `format_version > SAVE_FORMAT_VERSION` check
    // still passed (0 is never newer), so no existing test caught it, but SaveComponentMigration
    // dispatch is keyed by this exact field (save.h: "by format_version") and would never see the
    // real number. Root cause, same file, blocks the coverage this fix is for.
    hdr.format_version = SAVE_FORMAT_VERSION;
    memcpy(hdr.build_id, desc->build_id, 32u);
    memcpy(hdr.session_fingerprint, desc->session_fingerprint, 32u);
    hdr.seed = desc->seed;
    hdr.tick = desc->tick;
    hdr.session_model = 0u;
    hdr.origin = SAVE_ORIGIN_LOCAL;
    hdr.name_table_len = 0u;   // save.h's documented NameTable scope cut
    hdr.arena_count = arena_count;
    hdr.flags = 0u;
    for (u32 i = 0; i < 52u; ++i) { hdr._pad0[i] = 0u; }
    ByteWriter hw;
    bw_init(&hw, buf, 160u);
    wire_write_SaveHeader(&hw, &hdr);
    TL_CHECK(hw.len == 160u);

    // Round 1 review D6: the whole file, header included - a save_write/save_read agreement that
    // used to start at byte 160, leaving seed/tick/format_version/origin/name_table_len/
    // arena_count outside the integrity check entirely (docs/ASSETS-AND-DATA.md §8.4).
    u32 crc = crc32(buf, w.len);
    bw_put_u32(&w, crc);

    ErrCode e = platform->file.write_atomic(platform->file.ctx, path, Span<const u8>{ buf, (u32)w.len });
    arena_reset_to(scratch, mark);
    if (e != ERR_OK) { return ERR_SAVE_IO; }
    return ERR_OK;
}

ErrCode save_read(const SaveDesc* desc, const PlatformApi* platform, StrView path,
                  VMemArena* scratch, u64* out_seed, u64* out_tick) {
    u64 mark = arena_mark(scratch);
    Result<Span<u8>> file = platform->file.read_all(platform->file.ctx, path, scratch);
    if (file.err != ERR_OK) { arena_reset_to(scratch, mark); return ERR_SAVE_IO; }
    if (file.value.count < 164u) { arena_reset_to(scratch, mark); return ERR_SAVE_TRUNCATED; }   // header + crc32, no blocks

    const u8* buf = file.value.data;
    u32 total = file.value.count;

    if (buf[4] != SAVE_MAGIC[0] || buf[5] != SAVE_MAGIC[1] || buf[6] != SAVE_MAGIC[2] || buf[7] != SAVE_MAGIC[3]) {
        arena_reset_to(scratch, mark);
        return ERR_SAVE_BAD_MAGIC;
    }
    u32 stored_crc = read_u32_le(buf + total - 4u);
    // Round 1 review D6: the whole file, header included (docs/ASSETS-AND-DATA.md §8.4) - was
    // buf+160u, total-160u-4u, which left the header unprotected (measured: a single flipped
    // `tick` byte loaded as ERR_OK with the wrong tick).
    u32 computed_crc = crc32(buf, total - 4u);
    if (stored_crc != computed_crc) { arena_reset_to(scratch, mark); return ERR_SAVE_CRC_MISMATCH; }

    ByteReader hr;
    br_init(&hr, buf, 160u);
    SaveHeader hdr{};
    ErrCode hdr_err = wire_read_SaveHeader(&hr, &hdr);
    if (hdr_err != ERR_OK) { arena_reset_to(scratch, mark); return ERR_SAVE_TRUNCATED; }
    if (hdr.format_version > SAVE_FORMAT_VERSION) { arena_reset_to(scratch, mark); return ERR_SAVE_VERSION; }

    ByteReader r;
    br_init(&r, buf, total - 4u);   // exclude the trailer from the block walk
    r.pos = 160u;
    // Round 1 review D8: hdr.name_table_len is a file-supplied u32 with no bound of its own (not
    // even CRC-covered before D6's fix); a hostile 0xFFFFFFFF used to spin ~4G no-op iterations
    // (br_get_u64/u16/br_skip are bounds-safe but still cost a loop iteration each once sticky)
    // before the block loop below finally reported ERR_SAVE_TRUNCATED. Breaking out the moment the
    // reader turns sticky makes the same refusal immediate instead of a free stall.
    for (u32 i = 0; i < hdr.name_table_len; ++i) {
        if (!br_ok(&r)) { break; }
        br_get_u64(&r); u16 len = br_get_u16(&r); br_skip(&r, len);   // save.h's NameTable scope cut: skipped, not resolved
    }
    if (!br_ok(&r)) { arena_reset_to(scratch, mark); return ERR_SAVE_TRUNCATED; }

    // Decode every block into a scratch buffer first; nothing touches desc->world or a live
    // arena until every block has decoded successfully (save.h: "no silent partial loads").
    struct Pending { NameHash arena_id; SaveEncoderKind kind; const SaveArenaDesc* ad; void* rows; u32 row_count; Entity* entities; };
    enum : u32 { MAX_PENDING = MAX_ARENAS };
    Pending* pend = (Pending*)arena_push(scratch, (u64)MAX_PENDING * sizeof(Pending), alignof(Pending));
    u32 pend_count = 0u;

    for (u32 i = 0; i < hdr.arena_count; ++i) {
        if (!br_ok(&r)) { arena_reset_to(scratch, mark); return ERR_SAVE_TRUNCATED; }
        NameHash arena_id = br_get_u64(&r);
        u8 kind = br_get_u8(&r);
        br_get_u8(&r); br_get_u8(&r); br_get_u8(&r);
        u32 byte_len = br_get_u32(&r);
        u64 block_start = r.pos;
        if (!br_ok(&r)) { arena_reset_to(scratch, mark); return ERR_SAVE_TRUNCATED; }

        const SaveArenaDesc* ad = find_arena_desc(desc->arena_descs, arena_id);
        if (ad == nullptr) { arena_reset_to(scratch, mark); return ERR_SAVE_ARENA_MISSING; }
        // Round 1 review D7: the file's own kind byte used to drive decode dispatch unchecked
        // against what the caller actually registered for this arena_id. A mismatch (or a byte
        // outside SaveEncoderKind's own range - previously reaching TL_FATAL from file content)
        // could decode via the wrong encoder and, on apply, target the wrong component through
        // `ad->comp` (set for ECS_COLUMN entries only). Checked once, here, before anything about
        // this block is trusted; ad->kind is always one of the four values a caller registered, so
        // the RAW_POOL/CHUNK_STORE TL_FATAL below is now reachable only by a caller registering an
        // unimplemented kind - a genuine engineering bug, the same class save_write already fatals
        // on, never file content.
        if ((SaveEncoderKind)kind != ad->kind) {
            arena_reset_to(scratch, mark);
            return ERR_SAVE_KIND_MISMATCH;
        }
        // Round 1 review D5: byte_len is file-supplied and was never checked against the bytes
        // actually remaining. ByteReader is bounds-safe only WITHIN the length it is handed, so an
        // inflated byte_len authorised block_r to read past the real payload and into whatever
        // scratch-arena memory happened to follow it (measured: a 252 B file declaring byte_len =
        // 64 KiB and row_count = 48 decoded 48 rows from ~16 real bytes with err = ERR_OK).
        if (byte_len > (total - 4u) - block_start) {
            arena_reset_to(scratch, mark);
            return ERR_SAVE_TRUNCATED;
        }

        ByteReader block_r;
        br_init(&block_r, buf + block_start, byte_len);

        Span<const FieldAlias> aliases = find_aliases(desc->aliases, ad->info->name_hash);
        const SaveComponentMigration* mig = find_migration(desc->migrations, ad->info->name_hash, hdr.format_version);

        void* out_rows = arena_push(scratch, (u64)ad->max_rows * ad->info->size, ad->info->align);
        Entity* out_entities = nullptr;
        u32 row_count = 0u;

        if (mig != nullptr) {
            Result<u32> mr = mig->fn(&block_r, ad->info, out_rows, ad->max_rows);
            if (mr.err != ERR_OK) { arena_reset_to(scratch, mark); return mr.err; }
            row_count = mr.value;
        } else if ((SaveEncoderKind)kind == SAVE_ENC_REFLECTED) {
            Result<u32> dr = encoder_read_rows(&block_r, ad->info, aliases, out_rows, ad->max_rows);
            if (dr.err != ERR_OK) {
                arena_reset_to(scratch, mark);
                return dr.err == ERR_ENC_FIELD_KIND ? ERR_SAVE_FIELD_KIND : dr.err;
            }
            row_count = dr.value;
        } else if ((SaveEncoderKind)kind == SAVE_ENC_ECS_COLUMN) {
            out_entities = (Entity*)arena_push(scratch, (u64)ad->max_rows * sizeof(Entity), alignof(Entity));
            Result<u32> dr = encoder_read_column(&block_r, ad->info, aliases, out_rows, out_entities, ad->max_rows);
            if (dr.err != ERR_OK) {
                arena_reset_to(scratch, mark);
                return dr.err == ERR_ENC_FIELD_KIND ? ERR_SAVE_FIELD_KIND : dr.err;
            }
            row_count = dr.value;
        } else {
            arena_reset_to(scratch, mark);
            TL_FATAL("save_read: encoder kind not yet built - no Alloy pool/chunk-store consumer exists yet");
        }

        pend[pend_count] = Pending{ arena_id, (SaveEncoderKind)kind, ad, out_rows, row_count, out_entities };
        pend_count += 1u;
        r.pos = block_start + byte_len;
    }

    // Every block decoded - apply.
    for (u32 i = 0; i < pend_count; ++i) {
        const Pending* p = &pend[i];
        if (p->kind == SAVE_ENC_REFLECTED) {
            const ArenaEntry* e = nullptr;
            for (u32 j = 0; j < desc->registry->count; ++j) {
                if (desc->registry->e[j].id == p->arena_id) { e = &desc->registry->e[j]; break; }
            }
            TL_CHECK(e != nullptr);
            memcpy(e->arena->base, p->rows, p->ad->info->size);
        } else {   // SAVE_ENC_ECS_COLUMN
            const u8* rows_bytes = (const u8*)p->rows;
            for (u32 k = 0; k < p->row_count; ++k) {
                world_add_raw(desc->world, p->entities[k], p->ad->comp, rows_bytes + (u64)k * p->ad->info->size);
            }
        }
    }

    *out_seed = hdr.seed;
    *out_tick = hdr.tick;
    arena_reset_to(scratch, mark);
    return ERR_OK;
}
