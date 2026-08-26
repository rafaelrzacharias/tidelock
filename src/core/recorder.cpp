// recorder.cpp - RecordedInput read/write and the per-tick recorder (docs/DETERMINISM.md §9.2,
//   docs/INPUT.md §9.5). See recorder.h for the contract.
#include "core/recorder.h"
#include "foundation/crc32.h"
#include "foundation/arena_registry.h"
#include <string.h>

namespace {

// InputFrame is hand-written LE (docs/INPUT.md §1: not a TL_WIRE_STRUCT - see core/input.h's
// contract block), so the recorder writes/reads it field-by-field itself, exactly the byte order
// docs/NETCODE.md §20.2.2's WireFrame mirror uses for the same 76 B geometry.
void put_input_frame(ByteWriter* w, const InputFrame* f) {
    for (u32 a = 0; a < MAX_ACTIONS; ++a) {
        bw_put_u8(w, (u8)f->actions[a].value);
        bw_put_u8(w, f->actions[a].flags);
    }
    bw_put_u32(w, (u32)f->pointer_x);
    bw_put_u32(w, (u32)f->pointer_y);
    bw_put_u32(w, f->tick);
}

void get_input_frame(ByteReader* r, InputFrame* f) {
    for (u32 a = 0; a < MAX_ACTIONS; ++a) {
        f->actions[a].value = (i8)br_get_u8(r);
        f->actions[a].flags = br_get_u8(r);
    }
    f->pointer_x = (i32)br_get_u32(r);
    f->pointer_y = (i32)br_get_u32(r);
    f->tick = br_get_u32(r);
}

}  // namespace

void recorder_init(Recorder* rec, VMemArena* arena, u32 max_rows, u64 base_tick, u64 seed,
                   u8 peer_count, u8 live_mask, const u8 build_id[32], const u8 session_fingerprint[32]) {
    TL_CHECK(peer_count <= MAX_PEERS);
    memset(rec, 0, sizeof(Recorder));
    array_init_fixed(&rec->rows, arena, max_rows);
    rec->base_tick = base_tick;
    rec->seed = seed;
    rec->peer_count = peer_count;
    rec->live_mask = live_mask;
    memcpy(rec->build_id, build_id, 32u);
    memcpy(rec->session_fingerprint, session_fingerprint, 32u);
}

void recorder_tick(Recorder* rec, World* w, const InputFrame* frames) {
    u64 per_arena[MAX_ARENAS];
    RecordedInputRow row{};
    row.world_hash = registry_hash_all(w->registry, per_arena);
    // Only [0, peer_count) is ever written back out (recorder_write/recorder_read_body both key
    // their loops on peer_count, per this header's format) - slots beyond it are zeroed here, not
    // memcpy'd from the caller's buffer, so the in-memory row already matches what a write/read
    // round trip would reconstruct (RecordedInputRow row{} there is zero-init too), instead of
    // carrying whatever a producer left in Engine::frames' non-live slots from an earlier tick
    // (review round 1 finding 10 - no test compared a full row byte-for-byte across that seam).
    memcpy(row.frames, frames, (u64)rec->peer_count * sizeof(InputFrame));
    array_push(&rec->rows, row);
}

u64 recorder_bytes_needed(const Recorder* rec) {
    const u64 frame_bytes = 76ull * (u64)rec->peer_count;
    const u64 row_bytes = frame_bytes + 8ull;   // + world_hash
    return 128ull + (u64)rec->rows.count * row_bytes + 4ull;   // header + body + crc32 trailer
}

u64 recorder_write(const Recorder* rec, ByteWriter* w) {
    RecordedInputHeader h{};
    h.format_version = RECORDED_INPUT_FORMAT_VERSION;
    memcpy(h.magic, RECORDED_INPUT_MAGIC, 4u);
    memcpy(h.build_id, rec->build_id, 32u);
    memcpy(h.session_fingerprint, rec->session_fingerprint, 32u);
    h.seed = rec->seed;
    h.base_tick = rec->base_tick;
    h.peer_count = rec->peer_count;
    h.live_mask = rec->live_mask;
    h.flags = 0u;   // HAS_ARENA_HASHES not implemented - see recorder.h's contract block
    h.frame_count = rec->rows.count;
    wire_write_RecordedInputHeader(w, &h);

    const u64 body_start = w->len;
    for (u32 i = 0; i < rec->rows.count; ++i) {
        const RecordedInputRow& row = rec->rows.data[i];
        for (u32 p = 0; p < rec->peer_count; ++p) { put_input_frame(w, &row.frames[p]); }
        bw_put_u64(w, row.world_hash);
    }
    const u64 body_len = w->len - body_start;
    const u32 crc = crc32(w->base + body_start, body_len);
    bw_put_u32(w, crc);
    return w->len;
}

ErrCode recorder_read_header(ByteReader* r, RecordedInputHeader* out, const u8 expected_session_fingerprint[32]) {
    const ErrCode e = wire_read_RecordedInputHeader(r, out);
    if (e != ERR_OK) { return e; }
    if (memcmp(out->magic, RECORDED_INPUT_MAGIC, 4u) != 0) { return ERR_RECORDER_BAD_MAGIC; }
    if (out->format_version != RECORDED_INPUT_FORMAT_VERSION) { return ERR_RECORDER_VERSION; }
    if (expected_session_fingerprint != nullptr
        && memcmp(out->session_fingerprint, expected_session_fingerprint, 32u) != 0) {
        return ERR_RECORDER_FINGERPRINT;
    }
    // Both counts below are about to size (or be used to size) the caller's out_rows buffer and
    // drive recorder_read_body's write loop - bound them against reality before handing them
    // back, rather than trusting a corrupt/truncated/malicious file (this header's contract block).
    if (out->peer_count > MAX_PEERS) { return ERR_RECORDER_PEER_COUNT; }
    const u64 row_bytes = 76ull * (u64)out->peer_count + 8ull;   // InputFrame's pinned 76 B + world_hash
    const u64 remaining = r->len > r->pos ? r->len - r->pos : 0u;
    if (remaining < 4ull || out->frame_count > (remaining - 4ull) / row_bytes) {   // -4: the crc32 trailer
        return ERR_BYTES_TRUNCATED;
    }
    return ERR_OK;
}

ErrCode recorder_read_body(ByteReader* r, const RecordedInputHeader* header, RecordedInputRow* out_rows) {
    const u64 body_start = r->pos;
    for (u64 i = 0; i < header->frame_count; ++i) {
        RecordedInputRow row{};
        for (u32 p = 0; p < header->peer_count; ++p) { get_input_frame(r, &row.frames[p]); }
        row.world_hash = br_get_u64(r);
        if (!br_ok(r)) { return r->err; }   // checked every row, not once after the whole loop
        out_rows[i] = row;
    }
    const u64 body_len = r->pos - body_start;
    const u32 computed_crc = crc32(r->base + body_start, body_len);
    const u32 stored_crc = br_get_u32(r);
    if (!br_ok(r)) { return r->err; }
    if (computed_crc != stored_crc) { return ERR_RECORDER_CRC; }
    return ERR_OK;
}
