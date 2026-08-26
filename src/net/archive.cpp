// ---------------------------------------------------------------------------------------------
// archive.cpp - the columnar + transition segment encoder/decoder.
//
// Spec: docs/NETCODE.md §20.2.9 (segment byte layout, channel numbering, the per-channel record
//   rules), §13.3 (the three encoding steps: columnar, transition, pointer second-order delta),
//   §13.4 (the size target), §20.6 T2 (what the tests prove). Declarations: net/wire.h.
// Determinism: pure integer functions over caller memory; no io, no alloc, no floats, no clock.
//   A segment's bytes are a function of its inputs alone - load-bearing, not incidental:
//   docs/NETCODE.md §20.2.8 hashes these bytes into ChainEntry.log_segment_hash, so two peers
//   holding the same confirmed log must produce the same segment or the chain forks with no
//   divergence behind it. Every stream is canonical for that reason (net/wire.h's note).
//
// THREE channel families, three record models (docs/NETCODE.md §20.2.9) - they are not one shape
// with different numbers, and the decoder must apply each one's rules:
//   - actions, channels 0..MAX_ACTIONS-1: a HELD VALUE. uvarint(u16(u8(value)) << 1 | (flags&1)),
//     one record wherever it changes. A button held 120 ticks is one record (§13.3 step 2).
//   - pointer_x/pointer_y, channels 32/33: INTEGRATED. Record 0 is mandatory at delta_tick 0 and
//     carries the ABSOLUTE position at base_tick; every later record carries a new VELOCITY, held
//     constant between records, p += v each tick (§13.3 step 3). svarint throughout.
//   - flag escape, channel 34: an EVENT stream. uvarint(action << 3 | flags) for a frame whose
//     stored pressed/released differ from the edges derived from the down bits. Several actions
//     can escape on one tick, so a delta_tick of 0 is legal after the first record here and
//     nowhere else.
// ---------------------------------------------------------------------------------------------
#include "net/wire.h"
#include "foundation/crc32.h"

// The offsets the two crc32 fields sit at, and the span each covers (docs/NETCODE.md §20.2.9).
// The header is written with both zeroed, the payload is appended, and the two are then patched
// in place - a crc cannot be computed over bytes that have not been written yet.
enum : u32 {
    AR_HDR_BYTES        = (u32)sizeof(ArchiveSegmentHeader),   // 112
    AR_PAYLOAD_CRC_OFF  = 100u,
    AR_HEADER_CRC_OFF   = 108u,
    AR_HEADER_CRC_SPAN  = 108u,                                 // crc32 over bytes [0,108)
};

// The action channel's packed word: value in the high bits, the down bit in bit 0.
static u32 ar_action_word(i8 value, u8 flags) {
    return ((u32)(u8)value << 1) | (u32)(flags & 1u);
}

// Unpacks ar_action_word back into the value and the down bit.
static void ar_action_unword(u32 word, i8* value, u8* down) {
    *value = (i8)(u8)((word >> 1) & 0xFFu);
    *down = (u8)(word & 1u);
}

// The edge flags implied by two consecutive down bits: pressed on a rising edge, released on a
// falling one. A frame whose stored flags differ from these is what the escape channel carries.
static u8 ar_derived_flags(u8 down, u8 down_prev) {
    u8 f = (u8)(down ? 1u : 0u);
    if (down != 0u && down_prev == 0u) { f |= 2u; }
    if (down == 0u && down_prev != 0u) { f |= 4u; }
    return f;
}

// Writes an ArchiveStreamHeader field by field, low byte first (never a memcpy of the struct -
// docs/CPP-SUBSET.md §9 R-2).
static void ar_put_stream_header(ByteWriter* w, u32 record_count, u8 channel, u8 slot) {
    bw_put_u32(w, record_count);
    bw_put_u8(w, channel);
    bw_put_u8(w, slot);
    bw_put_u16(w, 0u);
}

// Reads one back; refuses a nonzero pad and an out-of-range channel or slot.
static ErrCode ar_get_stream_header(ByteReader* r, ArchiveStreamHeader* h) {
    h->record_count = br_get_u32(r);
    h->channel      = br_get_u8(r);
    h->slot         = br_get_u8(r);
    h->_pad0        = br_get_u16(r);
    if (!br_ok(r)) { return ERR_BYTES_TRUNCATED; }
    if (h->_pad0 != 0u) { return ERR_WIRE_PAD_NONZERO; }
    // docs/NETCODE.md §20.2.9 defines 35 channels: 0..31 action, 32 pointer_x, 33 pointer_y,
    // 34 flag escape. Its layout line says "for ch in 0..35" and ARCHIVE_CH_COUNT follows it, so
    // a bound of >= ARCHIVE_CH_COUNT would leave 35 a VALID channel byte that the dispatch's
    // else-branch would then treat as the escape channel - a second spelling of one segment.
    // Bounded by the highest real channel instead. Doc off-by-one filed in TODO.md.
    if (h->channel > ARCHIVE_CH_MAX || h->slot >= (u8)MAX_PEERS) { return ERR_NET_MALFORMED; }
    return ERR_OK;
}

// An UPPER bound: empty streams are omitted on the wire, so a real segment is far smaller.
u64 archive_segment_max_bytes(u32 slot_count, u32 tick_count, u32 log_record_count) {
    // Worst case per stream: a record at every tick, each spelled at full varint width
    // (5 bytes of delta_tick + 5 of value). The escape channel can carry MAX_ACTIONS records per
    // tick, so it is budgeted separately rather than folded into the per-channel figure.
    const u64 hdr = (u64)sizeof(ArchiveStreamHeader);
    const u64 held = hdr + (u64)tick_count * 10ull;
    const u64 escape = hdr + (u64)tick_count * (u64)NET_FRAME_MAX_ACTIONS * 10ull;
    const u64 per_slot = (u64)(NET_FRAME_MAX_ACTIONS + 2u) * held + escape;
    return (u64)AR_HDR_BYTES + (u64)slot_count * per_slot
         + (u64)log_record_count * (u64)sizeof(LogRecord);
}

// --- encode -----------------------------------------------------------------------------------

// One action channel: the held-value model.
static u32 ar_encode_action(ByteWriter* w, const WireFrame* frames, u32 tick_count, u32 action,
                            u8 slot) {
    u32 count = 0;
    u32 prev = 0u;   // the ZERO state: value 0, not down
    for (u32 i = 0; i < tick_count; ++i) {
        const u32 word = ar_action_word(frames[i].actions[action].value,
                                        frames[i].actions[action].flags);
        if (word != prev) { ++count; }
        prev = word;
    }
    if (count == 0u) { return 0u; }   // an empty stream is OMITTED - see the note in the header
    ar_put_stream_header(w, count, (u8)action, slot);

    u32 last = 0;
    bool first = true;
    prev = 0u;
    for (u32 i = 0; i < tick_count; ++i) {
        const u32 word = ar_action_word(frames[i].actions[action].value,
                                        frames[i].actions[action].flags);
        if (word != prev) {
            wire_put_uvarint(w, first ? i : (i - last));
            wire_put_uvarint(w, word);
            last = i;
            first = false;
        }
        prev = word;
    }
    return count;
}

// One pointer axis: the integrated model. Record 0 is the absolute position at base_tick and is
// mandatory; later records are velocities.
static u32 ar_encode_pointer(ByteWriter* w, const WireFrame* frames, u32 tick_count, bool is_x,
                             u8 slot) {
    // A segment covering no ticks has no absolute position to state, and the decoder refuses a
    // pointer stream when tick_count == 0. Emitting one here made a zero-tick segment encodable
    // but not decodable.
    if (tick_count == 0u) { return 0u; }
    const i32 p0 = is_x ? frames[0].pointer_x : frames[0].pointer_y;

    // Count first: one mandatory absolute, then a record wherever the velocity changes.
    u32 count = 1u;
    i32 v_cur = 0;
    for (u32 i = 1; i < tick_count; ++i) {
        const i32 p = is_x ? frames[i].pointer_x : frames[i].pointer_y;
        const i32 pp = is_x ? frames[i - 1].pointer_x : frames[i - 1].pointer_y;
        const i32 v = wire_wrap_sub_i32(p, pp);
        if (v != v_cur) { ++count; v_cur = v; }
    }
    ar_put_stream_header(w, count, is_x ? (u8)ARCHIVE_CH_POINTER_X : (u8)ARCHIVE_CH_POINTER_Y, slot);

    wire_put_uvarint(w, 0u);          // delta_tick 0, mandatory
    wire_put_svarint(w, p0);          // absolute position at base_tick

    u32 last = 0;
    v_cur = 0;
    for (u32 i = 1; i < tick_count; ++i) {
        const i32 p = is_x ? frames[i].pointer_x : frames[i].pointer_y;
        const i32 pp = is_x ? frames[i - 1].pointer_x : frames[i - 1].pointer_y;
        const i32 v = wire_wrap_sub_i32(p, pp);
        if (v != v_cur) {
            wire_put_uvarint(w, i - last);
            wire_put_svarint(w, v);
            last = i;
            v_cur = v;
        }
    }
    return count;
}

// The escape channel: one record per (tick, action) whose stored flags differ from the derived
// edges. Several may share a tick, so delta_tick 0 recurs legitimately here.
static u32 ar_encode_escape(ByteWriter* w, const WireFrame* frames, u32 tick_count, u8 slot) {
    u32 count = 0;
    for (u32 i = 0; i < tick_count; ++i) {
        for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
            const u8 down = (u8)(frames[i].actions[a].flags & 1u);
            const u8 down_prev = (i == 0u) ? 0u : (u8)(frames[i - 1].actions[a].flags & 1u);
            if (frames[i].actions[a].flags != ar_derived_flags(down, down_prev)) { ++count; }
        }
    }
    if (count == 0u) { return 0u; }   // omitted when nothing escapes
    ar_put_stream_header(w, count, (u8)ARCHIVE_CH_FLAG_ESCAPE, slot);

    u32 last = 0;
    bool first = true;
    for (u32 i = 0; i < tick_count; ++i) {
        for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
            const u8 down = (u8)(frames[i].actions[a].flags & 1u);
            const u8 down_prev = (i == 0u) ? 0u : (u8)(frames[i - 1].actions[a].flags & 1u);
            const u8 flags = frames[i].actions[a].flags;
            if (flags != ar_derived_flags(down, down_prev)) {
                wire_put_uvarint(w, first ? i : (i - last));
                wire_put_uvarint(w, (a << 3) | (u32)(flags & (u8)WIRE_FLAG_BITS));
                last = i;
                first = false;
            }
        }
    }
    return count;
}

// Every ActionState.flags a segment stores must fit docs/INPUT.md §1's three bits: the action
// channel carries the down bit and the escape channel carries three, so a higher bit is dropped
// on the way out and the segment decodes to something the caller never handed in - or, worse,
// fails to decode at all after its bytes are already hashed into the chain. TL_CHECK, not
// TL_ASSERT: this must hold on netcode and ship too, where TL_ASSERT is ((void)0).
static void ar_check_frames_representable(const WireFrame* frames, u32 tick_count) {
    for (u32 i = 0; i < tick_count; ++i) {
        for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
            TL_CHECK((frames[i].actions[a].flags & (u8)~WIRE_FLAG_BITS) == 0u);
        }
    }
}

u64 archive_encode_segment(ByteWriter* w, u64 base_tick, u32 tick_count,
                           const ArchiveInput* inputs, u32 slot_count,
                           const LogRecord* records, u32 log_record_count, u32 segment_seq,
                           const u8 build_id[32], const u8 session_fingerprint[32]) {
    TL_ASSERT(w != nullptr);
    TL_ASSERT(inputs != nullptr || slot_count == 0u);
    TL_ASSERT(records != nullptr || log_record_count == 0u);
    TL_ASSERT(build_id != nullptr && session_fingerprint != nullptr);
    TL_ASSERT(slot_count <= MAX_PEERS);

    const u64 start = w->len;

    u8 slot_mask = 0u;
    for (u32 s = 0; s < slot_count; ++s) {
        // TL_CHECK, not TL_ASSERT: off TL_DEV these vanish, and a duplicate or descending slot
        // then writes streams that violate the ascending-(slot, channel) rule. The decoder
        // refuses them, so the failure surfaces as a permanently unreadable segment AFTER its
        // bytes are hashed into the chain. `1u << slot` is also UB once the bound vanishes.
        TL_CHECK(inputs[s].slot < MAX_PEERS);
        TL_CHECK(s == 0u || inputs[s].slot > inputs[s - 1].slot);
        TL_CHECK(inputs[s].frames != nullptr || tick_count == 0u);
        ar_check_frames_representable(inputs[s].frames, tick_count);
        slot_mask = (u8)(slot_mask | (u8)(1u << inputs[s].slot));
    }
    // The caller's ordering contract for the log array (docs/NETCODE.md §20.2.9). Spelled
    // inline: TL_ASSERT compiles to ((void)0) off TL_DEV, so a local used only by the assert is
    // an unused variable under -Werror on the netcode and ship tiers.
    for (u32 i = 1; i < log_record_count; ++i) {
        TL_ASSERT((records[i - 1].effective_tick < records[i].effective_tick)
               || (records[i - 1].effective_tick == records[i].effective_tick
                   && records[i - 1].origin_slot < records[i].origin_slot)
               || (records[i - 1].effective_tick == records[i].effective_tick
                   && records[i - 1].origin_slot == records[i].origin_slot
                   && records[i - 1].seq <= records[i].seq));
    }

    // Header with both crc fields zeroed; patched below once the payload exists.
    ArchiveSegmentHeader h;
    h.format_version      = NET_FORMAT_VERSION;
    h.max_actions         = NET_FRAME_MAX_ACTIONS;
    h.base_tick           = base_tick;
    h.tick_count          = tick_count;
    h.slot_mask           = slot_mask;
    h._pad0[0] = h._pad0[1] = h._pad0[2] = 0u;
    h.record_count        = 0u;          // filled after the streams are counted
    h.log_record_count    = log_record_count;
    for (u32 i = 0; i < 32u; ++i) { h.build_id[i] = build_id[i]; }
    for (u32 i = 0; i < 32u; ++i) { h.session_fingerprint[i] = session_fingerprint[i]; }
    h.payload_bytes       = 0u;
    h.payload_crc32       = 0u;
    h.segment_seq         = segment_seq;
    h.header_crc32        = 0u;
    wire_write_ArchiveSegmentHeader(w, &h);
    TL_CHECK(w->len - start == (u64)AR_HDR_BYTES);

    u32 total_records = 0;
    for (u32 s = 0; s < slot_count; ++s) {
        const WireFrame* frames = inputs[s].frames;
        const u8 slot = (u8)inputs[s].slot;
        for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
            total_records += ar_encode_action(w, frames, tick_count, a, slot);
        }
        total_records += ar_encode_pointer(w, frames, tick_count, true, slot);
        total_records += ar_encode_pointer(w, frames, tick_count, false, slot);
        total_records += ar_encode_escape(w, frames, tick_count, slot);
    }
    for (u32 i = 0; i < log_record_count; ++i) { wire_write_LogRecord(w, &records[i]); }

    // Patch the counts and the two checksums into the header now that the payload is written.
    // Spelled as explicit little-endian byte stores into the writer's own buffer, the same order
    // bytes.h would have used, because a crc covers bytes that exist.
    u8* hdr = w->base + start;
    const u64 payload_len = (w->len - start) - (u64)AR_HDR_BYTES;
    TL_CHECK(payload_len <= 0xFFFFFFFFull);

    const u32 payload_bytes = (u32)payload_len;
    const u32 payload_crc = crc32(hdr + AR_HDR_BYTES, payload_len);
    // record_count @24, payload_bytes @96, payload_crc32 @100 (docs/NETCODE.md §20.2.9).
    for (u32 i = 0; i < 4u; ++i) { hdr[24u + i] = (u8)((total_records >> (8u * i)) & 0xFFu); }
    for (u32 i = 0; i < 4u; ++i) { hdr[96u + i] = (u8)((payload_bytes >> (8u * i)) & 0xFFu); }
    for (u32 i = 0; i < 4u; ++i) { hdr[AR_PAYLOAD_CRC_OFF + i] = (u8)((payload_crc >> (8u * i)) & 0xFFu); }

    const u32 header_crc = crc32(hdr, AR_HEADER_CRC_SPAN);
    for (u32 i = 0; i < 4u; ++i) { hdr[AR_HEADER_CRC_OFF + i] = (u8)((header_crc >> (8u * i)) & 0xFFu); }

    return w->len - start;
}

// --- decode -----------------------------------------------------------------------------------

ErrCode archive_decode_segment(ByteReader* r, ArchiveSegmentHeader* out_header,
                               WireFrame* out_frames, u32 out_frame_capacity_per_slot,
                               LogRecord* out_records, u32 out_record_capacity,
                               u32* out_record_count) {
    TL_ASSERT(r != nullptr && out_header != nullptr && out_record_count != nullptr);
    TL_ASSERT(out_frames != nullptr);
    *out_record_count = 0u;

    const u64 header_start = r->pos;
    const ErrCode he = wire_read_ArchiveSegmentHeader(r, out_header);
    if (he != ERR_OK) { return he; }
    const ErrCode ve = wire_check_version(out_header->format_version);
    if (ve != ERR_OK) { return ve; }

    // The header's own checksum first: everything below trusts its counts, so it is verified
    // before any of them is used as a length.
    if (r->pos - header_start != (u64)AR_HDR_BYTES) { return ERR_NET_MALFORMED; }
    const u8* hdr = r->base + header_start;
    if (crc32(hdr, AR_HEADER_CRC_SPAN) != out_header->header_crc32) { return ERR_NET_MALFORMED; }

    // A segment from a build with a different MAX_ACTIONS cannot be decoded into these frames.
    if (out_header->max_actions != NET_FRAME_MAX_ACTIONS) { return ERR_NET_MALFORMED; }

    const u32 tick_count = out_header->tick_count;
    if (tick_count > out_frame_capacity_per_slot) { return ERR_NET_MALFORMED; }
    if (out_header->log_record_count > out_record_capacity) { return ERR_NET_MALFORMED; }

    // Now the payload's checksum, before decoding a single record out of it.
    if (out_header->payload_bytes > r->len - r->pos) { return ERR_BYTES_TRUNCATED; }
    if (crc32(r->base + r->pos, out_header->payload_bytes) != out_header->payload_crc32) {
        return ERR_NET_MALFORMED;
    }

    // Slots outside slot_mask decode as ZERO (docs/NETCODE.md §20.2.9).
    for (u32 s = 0; s < MAX_PEERS; ++s) {
        for (u32 i = 0; i < tick_count; ++i) { out_frames[s * tick_count + i] = wire_zero_frame(); }
    }

    // Empty streams are OMITTED on the wire, so the stream region is read until it ends rather
    // than as a fixed 36 per slot; each header names its own (slot, channel). The region ends
    // where the LogRecord array begins, which payload_bytes and log_record_count locate exactly.
    const u64 log_bytes = (u64)out_header->log_record_count * (u64)sizeof(LogRecord);
    if (log_bytes > (u64)out_header->payload_bytes) { return ERR_NET_MALFORMED; }
    const u64 streams_end = header_start + (u64)AR_HDR_BYTES
                          + (u64)out_header->payload_bytes - log_bytes;

    // Canonical ordering: strictly ascending (slot, channel) across the whole segment, so one
    // segment has exactly one byte spelling. Tracked as a single key to make that literal.
    u32 last_key = 0u;
    bool have_last = false;
    u8 flags_rebuilt = 0u;   // bit s: slot s's derived edge flags have been rebuilt
    u32 seen_records = 0u;
    // docs/NETCODE.md §20.2.9 makes a pointer stream's record 0 mandatory, so the encoder always
    // writes both axes for every slot in slot_mask. Without requiring them back, a slot could be
    // named in the mask and carry no streams at all, decoding to ZERO - the same frames a
    // segment that omitted the slot produces, i.e. two spellings of one segment.
    u8 ptr_x_seen = 0u;
    u8 ptr_y_seen = 0u;

    while (r->pos < streams_end) {
        ArchiveStreamHeader sh;
        const ErrCode e = ar_get_stream_header(r, &sh);
        if (e != ERR_OK) { return e; }
        if (((out_header->slot_mask >> sh.slot) & 1u) == 0u) { return ERR_NET_MALFORMED; }
        const u32 key = ((u32)sh.slot << 8) | (u32)sh.channel;
        if (have_last && key <= last_key) { return ERR_NET_MALFORMED; }
        last_key = key;
        have_last = true;
        if (sh.record_count == 0u) { return ERR_NET_MALFORMED; }   // omitted, never empty-encoded
        seen_records += sh.record_count;

        WireFrame* frames = out_frames + (u64)sh.slot * tick_count;

        if (sh.channel < (u8)NET_FRAME_MAX_ACTIONS) {
            const u32 a = sh.channel;
            if (sh.record_count > tick_count) { return ERR_NET_MALFORMED; }
            u32 cursor = 0;
            u32 run_start = 0;
            bool first = true;
            u32 prev_word = 0u;
            for (u32 rec = 0; rec < sh.record_count; ++rec) {
                u32 delta = 0, word = 0;
                ErrCode de = wire_get_uvarint(r, &delta);
                if (de != ERR_OK) { return de; }
                de = wire_get_uvarint(r, &word);
                if (de != ERR_OK) { return de; }
                if (!first && delta == 0u) { return ERR_NET_MALFORMED; }
                const u32 tick = first ? delta : (cursor + delta);
                if (tick < cursor || tick >= tick_count) { return ERR_NET_MALFORMED; }
                if (word == prev_word) { return ERR_NET_MALFORMED; }
                if (word > 0x1FFu) { return ERR_NET_MALFORMED; }
                // Close the PREVIOUS record's run at this record's tick rather than re-filling
                // the whole tail each time: the tail form is O(tick_count^2) per channel, which
                // is invisible at CHECKPOINT_HOT_TICKS = 300 and 4.5 s per segment at 60,000
                // ticks - and §20.2.5's BK_LOG_SEGMENT means an untrusted peer chooses the size.
                if (!first) {
                    i8 pv; u8 pd;
                    ar_action_unword(prev_word, &pv, &pd);
                    for (u32 i = run_start; i < tick; ++i) {
                        frames[i].actions[a].value = pv;
                        frames[i].actions[a].flags = pd;
                    }
                }
                run_start = tick;
                cursor = tick;
                prev_word = word;
                first = false;
            }
            // The last record runs to the end of the segment.
            if (sh.record_count != 0u) {
                i8 pv; u8 pd;
                ar_action_unword(prev_word, &pv, &pd);
                for (u32 i = run_start; i < tick_count; ++i) {
                    frames[i].actions[a].value = pv;
                    frames[i].actions[a].flags = pd;   // edges rebuilt below
                }
            }
        } else if (sh.channel == (u8)ARCHIVE_CH_POINTER_X
                || sh.channel == (u8)ARCHIVE_CH_POINTER_Y) {
            const bool is_x = (sh.channel == (u8)ARCHIVE_CH_POINTER_X);
            if (is_x) { ptr_x_seen = (u8)(ptr_x_seen | (u8)(1u << sh.slot)); }
            else      { ptr_y_seen = (u8)(ptr_y_seen | (u8)(1u << sh.slot)); }
            if (tick_count == 0u) { return ERR_NET_MALFORMED; }
            if (sh.record_count > tick_count) { return ERR_NET_MALFORMED; }
            u32 delta = 0;
            i32 val = 0;
            ErrCode de = wire_get_uvarint(r, &delta);
            if (de != ERR_OK) { return de; }
            de = wire_get_svarint(r, &val);
            if (de != ERR_OK) { return de; }
            if (delta != 0u) { return ERR_NET_MALFORMED; }   // record 0 is the absolute, at tick 0

            i32 p = val;
            i32 v = 0;
            u32 cursor = 0;
            u32 rec = 1;
            u32 next_tick = 0xFFFFFFFFu;
            i32 next_v = 0;
            bool have_next = false;
            for (u32 i = 0; i < tick_count; ++i) {
                if (i > 0u) {
                    if (!have_next && rec < sh.record_count) {
                        u32 d = 0;
                        ErrCode e2 = wire_get_uvarint(r, &d);
                        if (e2 != ERR_OK) { return e2; }
                        e2 = wire_get_svarint(r, &next_v);
                        if (e2 != ERR_OK) { return e2; }
                        if (d == 0u) { return ERR_NET_MALFORMED; }
                        next_tick = cursor + d;
                        if (next_tick >= tick_count) { return ERR_NET_MALFORMED; }
                        cursor = next_tick;
                        ++rec;
                        have_next = true;
                    }
                    if (have_next && i == next_tick) {
                        if (next_v == v) { return ERR_NET_MALFORMED; }
                        v = next_v;
                        have_next = false;
                    }
                    p = wire_wrap_add_i32(p, v);
                }
                if (is_x) { frames[i].pointer_x = p; } else { frames[i].pointer_y = p; }
            }
            if (rec != sh.record_count || have_next) { return ERR_NET_MALFORMED; }
        } else {
            // The escape channel. Every action stream for this slot has already been read
            // (ascending channel order), so the derived edges can be rebuilt now and overwritten.
            if (((flags_rebuilt >> sh.slot) & 1u) == 0u) {
                for (u32 i = 0; i < tick_count; ++i) {
                    for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
                        const u8 down = (u8)(frames[i].actions[a].flags & 1u);
                        const u8 dp = (i == 0u) ? 0u : (u8)(frames[i - 1].actions[a].flags & 1u);
                        frames[i].actions[a].flags = ar_derived_flags(down, dp);
                    }
                }
                flags_rebuilt = (u8)(flags_rebuilt | (u8)(1u << sh.slot));
            }
            if ((u64)sh.record_count > (u64)tick_count * (u64)NET_FRAME_MAX_ACTIONS) {
                return ERR_NET_MALFORMED;
            }
            u32 cursor = 0;
            bool first = true;
            u32 last_action = 0u;
            for (u32 rec = 0; rec < sh.record_count; ++rec) {
                u32 delta = 0, word = 0;
                ErrCode de = wire_get_uvarint(r, &delta);
                if (de != ERR_OK) { return de; }
                de = wire_get_uvarint(r, &word);
                if (de != ERR_OK) { return de; }
                // delta_tick 0 IS legal after the first record on this channel alone: several
                // actions can escape on one tick. Canonicality is then carried by the action
                // index, which must ascend within a tick.
                const u32 tick = first ? delta : (cursor + delta);
                if (tick < cursor || tick >= tick_count) { return ERR_NET_MALFORMED; }
                const u32 action = word >> 3;
                const u8 flags = (u8)(word & 7u);
                if (action >= NET_FRAME_MAX_ACTIONS) { return ERR_NET_MALFORMED; }
                if (!first && tick == cursor && action <= last_action) { return ERR_NET_MALFORMED; }
                if (frames[tick].actions[action].flags == flags) { return ERR_NET_MALFORMED; }
                // The DOWN bit belongs to the action channel; the escape channel carries only
                // the edge bits. Letting an escape change bit 0 lets the same frame set be
                // spelled two ways - state the down bit in the action stream, or omit that
                // stream and assert it here - which forks the chain, since §20.2.8 hashes these
                // bytes. The encoder never emits a disagreeing pair; the decoder now refuses one.
                if ((flags & 1u) != (frames[tick].actions[action].flags & 1u)) {
                    return ERR_NET_MALFORMED;
                }
                frames[tick].actions[action].flags = flags;
                cursor = tick;
                last_action = action;
                first = false;
            }
        }
    }
    if (r->pos != streams_end) { return ERR_NET_MALFORMED; }
    if (seen_records != out_header->record_count) { return ERR_NET_MALFORMED; }
    // A zero-tick segment carries no pointer streams (the encoder skips them - there is no
    // absolute position to state); at any other length both axes are mandatory per slot.
    if (tick_count != 0u
        && (ptr_x_seen != out_header->slot_mask || ptr_y_seen != out_header->slot_mask)) {
        return ERR_NET_MALFORMED;
    }

    // Slots whose streams carried no escapes still need their derived edges built.
    for (u32 s = 0; s < MAX_PEERS; ++s) {
        if (((out_header->slot_mask >> s) & 1u) == 0u) { continue; }
        if (((flags_rebuilt >> s) & 1u) != 0u) { continue; }
        WireFrame* frames = out_frames + (u64)s * tick_count;
        for (u32 i = 0; i < tick_count; ++i) {
            for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
                const u8 down = (u8)(frames[i].actions[a].flags & 1u);
                const u8 dp = (i == 0u) ? 0u : (u8)(frames[i - 1].actions[a].flags & 1u);
                frames[i].actions[a].flags = ar_derived_flags(down, dp);
            }
        }
    }

    for (u32 i = 0; i < out_header->log_record_count; ++i) {
        const ErrCode e = wire_read_LogRecord(r, &out_records[i]);
        if (e != ERR_OK) { return e; }
        const ErrCode lv = wire_check_version(out_records[i].format_version);
        if (lv != ERR_OK) { return lv; }
    }
    *out_record_count = out_header->log_record_count;

    // Every payload byte the header declared must have been consumed: a segment with trailing
    // bytes is not a segment this encoder produced.
    if (r->pos - header_start != (u64)AR_HDR_BYTES + (u64)out_header->payload_bytes) {
        return ERR_NET_MALFORMED;
    }
    return ERR_OK;
}
